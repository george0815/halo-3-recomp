// Field types of tags in host layout (native endian, host pointers), as
// the generated host structs (generated/tag_host.h) use them, and the
// converter that fills them from the cache file layout (tag_types.h).
// docs/ROADMAP.md 6.3, native milestone A.
//
// Host structs keep, next to each block's and data field's host pointer,
// the guest address it came from: decompiled code that reads host tags can
// still hand the guest address to recompiled callers.

#pragma once

#include "tags/tag_types.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

namespace h3::native::tags::host
{
    constexpr uint32_t kNone = 0xFFFFFFFF;  // datum index / group of nothing

    // A tag block: count elements of T.
    template <typename T>
    struct Block
    {
        T* elements = nullptr;
        int32_t count = 0;
        uint32_t address = 0;      // guest address of the elements
        int32_t storedCount = 0;   // the cache file's count (count is 0 when the
                                   // elements aren't readable: filled in at runtime)

        // Range-for support: `for (auto& element : block)`.
        T* begin() const { return elements; }
        T* end() const { return elements + count; }
        T& operator[](int32_t i) const { return elements[i]; }
        // No elements (or none readable: see storedCount).
        bool empty() const { return count == 0; }
    };

    // A reference to another tag.
    struct TagReference
    {
        uint32_t group = kNone;
        uint32_t datum = kNone;
    };

    // Raw bytes owned by the tag (contents as stored: big-endian if they
    // hold numbers).
    struct TagData
    {
        const uint8_t* bytes = nullptr;
        int32_t size = 0;
        uint32_t flags = 0;
        uint32_t address = 0;
    };

    // A streamed resource: its index in the map's resource gestalt.
    struct PageableResource
    {
        uint32_t handle = kNone;
    };

    // Bump allocator for converted tags: memory lives until the arena goes
    // (a map unload). Not thread-safe; the tag cache serializes access.
    class Arena
    {
    public:
        // bytes of memory at the given alignment: from the current chunk, or a
        // new chunk (4 MB, or bigger for a bigger request) when it's full.
        void* Allocate(size_t bytes, size_t alignment)
        {
            m_used = (m_used + alignment - 1) & ~(alignment - 1);
            if (m_chunks.empty() || m_used + bytes > m_chunkBytes)
            {
                m_chunkBytes = std::max(kChunkBytes, bytes + alignment);
                m_chunks.push_back(std::make_unique<uint8_t[]>(m_chunkBytes));
                m_used = 0;
            }
            void* p = m_chunks.back().get() + m_used;
            m_used += bytes;
            m_total += bytes;
            return p;
        }
        // count value-initialized Ts in the arena (freed with it, never one by one).
        template <typename T>
        T* New(size_t count = 1)
        {
            T* p = static_cast<T*>(Allocate(sizeof(T) * count, alignof(T)));
            for (size_t i = 0; i < count; i++)
                new (p + i) T();
            return p;
        }
        // Bytes handed out so far (for the tag cache's size report).
        size_t TotalBytes() const { return m_total; }

    private:
        static constexpr size_t kChunkBytes = 4 << 20;
        std::vector<std::unique_ptr<uint8_t[]>> m_chunks;
        size_t m_chunkBytes = 0, m_used = 0, m_total = 0;
    };

    // Fills host structs from cache file layout structs. Reads go through
    // a callback (guest memory at runtime, a file buffer in tools): it
    // returns bytes at a guest address, or nullptr if the range isn't
    // readable. Bad blocks (negative or huge counts, unreadable addresses)
    // come out empty and are counted.
    class Converter
    {
    public:
        using Reader = const uint8_t* (*)(void* context, uint32_t address, uint32_t bytes);

        // arena: where converted blocks and data go; read(context, address,
        // bytes): where the cache layout's bytes come from.
        Converter(Arena& arena, Reader read, void* context) : m_arena(arena), m_read(read), m_context(context) {}

        // bytes at a guest address through the reader, or null if unreadable.
        const uint8_t* Read(uint32_t address, uint32_t bytes) const { return m_read(m_context, address, bytes); }
        // The arena (generated converters allocate element arrays in it).
        Arena& GetArena() { return m_arena; }
        // How many blocks or data fields couldn't be converted.
        uint32_t Errors() const { return m_errors; }

        template <typename Host, typename Layout>
        void ConvertBlock(const layout::tag_block<Layout>& in, Block<Host>& out);
        // A data field: size, flags and guest address copied, and its bytes
        // copied into the arena (an unreadable range counts as an error and
        // leaves the field empty).
        void ConvertData(const layout::tag_data& in, TagData& out)
        {
            out.size = in.size.get();
            out.flags = in.flags.get();
            out.address = in.address.get();
            if (out.size <= 0)
            {
                out.size = 0;
                return;
            }
            const uint8_t* bytes = Read(out.address, uint32_t(out.size));
            if (!bytes)
            {
                m_errors++;
                out.size = 0;
                return;
            }
            uint8_t* copy = static_cast<uint8_t*>(m_arena.Allocate(size_t(out.size), 16));
            memcpy(copy, bytes, size_t(out.size));
            out.bytes = copy;
        }

