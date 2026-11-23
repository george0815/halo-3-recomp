// h3tags: prints what the native cache file reader (H3Native/cache) finds
// in a Halo 3 map. Usage:
//
//   h3tags <map> header            header fields, sections, partitions
//   h3tags <map> groups            tag groups: tag, parents, name
//   h3tags <map> tags [group]      tags (of a group, parents included):
//                                  datum index, group, address, path
//   h3tags <map> globals           the global tags (scenario, globals, ...)
//   h3tags <map> tag <datum hex>   one tag, as the game's tag_get finds it
//   h3tags <map> bsps              the scenario's structure BSPs, read through
//                                  the generated tag layouts (tools/tagdefs)
//   h3tags <map> convert           converts every tag to host layout (as the
//                                  game's tag cache does) and reports errors
//   h3tags <map> shaders <out dir> writes every compiled Xenon shader blob
//                                  (vtsh, pixl, glvs, glps) as
//                                  <group>_<datum>_<n>.bin

#include "cache/cache_file.h"

#ifdef H3_HAVE_TAG_LAYOUTS
#include "tags/generated/tag_host.h"
#endif

#include <chrono>
#include <cinttypes>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace h3::native::cache;

namespace
{
    const char* const kSectionNames[] = { "debug", "resource", "tags", "locale" };

    uint32_t ParseGroupTag(const char* text)
    {
        char name[5] = "    ";
        memcpy(name, text, std::min<size_t>(4, strlen(text)));
        return GroupTag(name);
    }

    void PrintHeader(const CacheFile& file)
    {
        const Header& h = file.GetHeader();
        printf("version %u, %u bytes, build %s\n", h.version, h.fileSize, h.build.c_str());
        printf("map %s (type %u): %s\n", h.name.c_str(), unsigned(h.type), h.scenarioPath.c_str());
        printf("tag index at %08X, virtual base %08X\n", h.tagIndexAddress, h.virtualBase);
        for (size_t i = 0; i < h.sections.size(); i++)
            printf("section %-8s address %08X size %08X file offset %08X\n", kSectionNames[i], h.sections[i].address,
                h.sections[i].size, h.sections[i].FileOffset(h.sections[i].address));
        for (const Partition& p : h.partitions)
            printf("partition %08X size %08X\n", p.address, p.size);
        printf("%u string ids, %u tag names\n", h.stringIdCount, h.tagNameCount);
        const TagIndex& index = file.Index();
        printf("%zu groups, %zu tag slots, %zu global tags, checksum %08X\n", index.groups.size(), index.tags.size(),
            index.globals.size(), index.checksum);
    }

    void PrintGroups(const CacheFile& file)
    {
        for (const TagGroup& g : file.Index().groups)
            printf("%s %s %s %s\n", GroupTagString(g.tag).c_str(),
                g.parent == kNoGroup ? "----" : GroupTagString(g.parent).c_str(),
                g.grandparent == kNoGroup ? "----" : GroupTagString(g.grandparent).c_str(), file.StringId(g.nameId).c_str());
    }

    void PrintTag(const CacheFile& file, uint32_t slot)
    {
        const TagInstance& t = file.Index().tags[slot];
        printf("%08X %s %08X %s\n", file.Datum(slot), GroupTagString(file.Index().groups[t.groupIndex].tag).c_str(), t.address,
            file.TagName(slot).c_str());
    }

