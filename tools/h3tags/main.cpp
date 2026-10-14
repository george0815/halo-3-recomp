// h3tags: prints what the native cache file reader (H3Native/cache) finds
// in a Halo 3 map. Usage:
//
//   h3tags <map> header            header fields, sections, partitions
//   h3tags <map> groups            tag groups: tag, parents, name
//   h3tags <map> tags [group]      tags (of a group, parents included):
//                                  datum index, group, address, path
//   h3tags <map> globals           the global tags (scenario, globals, ...)
//   h3tags <map> tag <datum hex>   one tag, as the game's tag_get finds it

#include "cache/cache_file.h"

#include <cinttypes>
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
    else
    {
        fprintf(stderr, "unknown command %s\n", command.c_str());
        return 2;
    }
    return 0;
}
