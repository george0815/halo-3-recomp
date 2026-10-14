// Loading and unloading a map's tags, decompiled in place (docs/ROADMAP.md
// 6.3): cache_files.c's tag load and unload. What they call (header read
// and checks, the physical allocator, the DVD/HDD read, SHA-1 and RSA,
// progress and error UI) is still recompiled and called as such
// (guest_call.h).
//
//   sub_821A11D0  cache_file_tags_load(scenario path) -> bool
//   sub_821A17E8  cache_file_tags_unload()
//
// The cache file globals (kGlobals):
//   +0       u8      tags loaded
//   +4       u32     the tag section's memory (physical allocation)
//   +16      12 KB   the map's header (cache/cache_file.h has the layout)
//   +0x3010  u32     the loaded tag index header (in the tag section)
//
// After a load the native tag cache converts the whole map
// (tags::OnTagsLoaded); an unload drops it. H3_ORIGINAL=cache_file_tags
// runs the recompiled pair instead (both or neither: they share state).
//
// Checked: the Sierra 117 film replays tick for tick the same as with the
// originals, and H3_CHECK_TAGS compares the loaded index with the file.

#include "guest_call.h"
#include "memory.h"
#include "native.h"

#include <ppc_recomp_shared.h>

#include <cstdint>
#include <cstring>

PPC_EXTERN_FUNC(sub_821A19D0);  // clears the signature check's work buffer
PPC_EXTERN_FUNC(sub_821F3500);  // cache_file_header_read(path, header out) -> bool
PPC_EXTERN_FUNC(sub_821A0E60);  // cache_file_header_verify(header, path, quiet) -> bool
PPC_EXTERN_FUNC(sub_821980F0);  // whether a build string is one this executable accepts
PPC_EXTERN_FUNC(sub_821A1A30);  // whether an address is inside one of the header's partitions
PPC_EXTERN_FUNC(sub_821AC948);  // physical_memory_allocate(...)
PPC_EXTERN_FUNC(sub_82589D30);  // MmSetAddressProtect(address, size, protect) unless size is 0
PPC_EXTERN_FUNC(sub_82589D50);  // MmQueryAddressProtect(address)
PPC_EXTERN_FUNC(sub_821F0998);  // loading progress: bytes about to be read (0: done)
PPC_EXTERN_FUNC(sub_821A1C60);  // cache_file_read(kind, offset, bytes, buffer) -> bool
PPC_EXTERN_FUNC(sub_821A9940);  // keeps the main loop's housekeeping alive during long work
PPC_EXTERN_FUNC(sub_821A1060);  // the signature check's work buffer, or 0
PPC_EXTERN_FUNC(sub_821F5E98);  // clears a header copy's signature fields
PPC_EXTERN_FUNC(sub_8219F0C0);  // sha1_begin(state, ...) -> bool
PPC_EXTERN_FUNC(sub_8259E4F8);  // sha1_update(state, data, bytes)
PPC_EXTERN_FUNC(sub_8259E500);  // sha1_end(state, digest out)
PPC_EXTERN_FUNC(sub_8219F058);  // sha1(data, bytes, ..., digest out)
PPC_EXTERN_FUNC(sub_8219F1D0);  // rsa_verify(digest, signature) -> bool
PPC_EXTERN_FUNC(sub_8219F3B0);  // error report: begin (on a stack buffer)
PPC_EXTERN_FUNC(sub_8219F408);  // error report: end
PPC_EXTERN_FUNC(sub_82084310);  // closes what the previous map left open (unknown yet)

namespace h3::native::tags
{
    void OnTagsLoaded();    // tag_cache.cpp
    void OnTagsUnloaded();  // tag_cache.cpp
}

namespace h3::native::cache
{
    namespace
    {
        // The cache file globals and offsets in them.
        constexpr uint32_t kGlobals = 0x82945F50;
        constexpr uint32_t kLoaded = 0;            // u8
        constexpr uint32_t kTagMemory = 4;         // u32
        constexpr uint32_t kHeader = 16;           // the map's header
        constexpr uint32_t kHeaderBytes = 12288;
        constexpr uint32_t kTagIndex = 12304;      // u32: the loaded tag index header