    void PrintTags(const CacheFile& file, const char* group)
    {
        uint32_t groupTag = group ? ParseGroupTag(group) : 0;
        const auto& tags = file.Index().tags;
        for (uint32_t slot = 0; slot < tags.size(); slot++)
            if (!tags[slot].Empty() && (!group || file.IsA(tags[slot], groupTag)))
                PrintTag(file, slot);
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: h3tags <map> header|groups|tags [group]|globals|tag <datum>\n");
        return 2;
    }
    std::string error;
    std::unique_ptr<CacheFile> file = CacheFile::Open(argv[1], &error);
    if (!file)
    {
        fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
        return 1;
    }
    std::string command = argv[2];
    if (command == "header")
        PrintHeader(*file);
    else if (command == "groups")
        PrintGroups(*file);
    else if (command == "tags")
        PrintTags(*file, argc > 3 ? argv[3] : nullptr);
    else if (command == "globals")
    {
        for (const GlobalTag& g : file->Index().globals)
        {
            const TagInstance* tag = file->Tag(g.datum);
            printf("%s %08X %s\n", GroupTagString(g.group).c_str(), g.datum, tag ? file->TagName(g.datum & 0xFFFF).c_str() : "(missing)");
        }
    }
    else if (command == "tag" && argc > 3)
    {
        uint32_t datum = uint32_t(strtoul(argv[3], nullptr, 16));
        if (!file->Tag(datum))
        {
            printf("no tag %08X\n", datum);
            return 1;
        }
        PrintTag(*file, datum & 0xFFFF);
    }
#ifdef H3_HAVE_TAG_LAYOUTS
    else if (command == "bsps")
    {
        namespace layout = h3::native::tags::layout;
        const TagInstance* scenario = nullptr;
        for (const GlobalTag& g : file->Index().globals)
            if (g.group == GroupTag("scnr"))
                scenario = file->Tag(g.datum);
        layout::scenario_struct_definition s;
        if (!scenario || !file->ReadTagData(scenario->address, &s, sizeof(s)))
        {
            fprintf(stderr, "no scenario\n");
            return 1;
        }
        int32_t count = s.structure_bsps.count.get();
        printf("%d structure bsps\n", count);
        for (int32_t i = 0; i < count; i++)
        {
            layout::scenario_structure_bsp_reference_block bsp;
            if (!file->ReadTagData(s.structure_bsps.address.get() + uint32_t(i) * sizeof(bsp), &bsp, sizeof(bsp)))
                break;
            uint32_t datum = bsp.structure_bsp.datum.get();
            printf("  %2d %s %08X %s\n", i, GroupTagString(bsp.structure_bsp.group.get()).c_str(), datum,
                file->Tag(datum) ? file->TagName(datum & 0xFFFF).c_str() : "(none)");
        }
    }
    else if (command == "shaders" && argc > 3)
    {
        namespace layout = h3::native::tags::layout;
        std::string out = argv[3];
        uint32_t written = 0;
        uint64_t bytes = 0;
        const auto& tags = file->Index().tags;
        struct Micro
        {
            uint32_t address;
            std::string group;
            uint32_t datum;
            int32_t index;
        };
        std::vector<Micro> micro;
        auto dumpBlocks = [&](uint32_t slot, const char* group, int32_t count, uint32_t address) {
            for (int32_t i = 0; i < count; i++)
            {
                layout::compiled_vertex_shader_block block;  // same layout as the pixel one
                static_assert(sizeof(layout::compiled_vertex_shader_block) == sizeof(layout::compiled_pixel_shader_block));
                if (!file->ReadTagData(address + uint32_t(i) * sizeof(block), &block, sizeof(block)))
                    break;
                // Cache files keep no Xenon blob: +0x4C points at the game's
                // runtime shader object, whose +0x10 is its microcode.
                if (uint32_t runtime = block.runtime_vertex_shader.get())
                {
                    uint8_t object[20];
                    if (file->ReadTagData(runtime, object, sizeof(object)))
                    {
                        uint32_t code = (uint32_t(object[16]) << 24) | (uint32_t(object[17]) << 16) | (uint32_t(object[18]) << 8) | object[19];
                        if (code)
                            micro.push_back({ code, std::string(group), file->Datum(slot), i });
                    }
                }
                int32_t size = block.xenon_compiled_shader.size.get();
                if (size <= 0 || size > (1 << 20))
                    continue;
                std::vector<uint8_t> data(static_cast<size_t>(size));
                if (!file->ReadTagData(block.xenon_compiled_shader.address.get(), data.data(), data.size()))
                    continue;
                char name[64];
                snprintf(name, sizeof(name), "/%s_%08X_%d.bin", group, file->Datum(slot), i);
                if (FILE* f = fopen((out + name).c_str(), "wb"))
                {
                    fwrite(data.data(), 1, data.size(), f);
                    fclose(f);
                    written++;
                    bytes += data.size();
                }
            }
        };
        for (uint32_t slot = 0; slot < tags.size(); slot++)
        {
            if (tags[slot].Empty() || !tags[slot].address)
                continue;
            std::string group = GroupTagString(file->Index().groups[tags[slot].groupIndex].tag);
            if (group == "vtsh" || group == "pixl")
            {
                layout::vertex_shader_block_struct s;  // the pixel one has the same layout
                static_assert(sizeof(layout::vertex_shader_block_struct) == sizeof(layout::pixel_shader_block_struct));
                if (file->ReadTagData(tags[slot].address, &s, sizeof(s)))
                {
                    dumpBlocks(slot, group.c_str(), s.compiled_shaders.count.get(), s.compiled_shaders.address.get());
                }
            }
            else if (group == "glvs" || group == "glps")
            {
                layout::global_vertex_shader_block_struct s;
                static_assert(sizeof(layout::global_vertex_shader_block_struct) == sizeof(layout::global_pixel_shader_block_struct));
                if (file->ReadTagData(tags[slot].address, &s, sizeof(s)))
                    dumpBlocks(slot, group.c_str(), s.compiled_shaders.count.get(), s.compiled_shaders.address.get());
            }
        }
        // Each microcode runs to the next one's start (or 16 KB): shaders
        // ignore what follows their last instruction.
        std::vector<uint32_t> starts;
        for (const Micro& m : micro)
            starts.push_back(m.address);
        std::sort(starts.begin(), starts.end());
        starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
        uint32_t microWritten = 0;
        for (uint32_t start : starts)
        {
            auto next = std::upper_bound(starts.begin(), starts.end(), start);
            uint32_t size = next == starts.end() ? 16384 : std::min<uint32_t>(*next - start, 16384);
            std::vector<uint8_t> code(size);
            while (size && !file->ReadTagData(start, code.data(), size))
                size -= 4;
            if (!size)
                continue;
            const Micro* first = nullptr;
            for (const Micro& m : micro)
                if (m.address == start && (!first || m.group < first->group))
                    first = &m;
            char name[64];
            snprintf(name, sizeof(name), "/mc_%s_%08X.bin", first->group.c_str(), start);
            if (FILE* f = fopen((out + name).c_str(), "wb"))
            {
                fwrite(code.data(), 1, size, f);
                fclose(f);
                microWritten++;
            }
        }
        printf("%u shader blobs, %" PRIu64 " bytes; %zu microcode references, %u distinct written\n", written, bytes, micro.size(), microWritten);
    }
    else if (command == "convert")
    {
        namespace host = h3::native::tags::host;
        // The whole tag section in memory, read through the converter's callback.
        const Header& h = file->GetHeader();
        struct TagSection
        {
            std::vector<uint8_t> bytes;
            uint32_t base;
        } section{ std::vector<uint8_t>(h.sections[size_t(Section::Tags)].size), h.virtualBase };
        if (!file->ReadTagData(h.virtualBase, section.bytes.data(), section.bytes.size()))
        {
            fprintf(stderr, "can't read the tag section\n");
            return 1;
        }
        auto read = [](void* context, uint32_t address, uint32_t bytes) -> const uint8_t* {
            auto* s = static_cast<TagSection*>(context);
            if (address < s->base || uint64_t(address - s->base) + bytes > s->bytes.size())
                return nullptr;
            return s->bytes.data() + (address - s->base);
        };
        host::Arena arena;
        host::Converter converter(arena, read, &section);
        uint32_t converted = 0, unknown = 0;
        auto start = std::chrono::steady_clock::now();
        const auto& tags = file->Index().tags;
        for (uint32_t slot = 0; slot < tags.size(); slot++)
        {
            if (tags[slot].Empty() || !tags[slot].address)
                continue;
            const host::GroupConverter* g = host::FindGroupConverter(file->Index().groups[tags[slot].groupIndex].tag);
            if (!g)
            {
                unknown++;
                continue;
            }
            g->convert(tags[slot].address, converter);
            converted++;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        printf("%u tags converted (%u of unknown groups) in %.0f ms: %zu MB host, %zu MB tag section, %u errors\n",
            converted, unknown, ms, arena.TotalBytes() >> 20, section.bytes.size() >> 20, converter.Errors());

        // Round trip: every tag written back into a zeroed copy of the
        // section must give the original bytes wherever something was written.
        struct Copy
        {
            std::vector<uint8_t> bytes;
            std::vector<uint32_t> written;  // per byte: 1 + the slot whose write-back wrote it
            uint32_t base, slot;
        } copy{ std::vector<uint8_t>(section.bytes.size()), std::vector<uint32_t>(section.bytes.size()), section.base, 0 };
        auto write = [](void* context, uint32_t address, uint32_t bytes) -> uint8_t* {
            auto* c = static_cast<Copy*>(context);
            if (address < c->base || uint64_t(address - c->base) + bytes > c->bytes.size())
                return nullptr;
            std::fill_n(&c->written[address - c->base], bytes, c->slot + 1);
            return c->bytes.data() + (address - c->base);
        };
        host::Unconverter unconverter(write, &copy);
        host::Arena arena2;
        host::Converter again(arena2, read, &section);
        for (uint32_t slot = 0; slot < tags.size(); slot++)
        {
            if (tags[slot].Empty() || !tags[slot].address)
                continue;
            copy.slot = slot;
            if (const host::GroupConverter* g = host::FindGroupConverter(file->Index().groups[tags[slot].groupIndex].tag))
                g->unconvert(g->convert(tags[slot].address, again), tags[slot].address, unconverter);
        }
        size_t covered = 0, differ = 0, first = SIZE_MAX;
        for (size_t i = 0; i < copy.bytes.size(); i++)
            if (copy.written[i])
            {
                covered++;
                if (copy.bytes[i] != section.bytes[i])
                {
                    differ++;
                    first = std::min(first, i);
                }
            }
        printf("round trip: %zu of %zu bytes written back, %zu differ", covered, section.bytes.size(), differ);
        if (differ)
        {
            uint32_t writer = copy.written[first] - 1;
            printf(" (first at %08zX: %02X, was %02X; written by %s %s at %08X)", section.base + first, copy.bytes[first],
                section.bytes[first], GroupTagString(file->Index().groups[tags[writer].groupIndex].tag).c_str(),
                file->TagName(writer).c_str(), tags[writer].address);
        }
        printf(", %u write errors\n", unconverter.Errors());
        return differ ? 1 : 0;
    }
#endif
    else
    {
        fprintf(stderr, "unknown command %s\n", command.c_str());
        return 2;
    }
    return 0;
}
