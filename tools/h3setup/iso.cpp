// XDVDFS reader (iso.h). Port of tools/xdvdfs_extract.py.
//
// The game partition starts at one of a few fixed offsets depending on the
// disc layout; its volume descriptor sits 32 sectors in and starts with
// "MICROSOFT*XBOX*MEDIA", followed by the root directory's sector and size.
// A directory is a binary tree of entries: left and right subtree offsets
// (in 4-byte units, 0 = none), the first sector and size of the data, the
// attributes (0x10 = directory) and the name. 0xFFFF/0xFFFF is padding.

#include "iso.h"
#include "build.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

namespace h3setup
{
    namespace
    {
        constexpr uint64_t kSector = 2048;
        constexpr uint64_t kPartitions[] = { 0xFD90000, 0x18300000, 0x2080000, 0 };  // XGD2, XGD1, XGD3, plain XISO
        constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

        // XDVDFS stores numbers little-endian.
        uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
        uint32_t Le32(const uint8_t* p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }

        // A name from the disc that is safe to use as one path component.
        bool SafeName(const std::string& name)
        {
            if (name.empty() || name == "." || name == "..")
                return false;
            for (unsigned char c : name)
                if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                    c == '>' || c == '|')
                    return false;
            return true;
        }

        // ASCII lower case: disc paths compare case-insensitively (XDVDFS is).
        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        }
    }

    // size bytes at an image offset; false past the end of the file.
    bool Iso::ReadAt(uint64_t offset, void* buffer, size_t size)
    {
        if (offset + size > m_fileSize)
            return false;
        m_file.clear();
        m_file.seekg(std::streamoff(offset));
        m_file.read(static_cast<char*>(buffer), std::streamsize(size));
        return size_t(m_file.gcount()) == size;
    }

    // Finds the XDVDFS volume (trying the disc layouts' offsets: XGD1/2/3
    // and a plain XISO), checks its magic, and walks the directory tree.
    bool Iso::Open(const std::filesystem::path& path)
    {
        m_entries.clear();
        m_file.open(path, std::ios::binary);
        if (!m_file)
        {
            m_error = "can't open " + h3setup::Utf8(path);
            return false;
        }
        m_file.seekg(0, std::ios::end);
        m_fileSize = uint64_t(m_file.tellg());

        for (uint64_t base : kPartitions)
        {
            uint8_t descriptor[28];
            if (ReadAt(base + 32 * kSector, descriptor, sizeof(descriptor)) && memcmp(descriptor, kMagic, 20) == 0)
            {
                m_base = base;
                if (!Walk(Le32(descriptor + 20), Le32(descriptor + 24), "", 0))
                    return false;
                std::sort(m_entries.begin(), m_entries.end(), [](const IsoEntry& a, const IsoEntry& b) { return a.path < b.path; });
                return true;
            }
        }
        m_error = "not an Xbox 360 disc image (no XDVDFS volume found)";
        return false;
    }

    // Adds a directory's entries (and, recursively, its subdirectories') to
    // m_entries. An XDVDFS directory is a binary tree in one extent: each
    // entry is left and right subtree offsets (in dwords; 0xFFFF both =
    // padding), start sector, length, attributes (0x10 = directory), name
    // length, name. Visited offsets are remembered and the depth is capped,
    // so a damaged or hostile image can't loop. Fails on file names that
    // could escape the install folder and on files past the image's end.
    bool Iso::Walk(uint32_t sector, uint32_t size, const std::string& path, int depth)
    {
        if (depth > 32 || size > (64u << 20))
        {
            m_error = "damaged directory tree at " + path;
            return false;
        }
        std::vector<uint8_t> data(size);
        if (!ReadAt(m_base + sector * kSector, data.data(), size))
        {
            m_error = "can't read the directory " + (path.empty() ? std::string("/") : path);
            return false;
        }
        std::vector<uint32_t> stack{ 0 };
        std::set<uint32_t> seen;
        while (!stack.empty())
        {
            uint32_t offset = stack.back();
            stack.pop_back();
            if (!seen.insert(offset).second || offset + 14 > size)
                continue;
            const uint8_t* entry = data.data() + offset;
            uint16_t left = Le16(entry), right = Le16(entry + 2);
            if (left == 0xFFFF && right == 0xFFFF)
                continue;
            uint32_t start = Le32(entry + 4), length = Le32(entry + 8);
            uint8_t attributes = entry[12], nameLength = entry[13];
            if (offset + 14 + nameLength > size)
                continue;
            std::string name(reinterpret_cast<const char*>(entry + 14), nameLength);
            if (left)
                stack.push_back(uint32_t(left) * 4);
            if (right)
                stack.push_back(uint32_t(right) * 4);
            if (!SafeName(name))
            {
                m_error = "unsafe file name on the disc: " + name;
                return false;
            }
            IsoEntry item{ path + "/" + name, m_base + uint64_t(start) * kSector, length, (attributes & 0x10) != 0 };
            if (!item.directory && item.offset + item.size > m_fileSize)
            {
                m_error = "the image is cut short (" + item.path + " runs past its end)";
                return false;
            }
            m_entries.push_back(item);
            if (item.directory && length && !Walk(start, length, item.path, depth + 1))
                return false;
        }
        return true;
    }

    // A file or folder by its path on the disc, any case.
    const IsoEntry* Iso::Find(const std::string& path) const
    {
        std::string wanted = Lower(path);
        for (const IsoEntry& entry : m_entries)
            if (Lower(entry.path) == wanted)
                return &entry;
        return nullptr;
    }

    // A whole (small) file in memory: default.xex for the checksum.
    bool Iso::Read(const IsoEntry& entry, std::vector<uint8_t>& data)
    {
        data.resize(entry.size);
        return ReadAt(entry.offset, data.data(), data.size());
    }

    // Copies one file (or creates one folder) at destination, in 8 MB
    // chunks, reporting progress. (Skipping files an earlier, interrupted
    // run already copied is the caller's job: main.cpp's Extract.)
    bool Iso::Extract(const IsoEntry& entry, const std::filesystem::path& destination,
        const std::function<void(uint64_t)>& progress)
    {
        std::error_code ec;
        if (entry.directory)
            return std::filesystem::create_directories(destination, ec) || std::filesystem::is_directory(destination);
        std::filesystem::create_directories(destination.parent_path(), ec);
        std::ofstream out(destination, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            m_error = "can't write " + h3setup::Utf8(destination);
            return false;
        }
        std::vector<char> buffer(8u << 20);
        uint64_t done = 0;
        m_file.clear();
        m_file.seekg(std::streamoff(entry.offset));
        while (done < entry.size)
        {
            size_t chunk = size_t(std::min<uint64_t>(buffer.size(), entry.size - done));
            m_file.read(buffer.data(), std::streamsize(chunk));
            if (size_t(m_file.gcount()) != chunk)
            {
                m_error = "read error in " + entry.path;
                return false;
            }
            out.write(buffer.data(), std::streamsize(chunk));
            if (!out)
            {
                m_error = "write error (disk full?) at " + h3setup::Utf8(destination);
                return false;
            }
            done += chunk;
            if (progress)
                progress(done);
        }
        return true;
    }
}
