// xam.xex content exports: storage devices and content packages (saves).
// Reference: Xenia's src/xenia/kernel/xam/xam_content.cc.
//
// There's one device, the hard drive (id 1). A content package is a host
// directory under the user data root, laid out like the console's:
//   \Device\Harddisk0\Content\<XUID>\<title ID>\<content type>\<file name>
// (XUID 0 for content that isn't per profile). Opening a package links its
// root name ("save:") there.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "config/env.h"
#include "objects.h"
#include "vfs.h"
#include "../memory.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using namespace h3;
using namespace h3::kernel;

namespace
{
    constexpr uint32_t kErrorSuccess = 0;
    constexpr uint32_t kErrorFileNotFound = 0x2;
    constexpr uint32_t kErrorPathNotFound = 0x3;
    constexpr uint32_t kErrorInvalidParameter = 0x57;
    constexpr uint32_t kErrorNoMoreFiles = 0x12;
    constexpr uint32_t kErrorAlreadyExists = 0xB7;
    constexpr uint32_t kErrorFunctionFailed = 0x65B;

    constexpr uint32_t kHardDriveId = 1;
    constexpr uint32_t kTitleId = 0x4D5307E6;  // Halo 3
    constexpr uint32_t kContentTypeSavedGame = 1;
    constexpr uint32_t kContentDataSize = 0x134;

    // H3_LOG_CONTENT=1: log content (save) package calls.
    bool LogContent()
    {
        static const bool log = EnvFlag("H3_LOG_CONTENT");
        return log;
    }

    // XCONTENT_DATA (0x134): device id, content type, WCHAR display name[128],
    // CHAR file name[42], 2 bytes padding.
    struct ContentData
    {
        uint32_t deviceId = 0;
        uint32_t contentType = 0;
        std::string fileName;
        std::u16string displayName;
    };

    // Reads a guest XCONTENT_DATA (layout above) into host form.
    ContentData ReadContentData(uint32_t address)
    {
        ContentData data;
        data.deviceId = GuestRef<uint32_t>(address);
        data.contentType = GuestRef<uint32_t>(address + 4);
        for (uint32_t i = 0; i < 128; i++)
        {
            char16_t c = char16_t(uint16_t(GuestRef<uint16_t>(address + 8 + i * 2)));
            if (c == 0)
                break;
            data.displayName.push_back(c);
        }
        const char* name = GuestPtr<const char>(address + 0x108);
        data.fileName.assign(name, strnlen(name, 42));
        return data;
    }

    // XCONTENT_DATA in guest (big-endian) layout.
    std::vector<uint8_t> EncodeContentData(const ContentData& data)
    {
        std::vector<uint8_t> out(kContentDataSize, 0);
        auto store32 = [&](size_t offset, uint32_t value) {
            for (int i = 0; i < 4; i++)
                out[offset + i] = uint8_t(value >> (24 - 8 * i));
        };
        store32(0, data.deviceId);
        store32(4, data.contentType);
        for (size_t i = 0; i < data.displayName.size() && i < 127; i++)
        {
            out[8 + i * 2] = uint8_t(data.displayName[i] >> 8);
            out[8 + i * 2 + 1] = uint8_t(data.displayName[i]);
        }
        memcpy(out.data() + 0x108, data.fileName.data(), std::min<size_t>(data.fileName.size(), 41));
        return out;
    }

    // value as upper-case hex, zero-padded to `digits` (content folder names use
    // these).
    std::string Hex(uint64_t value, int digits)
    {
        char text[32];
        snprintf(text, sizeof(text), "%0*llX", digits, static_cast<unsigned long long>(value));
        return text;
    }

    // Guest directory holding packages of one type.
    std::string TypeDirectory(uint32_t contentType)
    {
        uint64_t xuid = contentType == kContentTypeSavedGame ? kLocalUserXuid : 0;
        return "\\Device\\Harddisk0\\Content\\" + Hex(xuid, 16) + "\\" + Hex(kTitleId, 8) + "\\" + Hex(contentType, 8);
    }