        // Header fields (cache_file.h).
        constexpr uint32_t kHeaderTagIndex = 0x10;
        constexpr uint32_t kHeaderTagSectionOffset = 0x14;
        constexpr uint32_t kHeaderTagSectionSize = 0x18;
        constexpr uint32_t kHeaderBuild = 0x11C;
        constexpr uint32_t kHeaderTracked = 0x141;     // u8: must be set (builds checked by name)
        constexpr uint32_t kHeaderPartitions = 0x2F0;  // 6 x (address, size)
        constexpr uint32_t kHeaderSignature = 0x36C;   // 256-byte RSA signature
        constexpr uint32_t kPartitionCount = 6;

        // Tag index header (cache_file.h).
        constexpr uint32_t kIndexTagCount = 0x08;
        constexpr uint32_t kIndexTagTable = 0x0C;
        constexpr uint32_t kIndexGlobalCount = 0x10;
        constexpr uint32_t kIndexGlobalTable = 0x14;
        constexpr uint32_t kIndexInteropCount = 0x18;  // (handle, definition index) pairs: D3D resources
        constexpr uint32_t kIndexInteropTable = 0x1C;
        constexpr uint32_t kIndexMagic = 0x24;
        constexpr uint32_t kTagsMagic = 0x74616773;    // 'tags'

        // What a load publishes.
        constexpr uint32_t kTagTable = 0x827CFF0C;        // u32: the tag table (tag_get reads it)
        constexpr uint32_t kTagTableDescriptor = 0x829DF558;     // +8 kind, +12 address
        constexpr uint32_t kTagTableDescriptorKind = 0x00475449;  // "GTI"
        constexpr uint32_t kTagTableDescriptorPointer = 0x828C29B4;
        constexpr uint32_t kScenarioDatum = 0x8274B794;
        constexpr uint32_t kGlobalsDatum = 0x8274B798;
        constexpr uint32_t kGlobalsTag = 0x827CF604;     // u32: 'matg' data address
        constexpr uint32_t kScenarioTag = 0x827CFF08;    // u32: 'scnr' data address
        constexpr uint32_t kGlobalsReference = 1272;     // a datum in 'matg' (whose tag's +80, +84 are copied)
        constexpr uint32_t kGlobalsCopy = 0x828C46C8;    // 2 words
        constexpr uint32_t kScnr = 0x73636E72, kMatg = 0x6D617467;
        constexpr uint32_t kNone = 0xFFFFFFFF;

        // Physical allocation of the tag section: kind 5, read-write.
        constexpr uint32_t kAllocationKind = 5;
        constexpr uint32_t kPageReadWrite = 4;
        constexpr uint32_t kPageReadOnlyProtect = 1028;  // the tag index's partition, after the load
        constexpr uint32_t kProtectedPartition = 4;
        constexpr uint32_t kReadKindTags = 2;
        constexpr uint32_t kPageSize = 4096;

        // Loading progress (UI).
        constexpr uint32_t kProgress = 0x827DB5EC;  // +12 u8 enabled, +16 u32 stage
        // The signature check's work buffer (13,680 bytes, sub_821A1060).
        constexpr uint32_t kWorkHeader = 0;          // the header copy (12 KB)
        constexpr uint32_t kWorkVerified = 12288;    // u8
        constexpr uint32_t kWorkRegionSizes = 12292; // 3 x u32, then 3 x u32 addresses
        constexpr uint32_t kWorkRegionAddresses = 12304;
        constexpr uint32_t kWorkDigests = 12316;     // 3 x 20 bytes
        constexpr uint32_t kWorkSha = 12376;         // SHA-1 state
        constexpr uint32_t kWorkDigest = 13400;      // 20 bytes
        constexpr uint32_t kWorkSignature = 13420;   // 256 bytes
        constexpr uint32_t kRegions = 3;
        constexpr uint32_t kDigestBytes = 20;
        constexpr uint32_t kHashChunk = 1 << 20;
        // Failure reporting.
        constexpr uint32_t kReportFailures = 0x82949AF0;  // u32: nonzero to report
        constexpr uint32_t kTlsGameState = 116, kTlsError = 152;
        constexpr uint32_t kQuietState = 3;
        constexpr uint32_t kErrorReportBytes = 160;
        // Something the previous map left open (closed before and after).
        constexpr uint32_t kPendingHandle = 0x829DB9B0;
        // Interop definitions: +44 the unregister callback.
        constexpr uint32_t kInteropDefinitions = 0x82746030;
        constexpr uint32_t kInteropUnregister = 44;

