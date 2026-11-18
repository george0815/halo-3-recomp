// Reading Xbox 360 disc images (XDVDFS): full XGD2 dumps (video partition
// plus game partition), XGD1/XGD3 layouts, and plain XISOs.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace h3setup
{
    struct IsoEntry
    {
        std::string path;  // "/maps/mainmenu.map" ('/' separated, from the root)
        uint64_t offset = 0;  // byte offset of the data in the image
        uint64_t size = 0;
        bool directory = false;
    };

    class Iso
    {
    public:
        // Opens the image and reads its directory tree. False (with error
        // set) if it isn't a readable XDVDFS image.
        bool Open(const std::filesystem::path& path);

        const std::vector<IsoEntry>& Entries() const { return m_entries; }
        const IsoEntry* Find(const std::string& path) const;  // case-insensitive
        const std::string& Error() const { return m_error; }

        // Copies a file out of the image. progress(bytes) is called as data
        // is written (total bytes of this file so far).
        bool Extract(const IsoEntry& entry, const std::filesystem::path& destination,
            const std::function<void(uint64_t)>& progress = {});

        // Reads a whole (small) file into memory.
        bool Read(const IsoEntry& entry, std::vector<uint8_t>& data);

    private:
        bool ReadAt(uint64_t offset, void* buffer, size_t size);
        bool Walk(uint32_t sector, uint32_t size, const std::string& path, int depth);

        std::ifstream m_file;
        uint64_t m_fileSize = 0;
        uint64_t m_base = 0;  // the game partition's start
        std::vector<IsoEntry> m_entries;
        std::string m_error;
    };
}
