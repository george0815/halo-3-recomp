// Native reader for Halo 3 cache files (maps/*.map, Xbox 360 retail,
// version 11): the header, the four sections, the string id and tag name
// tables, and the tag index (groups, tag instances, global tags), all in
// host layout (native endian, std containers). docs/ROADMAP.md 6.3.
//
// File layout (big-endian; offsets into the 0x3000-byte header):
//
//   0x000 'head'  0x004 version (11)  0x008 file size
//   0x010 tag index header address (virtual, in the tag section)
//   0x014 tag section address  0x018 tag section size
//   0x11C build string ("11855.07.08.20.2317.halo3_ship")
//   0x13C map type (u16: 0 campaign, 1 multiplayer, 2 main menu, 3 shared)
//   0x158 string id count, table size, offset index address, table address
//   0x18C map name  0x1B0 scenario path
//   0x2B4 tag name count, table address, table size, offset index address
//   0x2E8 virtual base: the tag section's address in memory
//   0x2F0 6 partitions (virtual address, size) of the tag section
//   0x46C per-section offset masks (debug, resource, tags, locale)
//   0x47C per-section (address, size)
//   0x2FFC 'foot'
//
// A section address plus its offset mask is a file offset: the debug
// section (string ids, tag names) follows the header, then resources, the
// tag section, and localized strings. The tag section is loaded into memory
// at the virtual base, so tag data addresses are memory addresses: the
// kernel's physical allocator places the map there (kernel/
// xboxkrnl_memory.cpp), and the game uses them as they are.
//
// Tag index header (at the header's tag index address):
//   +0x00 group count  +0x04 group table   +0x08 tag count  +0x0C tag table
//   +0x10 global tag count  +0x14 global tag table  +0x20 checksum  +0x24 'tags'
// Group (16 bytes): tag, parent tag, grandparent tag, name (string id).
// Tag (8 bytes): group index (u16, 0xFFFF = empty slot), salt (u16),
// data address (u32). A tag's datum index is salt << 16 | slot.
// Global tag (8 bytes): group tag, datum index.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace h3::native::cache
{
    // Four-character group tags as the game writes them ('scnr' = 0x73636E72).
    constexpr uint32_t GroupTag(const char (&name)[5])
    {
        return uint32_t(uint8_t(name[0])) << 24 | uint32_t(uint8_t(name[1])) << 16 | uint32_t(uint8_t(name[2])) << 8 |
            uint32_t(uint8_t(name[3]));
    }
    // "scnr" from 0x73636E72.
    std::string GroupTagString(uint32_t tag);

    constexpr uint32_t kNoGroup = 0xFFFFFFFF;    // a group's missing parent
    constexpr uint32_t kNoDatum = 0xFFFFFFFF;    // datum index of nothing
    constexpr uint16_t kEmptySlot = 0xFFFF;      // tag table slot without a tag

    enum class MapType : uint16_t
    {
        Campaign = 0,
        Multiplayer = 1,
        MainMenu = 2,
        Shared = 3,
    };

    enum class Section
    {
        Debug,     // string ids, tag names
        Resource,  // streamed resources (sound, bitmaps, geometry)
        Tags,      // tag definitions, loaded at the virtual base
        Locale,    // localized strings
        Count,
    };

    struct SectionBounds
    {
        uint32_t address = 0;  // within the section's own address space
        uint32_t size = 0;
        uint32_t offsetMask = 0;  // address + mask = file offset

        uint32_t FileOffset(uint32_t sectionAddress) const { return sectionAddress + offsetMask; }
    };

    struct Partition
    {
        uint32_t address = 0;  // virtual
        uint32_t size = 0;
    };

    struct Header
    {
        uint32_t version = 0;
        uint32_t fileSize = 0;
        uint32_t tagIndexAddress = 0;
        std::string build;
        MapType type = MapType::Campaign;
        std::string name;
        std::string scenarioPath;
        uint32_t virtualBase = 0;
        std::array<Partition, 6> partitions{};
        std::array<SectionBounds, size_t(Section::Count)> sections{};
        // Debug-section tables.
        uint32_t stringIdCount = 0, stringTableSize = 0, stringIndexAddress = 0, stringTableAddress = 0;
        uint32_t tagNameCount = 0, tagNameTableAddress = 0, tagNameTableSize = 0, tagNameIndexAddress = 0;
    };

    struct TagGroup
    {
        uint32_t tag = 0;
        uint32_t parent = kNoGroup;
        uint32_t grandparent = kNoGroup;
        uint32_t nameId = 0;  // string id
    };

    struct TagInstance
    {
        uint16_t groupIndex = kEmptySlot;  // into CacheFile::Groups()
        uint16_t salt = 0;
        uint32_t address = 0;  // virtual address of its data (0: not in this map)

        bool Empty() const { return groupIndex == kEmptySlot; }
    };

    struct GlobalTag
    {
        uint32_t group = 0;
        uint32_t datum = kNoDatum;
    };

    struct TagIndex
    {
        uint32_t checksum = 0;
        std::vector<TagGroup> groups;
        std::vector<TagInstance> tags;  // by slot (datum index & 0xFFFF)
        std::vector<GlobalTag> globals;
    };

    class CacheFile
    {
    public:
        // Reads the header, the string and name tables and the tag index.
        // Returns nullptr (and the reason in *error) if path isn't a Halo 3
        // cache file. The file stays open for Read().
        static std::unique_ptr<CacheFile> Open(const std::string& path, std::string* error = nullptr);
        ~CacheFile();

        const Header& GetHeader() const { return m_header; }
        const TagIndex& Index() const { return m_index; }

        // The tag at datum index, or nullptr if the slot is empty or the
        // salt doesn't match (as the game's tag_get checks).
        const TagInstance* Tag(uint32_t datum) const;
        uint32_t Datum(uint32_t slot) const { return uint32_t(m_index.tags[slot].salt) << 16 | slot; }
        // The group, or nullptr.
        const TagGroup* Group(uint32_t groupTag) const;
        // Whether a tag of this group is a groupTag (itself or by its
        // parent or grandparent, as tag_get accepts).
        bool IsA(const TagInstance& tag, uint32_t groupTag) const;

        // The tag's path ("globals\globals"); empty for unnamed slots.
        const std::string& TagName(uint32_t slot) const;
        // A string id's string. Ids carry a namespace set (bits 16-23) and
        // an index (bits 0-15); unknown ones come back as "<set>:<index>".
        std::string StringId(uint32_t id) const;

        // File offset of a tag-section virtual address, if it's in the
        // tag section.
        std::optional<uint32_t> TagFileOffset(uint32_t address) const;
        // Reads size bytes at a tag-section virtual address (raw, big-endian).
        bool ReadTagData(uint32_t address, void* out, size_t size) const;

    private:
        CacheFile() = default;
        bool Load(std::string* error);
        bool ReadAt(uint32_t offset, void* out, size_t size) const;
        // Reads a table of count null-terminated strings: an index of
        // count s32 offsets (-1: none) at indexOffset into a table of
        // tableSize bytes at tableOffset.
        bool ReadStringTable(uint32_t count, uint32_t indexOffset, uint32_t tableOffset, uint32_t tableSize,
            std::vector<std::string>* out) const;

        FILE* m_file = nullptr;
        Header m_header;
        TagIndex m_index;
        std::vector<std::string> m_strings;   // string id table, in file order
        std::vector<std::string> m_tagNames;  // by tag slot
    };
}