        uint32_t Load32(uint32_t address) { return GuestRef<uint32_t>(address); }
        void Store32(uint32_t address, uint32_t value) { GuestRef<uint32_t>(address) = value; }
        uint32_t Header(uint32_t field) { return Load32(kGlobals + kHeader + field); }
        uint32_t Tls(PPCContext& ctx, uint32_t slot) { return Load32(Load32(ctx.r13.u32) + slot); }

        // A global tag's datum by group, or NONE (the last match wins, as in
        // the original's loop).
        uint32_t FindGlobalTag(uint32_t index, uint32_t group)
        {
            uint32_t datum = kNone;
            int32_t count = int32_t(Load32(index + kIndexGlobalCount));
            uint32_t entry = Load32(index + kIndexGlobalTable);
            for (int32_t i = 0; i < count; i++, entry += 8)
                if (Load32(entry) == group)
                    datum = Load32(entry + 4);
            return datum;
        }

        // A tag's data address from the tag table by slot (no salt check).
        uint32_t TagAddress(uint32_t table, uint32_t datum) { return Load32(table + (datum & 0xFFFF) * 8 + 4); }

        // Hashes the header copy and the tag section's two halves, signs the
        // three digests' digest against the header's signature. True if it
        // verifies (the work buffer's flag is set too).
        bool VerifySignature(PPCContext& ctx, uint8_t* base)
        {
            Call(ctx, base, sub_821A9940);
            uint32_t work = Call(ctx, base, sub_821A1060);
            if (!work)
                return false;
            uint32_t digests = work + kWorkDigests;
            GuestRef<uint8_t>(work + kWorkVerified) = 0;
            memset(GuestPtr<uint8_t>(digests), 0xBB, kRegions * kDigestBytes);
            for (uint32_t i = 0; i < kDigestBytes; i += 4)
                Store32(work + kWorkDigest + i, 0xCCCCCCCC);
            memset(GuestPtr<uint8_t>(work + kWorkSignature), 0xDD, 256);
            memcpy(GuestPtr<uint8_t>(work + kWorkHeader), GuestPtr<uint8_t>(kGlobals + kHeader), kHeaderBytes);
            Call(ctx, base, sub_821F5E98, work + kWorkHeader);

            // Regions: the header copy, partitions 0-2, partitions 3-5.
            uint32_t partitionSize[kPartitionCount];
            for (uint32_t p = 0; p < kPartitionCount; p++)
                partitionSize[p] = Header(kHeaderPartitions + p * 8 + 4);
            uint32_t tagMemory = Load32(kGlobals + kTagMemory);
            uint32_t low = partitionSize[0] + partitionSize[1] + partitionSize[2];
            uint32_t high = partitionSize[3] + partitionSize[4] + partitionSize[5];
            uint32_t sizes[kRegions] = { kHeaderBytes, low, high };
            uint32_t addresses[kRegions] = { work + kWorkHeader, tagMemory, tagMemory + low };
            for (uint32_t i = 0; i < kRegions; i++)
            {
                Store32(work + kWorkRegionSizes + i * 4, sizes[i]);
                Store32(work + kWorkRegionAddresses + i * 4, addresses[i]);
            }
            for (uint32_t i = 0; i < kRegions; i++)
            {
                int32_t remaining = int32_t(Load32(work + kWorkRegionSizes + i * 4));
                uint32_t at = Load32(work + kWorkRegionAddresses + i * 4);
                if (!(Call(ctx, base, sub_8219F0C0, work + kWorkSha, 1024, 1) & 0xFF))
                    continue;
                while (remaining > 0)
                {
                    int32_t chunk = remaining > int32_t(kHashChunk) ? int32_t(kHashChunk) : remaining;
                    Call(ctx, base, sub_8259E4F8, work + kWorkSha, at, uint32_t(chunk));
                    Call(ctx, base, sub_821A9940);
                    remaining -= chunk;
                    at += uint32_t(chunk);
                }
                Call(ctx, base, sub_8259E500, work + kWorkSha, digests + i * kDigestBytes);
            }
            Call(ctx, base, sub_8219F058, digests, kRegions * kDigestBytes, 1, work + kWorkDigest);
            memcpy(GuestPtr<uint8_t>(work + kWorkSignature), GuestPtr<uint8_t>(kGlobals + kHeader + kHeaderSignature), 256);
            Call(ctx, base, sub_821A9940);
            work = Call(ctx, base, sub_821A1060);
            bool verified = Call(ctx, base, sub_8219F1D0, work + kWorkDigest, work + kWorkSignature) & 0xFF;
            if (verified)
                GuestRef<uint8_t>(work + kWorkVerified) = 1;
            return verified;
        }