    // Host directory for a guest directory under the user data root,
    // created if needed.
    std::filesystem::path HostDirectory(const std::string& guestDirectory)
    {
        auto root = MapGuestPath("\\Device\\Harddisk0");
        if (!root)
            return {};
        std::filesystem::path path = root->path;
        std::string relative = guestDirectory.substr(strlen("\\Device\\Harddisk0\\"));
        size_t start = 0;
        while (start < relative.size())
        {
            size_t end = relative.find('\\', start);
            path /= relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
            start = end == std::string::npos ? relative.size() : end + 1;
        }
        std::error_code error;
        std::filesystem::create_directories(path, error);
        return path;
    }

    // XamEnumerate's source: items already in guest layout.
    struct Enumerator : KernelObject
    {
        explicit Enumerator(uint32_t itemSize, uint32_t itemsPerCall)
            : KernelObject(ObjectType::Enumerator), itemSize(itemSize), itemsPerCall(std::max(itemsPerCall, 1u)) {}
        bool IsSignaled(uint32_t) const override { return false; }
        int32_t SignalState() const override { return 0; }

        uint32_t itemSize;
        uint32_t itemsPerCall;
        std::vector<std::vector<uint8_t>> items;
        size_t next = 0;
    };
}

// DWORD XamShowDeviceSelectorUI(DWORD userIndex, DWORD contentType, DWORD contentFlags,
//     ULARGE_INTEGER bytesRequested, PXCONTENTDEVICEID deviceId, PXOVERLAPPED overlapped)
// The hard drive, without asking.
PPC_FUNC(__imp__XamShowDeviceSelectorUI)
{
    if (ctx.r7.u32 != 0)
        GuestRef<uint32_t>(ctx.r7.u32) = kHardDriveId;
    if (LogContent())
        fprintf(stderr, "[xam] XamShowDeviceSelectorUI user %u type %08X flags %08X -> hard drive\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u64 = CompleteOverlapped(ctx.r8.u32, kErrorSuccess);
}

// DWORD XamContentGetDeviceData(DWORD deviceId, PXCONTENTDEVICE_DATA data)
// XCONTENTDEVICE_DATA (0x50): id, type (1 = hard drive), QWORD total and
// free bytes, WCHAR name[28].
PPC_FUNC(__imp__XamContentGetDeviceData)
{
    uint32_t data = ctx.r4.u32;
    if (LogContent())
        fprintf(stderr, "[xam] XamContentGetDeviceData device %u\n", ctx.r3.u32);
    if (ctx.r3.u32 != kHardDriveId || data == 0)
    {
        ctx.r3.u64 = kErrorFileNotFound;
        return;
    }
    memset(GuestPtr<uint8_t>(data), 0, 0x50);
    GuestRef<uint32_t>(data) = kHardDriveId;
    GuestRef<uint32_t>(data + 4) = 1;
    GuestRef<uint64_t>(data + 8) = 20ull << 30;
    GuestRef<uint64_t>(data + 0x10) = 10ull << 30;
    const char* name = "Hard Drive";
    for (uint32_t i = 0; name[i]; i++)
        GuestRef<uint16_t>(data + 0x18 + i * 2) = uint16_t(name[i]);
    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamContentCreateEx(DWORD userIndex, LPCSTR rootName, PXCONTENT_DATA contentData, DWORD flags,
//     PDWORD disposition, PDWORD licenseMask, DWORD cacheSize, ULARGE_INTEGER contentSize, PXOVERLAPPED overlapped)
// Flags (low byte): 1 create new, 2 create always, 3 open existing, 4 open
// always, 5 truncate existing. Disposition out: 1 created, 2 opened.
PPC_FUNC(__imp__XamContentCreateEx)
{
    std::string rootName = ctx.r4.u32 != 0 ? GuestPtr<const char>(ctx.r4.u32) : "";
    ContentData data = ctx.r5.u32 != 0 ? ReadContentData(ctx.r5.u32) : ContentData{};
    uint32_t create = ctx.r6.u32 & 0xFF, dispositionOut = ctx.r7.u32, licenseMask = ctx.r8.u32;
    uint32_t overlapped = StackArgument(ctx, 8);
    if (rootName.empty() || data.fileName.empty())
    {
        ctx.r3.u64 = CompleteOverlapped(overlapped, kErrorInvalidParameter);
        return;
    }

    std::string guestDirectory = TypeDirectory(data.contentType) + "\\" + data.fileName;
    std::filesystem::path typeDirectory = HostDirectory(TypeDirectory(data.contentType));
    std::filesystem::path package = typeDirectory / data.fileName;
    bool exists = std::filesystem::is_directory(package);
    uint32_t result = kErrorSuccess;
    uint32_t disposition = exists ? 2 : 1;
    std::error_code error;
    if ((create == 1 && exists))
        result = kErrorAlreadyExists;
    else if ((create == 3 || create == 5) && !exists)
        result = kErrorPathNotFound;
    else
    {
        if (exists && (create == 2 || create == 5))
        {
            std::filesystem::remove_all(package, error);
            disposition = 1;
            exists = false;
        }
        if (!exists && !std::filesystem::create_directories(package, error))
            result = kErrorFunctionFailed;
    }
    if (result == kErrorSuccess)
    {
        AddSymbolicLink(rootName + ":", guestDirectory);
        if (dispositionOut != 0)
            GuestRef<uint32_t>(dispositionOut) = disposition;
        if (licenseMask != 0)
            GuestRef<uint32_t>(licenseMask) = 0xFFFFFFFF;
    }
    if (LogContent())
        fprintf(stderr, "[xam] XamContentCreateEx %s: type %08X \"%s\" create %u -> %X (%s)\n", rootName.c_str(), data.contentType,
            data.fileName.c_str(), create, result, package.string().c_str());
    ctx.r3.u64 = CompleteOverlapped(overlapped, result);
}

// DWORD XamContentClose(LPCSTR rootName, PXOVERLAPPED overlapped)
PPC_FUNC(__imp__XamContentClose)
{
    std::string rootName = ctx.r3.u32 != 0 ? GuestPtr<const char>(ctx.r3.u32) : "";
    bool removed = RemoveSymbolicLink(rootName + ":");
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, removed ? kErrorSuccess : kErrorPathNotFound);
}

// DWORD XamContentFlush(LPCSTR rootName, PXOVERLAPPED overlapped)
// Writes go straight to host files.
PPC_FUNC(__imp__XamContentFlush)
{
    ctx.r3.u64 = CompleteOverlapped(ctx.r4.u32, kErrorSuccess);
}

// DWORD XamContentDelete(DWORD userIndex, PXCONTENT_DATA contentData, PXOVERLAPPED overlapped)
PPC_FUNC(__imp__XamContentDelete)
{
    uint32_t result = kErrorInvalidParameter;
    if (ctx.r4.u32 != 0)
    {
        ContentData data = ReadContentData(ctx.r4.u32);
        std::filesystem::path package = HostDirectory(TypeDirectory(data.contentType)) / data.fileName;
        std::error_code error;
        result = !data.fileName.empty() && std::filesystem::remove_all(package, error) > 0 ? kErrorSuccess : kErrorFileNotFound;
    }
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, result);
}

// DWORD XamContentCreateEnumerator(DWORD userIndex, DWORD deviceId, DWORD contentType, DWORD flags,
//     DWORD itemsPerEnumerate, PDWORD bufferSize, PHANDLE enumerator)
PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    uint32_t deviceId = ctx.r4.u32, contentType = ctx.r5.u32, items = ctx.r7.u32;
    uint32_t bufferSize = ctx.r8.u32, handleOut = ctx.r9.u32;
    if (handleOut == 0)
    {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    auto enumerator = std::make_shared<Enumerator>(kContentDataSize, items);
    if (deviceId == 0 || deviceId == kHardDriveId)
    {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(HostDirectory(TypeDirectory(contentType)), error))
        {
            if (!entry.is_directory())
                continue;
            ContentData data;
            data.deviceId = kHardDriveId;
            data.contentType = contentType;
            data.fileName = entry.path().filename().string();
            data.displayName.assign(data.fileName.begin(), data.fileName.end());
            enumerator->items.push_back(EncodeContentData(data));
        }
    }
    if (bufferSize != 0)
        GuestRef<uint32_t>(bufferSize) = enumerator->itemsPerCall * kContentDataSize;
    GuestRef<uint32_t>(handleOut) = CreateHandle(enumerator);
    if (LogContent())
        fprintf(stderr, "[xam] XamContentCreateEnumerator type %08X: %zu packages\n", contentType, enumerator->items.size());
    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamEnumerate(HANDLE enumerator, DWORD flags, PVOID buffer, DWORD bufferSize,
//     PDWORD itemsReturned, PXOVERLAPPED overlapped)
PPC_FUNC(__imp__XamEnumerate)
{
    auto enumerator = LookupHandleAs<Enumerator>(ctx.r3.u32);
    uint32_t buffer = ctx.r5.u32, bufferSize = ctx.r6.u32, itemsOut = ctx.r7.u32, overlapped = ctx.r8.u32;
    if (!enumerator || buffer == 0)
    {
        ctx.r3.u64 = CompleteOverlapped(overlapped, kErrorInvalidParameter);
        return;
    }
    uint32_t count = 0;
    uint32_t fits = std::min(bufferSize / enumerator->itemSize, enumerator->itemsPerCall);
    while (count < fits && enumerator->next < enumerator->items.size())
        memcpy(GuestPtr<uint8_t>(buffer + count++ * enumerator->itemSize), enumerator->items[enumerator->next++].data(),
            enumerator->itemSize);
    if (itemsOut != 0)
        GuestRef<uint32_t>(itemsOut) = count;
    // Nothing (left) to return: a plain call gets ERROR_NO_MORE_FILES; an
    // async one fails the XAM way, ERROR_FUNCTION_FAILED with the cause as
    // an HRESULT. The content catalogue only accepts an empty enumeration
    // in that form (sub_82580B30); anything else counts as a failed scan,
    // the catalogue is thrown away, and saving a film fails with "Failed to
    // save the film to your saved game storage".
    if (count == 0)
        ctx.r3.u64 = overlapped != 0
            ? CompleteOverlappedEx(overlapped, kErrorFunctionFailed, HresultFromWin32(kErrorNoMoreFiles))
            : kErrorNoMoreFiles;
    else
        ctx.r3.u64 = CompleteOverlapped(overlapped, kErrorSuccess, count);
}

// DWORD XamUserCreateAchievementEnumerator(DWORD titleId, DWORD userIndex, XUID xuid, DWORD flags,
//     DWORD offset, DWORD count, PDWORD bufferSize, PHANDLE enumerator)
// No achievements: the enumeration is empty. XACHIEVEMENT_DETAILS is 0x24
// bytes (plus strings the caller sizes the buffer for).
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator)
{
    uint32_t count = ctx.r8.u32, bufferSize = ctx.r9.u32, handleOut = ctx.r10.u32;
    if (handleOut == 0)
    {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    auto enumerator = std::make_shared<Enumerator>(0x24, count);
    if (bufferSize != 0)
        GuestRef<uint32_t>(bufferSize) = enumerator->itemsPerCall * 0x24;
    GuestRef<uint32_t>(handleOut) = CreateHandle(enumerator);
    ctx.r3.u64 = kErrorSuccess;
}
