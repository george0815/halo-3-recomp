// Native Halo 3 cache file reader (layout: cache_file.h).

#include "cache/cache_file.h"

#include <algorithm>
#include <cstring>

namespace h3::native::cache
{
    namespace
    {
        constexpr uint32_t kHeaderBytes = 0x3000;
        constexpr uint32_t kHeadMagic = GroupTag("head");
        constexpr uint32_t kFootMagic = GroupTag("foot");
        constexpr uint32_t kTagsMagic = GroupTag("tags");
        constexpr uint32_t kVersion = 11;

        // Header field offsets (cache_file.h).
        constexpr uint32_t kHeaderVersion = 0x004;
        constexpr uint32_t kHeaderFileSize = 0x008;
        constexpr uint32_t kHeaderTagIndex = 0x010;
        constexpr uint32_t kHeaderBuild = 0x11C;
        constexpr uint32_t kBuildBytes = 32;
        constexpr uint32_t kHeaderMapType = 0x13C;
        constexpr uint32_t kHeaderStringIds = 0x158;
        constexpr uint32_t kHeaderName = 0x18C;
        constexpr uint32_t kNameBytes = 32;
        constexpr uint32_t kHeaderScenarioPath = 0x1B0;
        constexpr uint32_t kScenarioPathBytes = 256;
        constexpr uint32_t kHeaderTagNames = 0x2B4;
        constexpr uint32_t kHeaderVirtualBase = 0x2E8;
        constexpr uint32_t kHeaderPartitions = 0x2F0;
        constexpr uint32_t kHeaderOffsetMasks = 0x46C;
        constexpr uint32_t kHeaderSections = 0x47C;
        constexpr uint32_t kHeaderFoot = 0x2FFC;

        // Tag index header and table entries.
        constexpr uint32_t kIndexHeaderBytes = 0x28;
        constexpr uint32_t kIndexChecksum = 0x20;
        constexpr uint32_t kIndexMagic = 0x24;
        constexpr uint32_t kGroupBytes = 16;
        constexpr uint32_t kTagBytes = 8;
        constexpr uint32_t kGlobalTagBytes = 8;
        // Sanity limits: a datum index has 16 bits of slot; Halo 3 has ~180 groups.
        constexpr uint32_t kMaxTags = 0x10000;
        constexpr uint32_t kMaxGroups = 0x1000;

        // String id sets (namespaces). The table holds set 0's first
        // kSet0StaticCount strings, then sets 1-8, then the rest of set 0
        // (map-specific strings). kSetStarts: where each set's index 0 is
        // in the table (set 0's rest: index - kSet0StaticCount + start).
        // Found by resolving the tag groups' names; sets 2-8 as documented
        // by Assembly (Blamite H3_StringIDs.xml).
        constexpr uint32_t kStringIdIndexMask = 0xFFFF;
        constexpr uint32_t kStringIdSetShift = 16;
        constexpr uint32_t kStringIdSetMask = 0xFF;
        constexpr uint32_t kSet0StaticCount = 0x4B7;
        constexpr uint32_t kSetStarts[] = { 0xC11, 0x4B7, 0xA7D, 0xB0F, 0xBAF, 0xB63, 0xBBF, 0xBF0, 0xC04 };
        constexpr uint32_t kSetCount = sizeof(kSetStarts) / sizeof(kSetStarts[0]);

        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

        std::string FixedString(const uint8_t* p, size_t bytes)
        {
            const char* text = reinterpret_cast<const char*>(p);
            return std::string(text, strnlen(text, bytes));
        }

        bool Fail(std::string* error, std::string message)
        {
            if (error)
                *error = std::move(message);
            return false;
        }
    }

    std::string GroupTagString(uint32_t tag)
    {
        std::string text(4, ' ');
        for (int i = 0; i < 4; i++)
            text[i] = char(tag >> (24 - 8 * i));
        return text;
    }

    std::unique_ptr<CacheFile> CacheFile::Open(const std::string& path, std::string* error)
    {
        std::unique_ptr<CacheFile> file(new CacheFile());
        file->m_file = fopen(path.c_str(), "rb");
        if (!file->m_file)
        {
            Fail(error, "can't open " + path);
            return nullptr;
        }
        if (!file->Load(error))
            return nullptr;
        return file;
    }

    CacheFile::~CacheFile()
    {
        if (m_file)
            fclose(m_file);
    }