    private:
        static constexpr int32_t kMaxElements = 1 << 20;
        Arena& m_arena;
        Reader m_read;
        void* m_context;
        uint32_t m_errors = 0;
    };

    // The reverse: writes host structs back in the cache file layout, blocks
    // and data to the guest addresses they came from. Used to check the
    // conversion loses nothing (a round trip must give the original bytes),
    // and later to give recompiled readers a guest copy of native tags.
    class Unconverter
    {
    public:
        using Writer = uint8_t* (*)(void* context, uint32_t address, uint32_t bytes);

        // write(context, address, bytes): where the cache layout's bytes go.
        Unconverter(Writer write, void* context) : m_write(write), m_context(context) {}

        // Writable bytes at a guest address through the writer, or null.
        uint8_t* Write(uint32_t address, uint32_t bytes) const { return m_write(m_context, address, bytes); }
        // How many blocks or data fields couldn't be written back.
        uint32_t Errors() const { return m_errors; }

        template <typename Host, typename Layout>
        void UnconvertBlock(const Block<Host>& in, layout::tag_block<Layout>& out);
        // The reverse: the field written back in cache layout, its bytes copied
        // to the guest address it came from.
        void UnconvertData(const TagData& in, layout::tag_data& out)
        {
            out.size = in.size;
            out.flags = in.flags;
            out.address = in.address;
            if (in.size <= 0)
                return;
            if (uint8_t* bytes = Write(in.address, uint32_t(in.size)))
                memcpy(bytes, in.bytes, size_t(in.size));
            else
                m_errors++;
        }
        // For generated code that finds a problem of its own.
        void NoteError() { m_errors++; }

    private:
        Writer m_write;
        void* m_context;
        uint32_t m_errors = 0;
    };

    // Convert(const layout::X&, X&, Converter&) for each struct is generated
    // (generated/tag_convert.cpp); argument-dependent lookup finds it here.
    template <typename Host, typename Layout>
    void Converter::ConvertBlock(const layout::tag_block<Layout>& in, Block<Host>& out)
    {
        int32_t count = in.count.get();
        out.address = in.address.get();
        out.storedCount = count;
        // An empty struct is 0 bytes in a cache file (1 in C++): nothing to read.
        if (count == 0 || std::is_empty_v<Layout>)
            return;
        const uint8_t* bytes = count > 0 && count <= kMaxElements ? Read(out.address, uint32_t(count) * sizeof(Layout)) : nullptr;
        if (!bytes)
        {
            m_errors++;
            return;
        }
        out.elements = m_arena.New<Host>(size_t(count));
        out.count = count;
        const Layout* elements = reinterpret_cast<const Layout*>(bytes);
        for (int32_t i = 0; i < count; i++)
            Convert(elements[i], out.elements[i], *this);
    }

    // A block written back: its count and address, then each element in
    // cache layout at the guest address it came from (an unwritable range
    // counts as an error).
    template <typename Host, typename Layout>
    void Unconverter::UnconvertBlock(const Block<Host>& in, layout::tag_block<Layout>& out)
    {
        out.count = in.storedCount;
        out.address = in.address;
        if (in.count == 0 || std::is_empty_v<Layout>)
            return;
        uint8_t* bytes = Write(in.address, uint32_t(in.count) * sizeof(Layout));
        if (!bytes)
        {
            m_errors++;
            return;
        }
        Layout* elements = reinterpret_cast<Layout*>(bytes);
        for (int32_t i = 0; i < in.count; i++)
            Unconvert(in.elements[i], elements[i], *this);
    }

    // The leaf field types: references and resource handles are copied as
    // they are, data fields through the converter (above).
    inline void Convert(const layout::tag_reference& in, TagReference& out, Converter&)
    {
        out.group = in.group.get();
        out.datum = in.datum.get();
    }
    inline void Convert(const layout::pageable_resource& in, PageableResource& out, Converter&)
    {
        out.handle = in.handle.get();
    }
    inline void Convert(const layout::tag_data& in, TagData& out, Converter& c) { c.ConvertData(in, out); }

    // Their reverses.
    inline void Unconvert(const TagReference& in, layout::tag_reference& out, Unconverter&)
    {
        out.group = in.group;
        out.datum = in.datum;
    }
    inline void Unconvert(const PageableResource& in, layout::pageable_resource& out, Unconverter&)
    {
        out.handle = in.handle;
    }
    inline void Unconvert(const TagData& in, layout::tag_data& out, Unconverter& u) { u.UnconvertData(in, out); }
}