        // Closes what kPendingHandle holds, if anything.
        void ClosePending(PPCContext& ctx, uint8_t* base)
        {
            if (Load32(kPendingHandle) != kNone)
            {
                Call(ctx, base, sub_82084310);
                Store32(kPendingHandle, kNone);
            }
        }

        bool LoadTags(PPCContext& ctx, uint8_t* base, uint32_t path)
        {
            bool headerRead = false, ok = false;
            uint32_t scenario = kNone;
            Call(ctx, base, sub_821A19D0);
            if (Call(ctx, base, sub_821F3500, path, kGlobals + kHeader) & 0xFF)
            {
                headerRead = true;
                bool accepted = (Call(ctx, base, sub_821A0E60, kGlobals + kHeader, path, 0) & 0xFF) &&
                    GuestRef<uint8_t>(kGlobals + kHeader + kHeaderTracked) &&
                    (Call(ctx, base, sub_821980F0, kGlobals + kHeader + kHeaderBuild) & 0xFF);
                if (accepted)
                {
                    // The tag section: allocate, read.
                    uint32_t size = Header(kHeaderTagSectionSize);
                    uint32_t memory = Call(ctx, base, sub_821AC948, kAllocationKind, 0, size, kPageReadWrite, 0, kNone);
                    Store32(kGlobals + kTagMemory, memory);
                    ok = memory != 0;
                    Call(ctx, base, sub_82589D30, memory, Header(kHeaderTagSectionSize), kPageReadWrite);
                    if (ok)
                    {
                        uint32_t rounded = (size & (kPageSize - 1)) ? (size | (kPageSize - 1)) + 1 : size;
                        bool progress = GuestRef<uint8_t>(kProgress + 12);
                        if (progress)
                        {
                            Store32(kProgress + 16, 1);
                            Call(ctx, base, sub_821F0998, rounded);
                        }
                        ok = Call(ctx, base, sub_821A1C60, kReadKindTags, Header(kHeaderTagSectionOffset), rounded,
                                 Load32(kGlobals + kTagMemory)) & 0xFF;
                        if (GuestRef<uint8_t>(kProgress + 12))
                        {
                            Call(ctx, base, sub_821F0998, 0);
                            Store32(kProgress + 16, kNone);
                        }
                    }
                    if (ok)
                        ok = VerifySignature(ctx, base);
                    if (ok)
                    {
                        // The tag index must be inside a partition, have a
                        // tag table and tags, and the 'tags' magic.
                        uint32_t index = Header(kHeaderTagIndex);
                        ok = (Call(ctx, base, sub_821A1A30, index) & 0xFF) && Load32(index + kIndexTagTable) != 0 &&
                            int32_t(Load32(index + kIndexTagCount)) > 0 && Load32(index + kIndexMagic) == kTagsMagic;
                        if (ok)
                            Store32(kGlobals + kTagIndex, index);
                    }
                    if (ok)
                    {
                        uint32_t partition = kGlobals + kHeader + kHeaderPartitions + kProtectedPartition * 8;
                        Call(ctx, base, sub_82589D30, Load32(partition), Load32(partition + 4), kPageReadOnlyProtect);
                        Call(ctx, base, sub_82589D50, Load32(partition));
                        uint32_t index = Load32(kGlobals + kTagIndex);
                        uint32_t table = Load32(index + kIndexTagTable);
                        Store32(kTagTable, table);
                        Store32(kTagTableDescriptor + 8, kTagTableDescriptorKind);
                        Store32(kTagTableDescriptor + 12, table);
                        Store32(kTagTableDescriptorPointer, kTagTableDescriptor);
                        GuestRef<uint8_t>(kGlobals + kLoaded) = 1;
                        scenario = FindGlobalTag(index, kScnr);
                    }
                }
            }
            if (!ok)
            {
                if (headerRead && Load32(kReportFailures) != 0 && Tls(ctx, kTlsGameState) != kQuietState)
                {
                    GuestFrame frame(ctx, base, kErrorReportBytes);
                    Call(ctx, base, sub_8219F3B0, frame.Address(0));
                    uint32_t error = Tls(ctx, kTlsError);
                    Store32(error + 52, 0);
                    GuestRef<uint8_t>(error + 1) = 1;
                    Call(ctx, base, sub_8219F408, frame.Address(0));
                }
                // (The original forgets the allocation without freeing it.)
                if (Load32(kGlobals + kTagMemory))
                    Store32(kGlobals + kTagMemory, 0);
                ClosePending(ctx, base);
            }
            Store32(kScenarioDatum, scenario);
            if (scenario == kNone)
                return ok;

            uint32_t index = Load32(kGlobals + kTagIndex);
            uint32_t table = Load32(kTagTable);
            uint32_t globals = FindGlobalTag(index, kMatg);
            Store32(kGlobalsDatum, globals);
            uint32_t globalsTag = TagAddress(table, globals);
            Store32(kGlobalsTag, globalsTag);
            Store32(kScenarioTag, TagAddress(table, scenario));
            uint32_t referenced = Load32(globalsTag + kGlobalsReference);
            // With no such tag the original reads guest address 80 and 84.
            uint32_t tag = referenced != kNone ? TagAddress(table, referenced) : 0;
            Store32(kGlobalsCopy, Load32(tag + 80));
            Store32(kGlobalsCopy + 4, Load32(tag + 84));
            return true;
        }