    bool CacheFile::ReadAt(uint32_t offset, void* out, size_t size) const
    {
        return fseek(m_file, long(offset), SEEK_SET) == 0 && fread(out, 1, size, m_file) == size;
    }

    bool CacheFile::ReadStringTable(uint32_t count, uint32_t indexOffset, uint32_t tableOffset, uint32_t tableSize,
        std::vector<std::string>* out) const
    {
        std::vector<uint8_t> index(size_t(count) * 4), table(tableSize);
        if (!ReadAt(indexOffset, index.data(), index.size()) || !ReadAt(tableOffset, table.data(), table.size()))
            return false;
        out->assign(count, std::string());
        for (uint32_t i = 0; i < count; i++)
        {
            int32_t offset = int32_t(Be32(&index[i * 4]));
            if (offset < 0 || uint32_t(offset) >= tableSize)
                continue;
            const char* text = reinterpret_cast<const char*>(&table[offset]);
            (*out)[i].assign(text, strnlen(text, tableSize - offset));
        }
        return true;
    }

    bool CacheFile::Load(std::string* error)
    {
        uint8_t h[kHeaderBytes];
        if (!ReadAt(0, h, sizeof(h)))
            return Fail(error, "file too short for a cache file header");
        if (Be32(h) != kHeadMagic || Be32(h + kHeaderFoot) != kFootMagic)
            return Fail(error, "not a cache file (no head/foot)");
        Header& header = m_header;
        header.version = Be32(h + kHeaderVersion);
        if (header.version != kVersion)
            return Fail(error, "cache file version " + std::to_string(header.version) + ", expected 11 (Halo 3 retail)");
        header.fileSize = Be32(h + kHeaderFileSize);
        header.tagIndexAddress = Be32(h + kHeaderTagIndex);
        header.build = FixedString(h + kHeaderBuild, kBuildBytes);
        header.type = MapType(Be16(h + kHeaderMapType));
        header.name = FixedString(h + kHeaderName, kNameBytes);
        header.scenarioPath = FixedString(h + kHeaderScenarioPath, kScenarioPathBytes);
        header.virtualBase = Be32(h + kHeaderVirtualBase);
        for (size_t i = 0; i < header.partitions.size(); i++)
            header.partitions[i] = { Be32(h + kHeaderPartitions + i * 8), Be32(h + kHeaderPartitions + i * 8 + 4) };
        for (size_t i = 0; i < header.sections.size(); i++)
        {
            header.sections[i].offsetMask = Be32(h + kHeaderOffsetMasks + i * 4);
            header.sections[i].address = Be32(h + kHeaderSections + i * 8);
            header.sections[i].size = Be32(h + kHeaderSections + i * 8 + 4);
        }
        header.stringIdCount = Be32(h + kHeaderStringIds);
        header.stringTableSize = Be32(h + kHeaderStringIds + 4);
        header.stringIndexAddress = Be32(h + kHeaderStringIds + 8);
        header.stringTableAddress = Be32(h + kHeaderStringIds + 12);
        header.tagNameCount = Be32(h + kHeaderTagNames);
        header.tagNameTableAddress = Be32(h + kHeaderTagNames + 4);
        header.tagNameTableSize = Be32(h + kHeaderTagNames + 8);
        header.tagNameIndexAddress = Be32(h + kHeaderTagNames + 12);

        // The shared maps hold resources only: no tag index.
        if (header.tagIndexAddress == 0)
            return true;

        const SectionBounds& debug = header.sections[size_t(Section::Debug)];
        if (!ReadStringTable(header.stringIdCount, debug.FileOffset(header.stringIndexAddress),
                debug.FileOffset(header.stringTableAddress), header.stringTableSize, &m_strings))
            return Fail(error, "can't read the string id table");
        if (!ReadStringTable(header.tagNameCount, debug.FileOffset(header.tagNameIndexAddress),
                debug.FileOffset(header.tagNameTableAddress), header.tagNameTableSize, &m_tagNames))
            return Fail(error, "can't read the tag name table");

        uint8_t ih[kIndexHeaderBytes];
        if (!ReadTagData(header.tagIndexAddress, ih, sizeof(ih)) || Be32(ih + kIndexMagic) != kTagsMagic)
            return Fail(error, "no tag index header ('tags') at the header's tag index address");
        uint32_t groupCount = Be32(ih + 0x00), groupTable = Be32(ih + 0x04);
        uint32_t tagCount = Be32(ih + 0x08), tagTable = Be32(ih + 0x0C);
        uint32_t globalCount = Be32(ih + 0x10), globalTable = Be32(ih + 0x14);
        if (groupCount > kMaxGroups || tagCount > kMaxTags || globalCount > kMaxGroups)
            return Fail(error, "implausible tag index counts");
        m_index.checksum = Be32(ih + kIndexChecksum);

        std::vector<uint8_t> raw(size_t(std::max({ groupCount * kGroupBytes, tagCount * kTagBytes, globalCount * kGlobalTagBytes })));
        if (!ReadTagData(groupTable, raw.data(), groupCount * kGroupBytes))
            return Fail(error, "can't read the tag group table");
        for (uint32_t i = 0; i < groupCount; i++)
        {
            const uint8_t* g = &raw[i * kGroupBytes];
            m_index.groups.push_back({ Be32(g), Be32(g + 4), Be32(g + 8), Be32(g + 12) });
        }
        if (!ReadTagData(tagTable, raw.data(), tagCount * kTagBytes))
            return Fail(error, "can't read the tag table");
        for (uint32_t i = 0; i < tagCount; i++)
        {
            const uint8_t* t = &raw[i * kTagBytes];
            m_index.tags.push_back({ Be16(t), Be16(t + 2), Be32(t + 4) });
        }
        if (!ReadTagData(globalTable, raw.data(), globalCount * kGlobalTagBytes))
            return Fail(error, "can't read the global tag table");
        for (uint32_t i = 0; i < globalCount; i++)
        {
            const uint8_t* g = &raw[i * kGlobalTagBytes];
            m_index.globals.push_back({ Be32(g), Be32(g + 4) });
        }
        return true;
    }

