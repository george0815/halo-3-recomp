// Title module loader: the executable and the DLLs it loads with
// XexLoadImage. Reference: Xenia's user_module.cc, xex_module.cc and
// kernel_state.cc (LoadUserModule).
//
// Every module's code was recompiled ahead of time (H3RecompLib/ppc and
// H3RecompLib/modules), so loading one means copying its image into guest
// memory, registering its functions in the shared lookup table, patching its
// data imports and running its entry point.

#include <ppc_recomp_shared.h>

#include "modules.h"
#include "imports.h"
#include "module.h"
#include "variables.h"
#include "vfs.h"

#include "../memory.h"

#include <file.h>
#include <image.h>
#include <xex.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace h3::kernel
{
    namespace
    {
        constexpr uint32_t kDllProcessAttach = 1;

        // XEX header fields.
        constexpr uint32_t kXexHeaderSize = 0x08;
        constexpr uint32_t kXexSecurityInfo = 0x10;
        constexpr uint32_t kSecurityExportTable = 0x160;

        // xex2_export_table: export i (from `base`) is at
        // (imageBase << 16) + offsets[i - base]; 0 means no export.
        constexpr uint32_t kExportImageBase = 0x20;
        constexpr uint32_t kExportCount = 0x24;
        constexpr uint32_t kExportBase = 0x28;
        constexpr uint32_t kExportOffsets = 0x2C;

        // The loader lock. Recursive: resolving a DLL's imports can load
        // another DLL.
        std::recursive_mutex g_mutex;
        std::vector<std::unique_ptr<Module>> g_modules;  // [0] is the executable

        // The whole image is mapped, not just the PE sections: data can sit
        // past a section's virtual size (L360's XML resources overrun
        // .edata). XenonUtils' buffer can be shorter than the image (trailing
        // BSS); guest memory starts zeroed, so copying what's there is enough.
        void CopyImage(const Image& image)
        {
            memcpy(g_base + image.base, image.data.get(), std::min<size_t>(image.dataSize, image.size));
        }

        // Fills the recompiler's function lookup table (guest address -> host function)
        // from a module's mapping list, so indirect calls (function pointers, vtables)
        // find the recompiled code.
        void RegisterFunctions(PPCFuncMapping* mappings)
        {
            for (PPCFuncMapping* mapping = mappings; mapping->host != nullptr; mapping++)
                PPC_LOOKUP_FUNC(g_base, mapping->guest) = mapping->host;
        }

        // 0x822010D0 is a one-instruction thunk, `b 0x82200E38`, that is also
        // a jump table case of the function before it, so that function's
        // boundaries (halo3.toml) take it in and it gets no function of its
        // own. A function pointer table calls it directly (at the end of the
        // opening cinematic).
        constexpr uint32_t kThunkAddress = 0x822010D0;
        void Thunk_822010D0(PPCContext& ctx, uint8_t* base)
        {
            sub_82200E38(ctx, base);
        }

        // Fills the executable's empty function table slots, so an indirect
        // call to code that wasn't recompiled reports where it went instead
        // of jumping to address 0. Indirect calls go through CTR.
        void MissingFunction(PPCContext& ctx, uint8_t*)
        {
            fprintf(stderr, "[loader] indirect call to 0x%08X, which wasn't recompiled as a function (lr=0x%08X)\n",
                ctx.ctr.u32, uint32_t(ctx.lr));
            GuestFatal("call to code that wasn't recompiled", ctx);
        }

        // Registers default.xex's functions, plus the thunk above, and points every
        // other code address at MissingFunction so a bad indirect call reports itself
        // instead of crashing.
        void RegisterExecutableFunctions()
        {
            RegisterFunctions(PPCFuncMappings);
            PPC_LOOKUP_FUNC(g_base, kThunkAddress) = Thunk_822010D0;
            for (uint64_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
                if (PPC_LOOKUP_FUNC(g_base, address) == nullptr)
                    PPC_LOOKUP_FUNC(g_base, address) = MissingFunction;
        }

        // X_LDR_DATA_TABLE_ENTRY, with a guest copy of the XEX headers it
        // points at. Only the fields a title is likely to read are filled in.
        uint32_t CreateLdrEntry(const Module& module)
        {
            uint32_t headers = SystemAlloc(uint32_t(module.headers.size()));
            memcpy(GuestPtr<uint8_t>(headers), module.headers.data(), module.headers.size());

            uint32_t entry = SystemAlloc(0x80);
            GuestRef<uint32_t>(entry + 0x18) = module.base;        // dll_base
            GuestRef<uint32_t>(entry + 0x1C) = module.base;        // image_base
            GuestRef<uint32_t>(entry + 0x20) = module.size;        // image_size
            GuestRef<uint32_t>(entry + 0x3C) = module.entryPoint;  // entry_point
            GuestRef<uint16_t>(entry + 0x40) = 1;                  // load_count
            GuestRef<uint32_t>(entry + 0x58) = headers;            // xex_header_base
            return entry;
        }

        // Builds the kernel's record of a loaded module: name, image range, entry
        // point, a copy of its XEX headers (exports and imports are read from them
        // later) and its loader entry.
        std::unique_ptr<Module> CreateModule(std::string name, const Image& image, const std::vector<uint8_t>& file)
        {
            auto module = std::make_unique<Module>();
            module->name = std::move(name);
            module->base = uint32_t(image.base);
            module->size = image.size;
            module->entryPoint = uint32_t(image.entry_point);

            uint32_t headerSize = reinterpret_cast<const be<uint32_t>*>(file.data() + kXexHeaderSize)->get();
            module->headers.assign(file.begin(), file.begin() + std::min<size_t>(headerSize, file.size()));

            uint32_t security = reinterpret_cast<const be<uint32_t>*>(file.data() + kXexSecurityInfo)->get();
            module->exportTable = reinterpret_cast<const be<uint32_t>*>(file.data() + security + kSecurityExportTable)->get();

            module->ldrEntry = CreateLdrEntry(*module);
            return module;
        }

        // The loaded module `name`, loading it from game:\ first if needed (when ctx
        // allows running its entry point). Null if it can't be found.
        Module* LoadDependency(std::string_view name, PPCContext* ctx)
        {
            if (Module* module = FindModule(name))
                return module;
            if (ctx == nullptr)
                return nullptr;
            return LoadModule("game:\\" + std::string(name), *ctx);
        }

        // Patches the module's import slots that need runtime addresses:
        // xboxkrnl data exports, and every slot importing from another title
        // module. ctx is used to run the entry points of DLLs this one
        // depends on; it's null for the executable.
        bool ResolveImports(const Image& image, const Module& module, PPCContext* ctx)
        {
            auto* header = static_cast<const Xex2ImportHeader*>(getOptHeaderPtr(module.headers.data(), XEX_HEADER_IMPORT_LIBRARIES));
            if (header == nullptr)
                return true;

            std::vector<std::string_view> names;
            auto* strings = reinterpret_cast<const char*>(header + 1);
            for (size_t offset = 0; names.size() < header->numImports;)
            {
                names.emplace_back(strings + offset);
                offset += (names.back().size() + 1 + 3) & ~size_t(3);
            }

            auto inCode = [&](uint32_t address)
            {
                return std::ranges::any_of(image.sections, [&](const auto& section)
                {
                    return (section.flags & SectionFlags_Code) && address >= section.base && address < section.base + section.size;
                });
            };

            auto* library = reinterpret_cast<const Xex2ImportLibrary*>(strings + header->sizeOfStringTable);
            for (std::string_view name : names)
            {
                auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);
                bool kernel = name == "xboxkrnl.exe";
                bool xam = name == "xam.xex";
                Module* dependency = nullptr;
                if (!kernel && !xam)
                {
                    dependency = LoadDependency(name, ctx);
                    if (dependency == nullptr)
                    {
                        fprintf(stderr, "[loader] %s imports from %.*s, which couldn't be loaded\n", module.name.c_str(),
                            int(name.size()), name.data());
                        return false;
                    }
                }

                for (uint32_t i = 0; i < library->numberOfImports; i++)
                {
                    // Function thunks live in code; XenonUtils turned them
                    // into calls to the __imp__ functions. The rest are
                    // import slots (type 0 records). XenonUtils byte-swapped
                    // every record in place, so a native read gets the
                    // original big-endian record: type in the top byte,
                    // ordinal in the low 16 bits.
                    uint32_t slot = descriptors[i].firstThunk;
                    if (inCode(slot))
                        continue;
                    uint32_t record = *GuestPtr<uint32_t>(slot);
                    if ((record >> 24) != 0)
                        continue;
                    uint16_t ordinal = record & 0xFFFF;

                    // Kernel function slots stay as they are (the game calls
                    // the thunks); only kernel variables get patched.
                    uint32_t address = 0;
                    if (kernel)
                        address = KernelVariable(ordinal);
                    else if (dependency != nullptr)
                        address = GetProcAddress(*dependency, ordinal);
                    if (address != 0)
                        GuestRef<uint32_t>(slot) = address;
                }
                library = reinterpret_cast<const Xex2ImportLibrary*>(descriptors + library->numberOfImports);
            }
            return true;
        }

        // The recompiled code for a DLL image, matched by base address and size (each
        // DLL was recompiled for the base it loads at; see
        // H3RecompLib/config/modules/).
        const RecompiledModule* FindRecompiledModule(const Image& image)
        {
            for (const RecompiledModule* entry = kRecompiledModules; entry->name != nullptr; entry++)
            {
                if (entry->imageBase == image.base && entry->imageSize == image.size)
                    return entry;
            }
            return nullptr;
        }
    }

    bool LoadExecutable(const char* path)
    {
        std::lock_guard lock(g_mutex);

        std::vector<uint8_t> file = LoadFile(path);
        if (file.empty())
        {
            fprintf(stderr, "Failed to read %s\n", path);
            return false;
        }

        Image image = Image::ParseImage(file.data(), file.size());
        if (image.data == nullptr)
        {
            fprintf(stderr, "Failed to parse %s\n", path);
            return false;
        }

        // The recompiled code hardcodes addresses, so the XEX must be the
        // exact one that was fed to XenonRecomp.
        if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
        {
            fprintf(stderr, "%s does not match the recompiled executable "
                "(base 0x%zX size 0x%X, expected base 0x%llX size 0x%llX)\n",
                path, image.base, image.size, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
            return false;
        }

        LoadModuleInfo(file.data());
        CopyImage(image);
        RegisterExecutableFunctions();

        g_modules.push_back(CreateModule(std::filesystem::path(path).filename().string(), image, file));
        return ResolveImports(image, *g_modules.back(), nullptr);
    }

    const Module& ExecutableModule()
    {
        return *g_modules.front();
    }

    Module* LoadModule(std::string_view guestPath, PPCContext& ctx)
    {
        std::lock_guard lock(g_mutex);

        std::string_view name = guestPath.substr(guestPath.find_last_of("\\:") + 1);
        if (Module* module = FindModule(name))
        {
            module->loadCount++;
            return module;
        }

        auto path = ResolveGuestPath(guestPath);
        if (!path)
        {
            fprintf(stderr, "[loader] %.*s not found\n", int(guestPath.size()), guestPath.data());
            return nullptr;
        }

        std::vector<uint8_t> file = LoadFile(*path);
        Image image = Image::ParseImage(file.data(), file.size());
        if (image.data == nullptr)
        {
            fprintf(stderr, "[loader] failed to parse %s\n", path->string().c_str());
            return nullptr;
        }

        const RecompiledModule* recompiled = FindRecompiledModule(image);
        if (recompiled == nullptr)
        {
            fprintf(stderr, "[loader] %s (base 0x%zX size 0x%X) wasn't recompiled; "
                "add a config for it in H3RecompLib/config/modules/\n", path->string().c_str(), image.base, image.size);
            return nullptr;
        }

        // All modules share the executable's function table (see
        // H3RecompLib/CMakeLists.txt), which puts a DLL's slots at
        // table + (address - code base) * 2. They must land in the free
        // range between the executable's table and physical memory.
        uint64_t firstSlot = PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (uint64_t(image.base) - PPC_CODE_BASE) * 2;
        uint64_t endSlot = firstSlot + uint64_t(image.size) * 2;
        if (image.base < PPC_IMAGE_BASE + PPC_IMAGE_SIZE || endSlot > kPhysicalMemoryBase)
        {
            fprintf(stderr, "[loader] %s at 0x%zX: its function table slots (0x%llX-0x%llX) overlap guest memory\n",
                path->string().c_str(), image.base, (unsigned long long)firstSlot, (unsigned long long)endSlot);
            return nullptr;
        }

        CopyImage(image);
        RegisterFunctions(recompiled->functions);

        g_modules.push_back(CreateModule(path->filename().string(), image, file));
        Module* module = g_modules.back().get();
        if (!ResolveImports(image, *module, &ctx))
            return nullptr;

        // DllMain(handle, DLL_PROCESS_ATTACH, reserved), on the loading
        // thread like the real loader. Run on a copy of the context so the
        // caller's registers survive.
        if (module->entryPoint != 0)
        {
            PPCContext entryCtx = ctx;
            entryCtx.r3.u64 = module->ldrEntry;
            entryCtx.r4.u64 = kDllProcessAttach;
            entryCtx.r5.u64 = 0;
            (PPC_LOOKUP_FUNC(g_base, module->entryPoint))(entryCtx, g_base);
            if (entryCtx.r3.u32 == 0)
                fprintf(stderr, "[loader] %s: DllMain failed\n", module->name.c_str());
        }

        fprintf(stderr, "[loader] loaded %s at 0x%08X\n", module->name.c_str(), module->base);
        return module;
    }

    Module* ModuleFromHandle(uint32_t handle)
    {
        std::lock_guard lock(g_mutex);
        for (auto& module : g_modules)
        {
            if (module->ldrEntry == handle)
                return module.get();
        }
        return nullptr;
    }

    Module* FindModule(std::string_view name)
    {
        std::lock_guard lock(g_mutex);
        for (auto& module : g_modules)
        {
            if (EqualsIgnoreCase(module->name, name))
                return module.get();
        }
        return nullptr;
    }

    uint32_t GetProcAddress(const Module& module, uint32_t ordinal)
    {
        uint32_t table = module.exportTable;
        if (table == 0)
            return 0;

        uint32_t base = GuestRef<uint32_t>(table + kExportBase);
        uint32_t count = GuestRef<uint32_t>(table + kExportCount);
        if (ordinal < base || ordinal - base >= count)
            return 0;

        uint32_t offset = GuestRef<uint32_t>(table + kExportOffsets + (ordinal - base) * 4);
        if (offset == 0)
            return 0;
        return (GuestRef<uint32_t>(table + kExportImageBase) << 16) + offset;
    }

    void CallModuleImport(const char* library, uint32_t ordinal, PPCContext& ctx, uint8_t* base)
    {
        Module* module = FindModule(library);
        uint32_t target = module != nullptr ? GetProcAddress(*module, ordinal) : 0;
        PPCFunc* function = target != 0 ? PPC_LOOKUP_FUNC(base, target) : nullptr;
        if (function == nullptr)
        {
            fprintf(stderr, "[loader] unresolved import %s @%u (target 0x%08X)\n", library, ordinal, target);
            GuestFatal("unresolved module import", ctx);
        }
        function(ctx, base);
    }
}