        void UnloadTags(PPCContext& ctx, uint8_t* base)
        {
            ClosePending(ctx, base);
            Call(ctx, base, sub_821A19D0);
            GuestRef<uint8_t>(kGlobals + kLoaded) = 0;
            Store32(kTagTable, 0);
            // Unregister the map's D3D resources (index rereads: a callback
            // may change it, as the original allows for).
            uint32_t index = Load32(kGlobals + kTagIndex);
            for (int32_t i = 0; i < int32_t(Load32(index + kIndexInteropCount)); i++)
            {
                uint32_t entry = Load32(index + kIndexInteropTable) + uint32_t(i) * 8;
                uint32_t definition = Load32(kInteropDefinitions + Load32(entry + 4) * 4);
                uint32_t unregister = Load32(definition + kInteropUnregister);
                if (unregister)
                {
                    CallIndirect(ctx, base, unregister, Load32(entry));
                    index = Load32(kGlobals + kTagIndex);
                }
            }
            Store32(kGlobals + kTagMemory, 0);
            memset(GuestPtr<uint8_t>(kGlobals + kHeader), 0, kHeaderBytes);
            Store32(kGlobals + kTagIndex, 0);
        }
    }
}

// cache_file_tags_load (r3 = the scenario path) -> bool.
PPC_FUNC_IMPL(__imp__sub_821A11D0);
PPC_FUNC(sub_821A11D0)
{
    static const bool original = h3::native::UseOriginal("cache_file_tags");
    if (original)
    {
        __imp__sub_821A11D0(ctx, base);
        return;
    }
    bool ok = h3::native::cache::LoadTags(ctx, base, ctx.r3.u32);
    if (ok)
        h3::native::tags::OnTagsLoaded();
    ctx.r3.u64 = ok ? 1 : 0;
}

// cache_file_tags_unload().
PPC_FUNC_IMPL(__imp__sub_821A17E8);
PPC_FUNC(sub_821A17E8)
{
    static const bool original = h3::native::UseOriginal("cache_file_tags");
    h3::native::tags::OnTagsUnloaded();
    if (original)
    {
        __imp__sub_821A17E8(ctx, base);
        return;
    }
    h3::native::cache::UnloadTags(ctx, base);
}