    const TagInstance* CacheFile::Tag(uint32_t datum) const
    {
        uint32_t slot = datum & 0xFFFF;
        if (datum == kNoDatum || slot >= m_index.tags.size())
            return nullptr;
        const TagInstance& tag = m_index.tags[slot];
        if (tag.Empty() || tag.salt != uint16_t(datum >> 16))
            return nullptr;
        return &tag;
    }

    const TagGroup* CacheFile::Group(uint32_t groupTag) const
    {
        for (const TagGroup& group : m_index.groups)
            if (group.tag == groupTag)
                return &group;
        return nullptr;
    }

    bool CacheFile::IsA(const TagInstance& tag, uint32_t groupTag) const
    {
        if (tag.groupIndex >= m_index.groups.size())
            return false;
        const TagGroup& group = m_index.groups[tag.groupIndex];
        return group.tag == groupTag || group.parent == groupTag || group.grandparent == groupTag;
    }

    const std::string& CacheFile::TagName(uint32_t slot) const
    {
        static const std::string kNone;
        return slot < m_tagNames.size() ? m_tagNames[slot] : kNone;
    }

    std::string CacheFile::StringId(uint32_t id) const
    {
        uint32_t index = id & kStringIdIndexMask;
        uint32_t set = (id >> kStringIdSetShift) & kStringIdSetMask;
        uint32_t entry;
        if (set == 0)
            entry = index < kSet0StaticCount ? index : index - kSet0StaticCount + kSetStarts[0];
        else if (set < kSetCount)
            entry = index + kSetStarts[set];
        else
            entry = UINT32_MAX;
        return entry < m_strings.size() ? m_strings[entry] : std::to_string(set) + ":" + std::to_string(index);
    }

    std::optional<uint32_t> CacheFile::TagFileOffset(uint32_t address) const
    {
        const SectionBounds& tags = m_header.sections[size_t(Section::Tags)];
        if (address < m_header.virtualBase || address - m_header.virtualBase >= tags.size)
            return std::nullopt;
        return tags.FileOffset(tags.address) + (address - m_header.virtualBase);
    }

    bool CacheFile::ReadTagData(uint32_t address, void* out, size_t size) const
    {
        std::optional<uint32_t> offset = TagFileOffset(address);
        const SectionBounds& tags = m_header.sections[size_t(Section::Tags)];
        if (!offset || address - m_header.virtualBase + size > tags.size)
            return false;
        return ReadAt(*offset, out, size);
    }
}
