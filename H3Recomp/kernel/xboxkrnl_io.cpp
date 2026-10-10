// xboxkrnl I/O manager exports: devices, files, symbolic links. Reference:
// Xenia's xboxkrnl_io.cc and xboxkrnl_ob.cc.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "objects.h"
#include "vfs.h"
#include "../memory.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

using namespace h3;
using namespace h3::kernel;

namespace
{
    // X_DEVICE_OBJECT. Only the fields the runtime fills in; the device
    // extension follows the object directly, as in Xenia.
    constexpr uint32_t kDeviceObjectSize = 0x60;
    constexpr uint32_t kDeviceType = 0x00;  // uint16_t, IO_TYPE_DEVICE
    constexpr uint32_t kDeviceSize = 0x02;  // uint16_t
    constexpr uint32_t kDeviceDriverObject = 0x08;
    constexpr uint32_t kDeviceExtension = 0x18;
    constexpr uint16_t kIoTypeDevice = 3;
}

// NTSTATUS IoCreateDevice(PDRIVER_OBJECT driver, ULONG extensionSize,
//     PANSI_STRING name, DEVICE_TYPE type, ULONG characteristics,
//     PDEVICE_OBJECT* deviceObject)
//
// Halo 3 creates its own device and calls into its dispatch routines
// directly, so all it needs is a zeroed object with a working extension.
PPC_FUNC(__imp__IoCreateDevice)
{
    uint32_t extensionSize = ctx.r4.u32;
    uint32_t device = SystemAlloc(kDeviceObjectSize + extensionSize);
    if (device == 0)
    {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }

    GuestRef<uint16_t>(device + kDeviceType) = kIoTypeDevice;
    GuestRef<uint16_t>(device + kDeviceSize) = uint16_t(kDeviceObjectSize + extensionSize);
    GuestRef<uint32_t>(device + kDeviceDriverObject) = ctx.r3.u32;
    GuestRef<uint32_t>(device + kDeviceExtension) = device + kDeviceObjectSize;
    GuestRef<uint32_t>(ctx.r8.u32) = device;
    ctx.r3.u64 = kStatusSuccess;
}

// --- Files -------------------------------------------------------------------
//
// Guest files map to host files through the VFS (vfs.h). All I/O completes
// synchronously: the IO_STATUS_BLOCK is filled in and the caller's event
// signaled before the call returns.

namespace
{
    constexpr uint32_t kStatusBufferOverflow = 0x80000005;
    constexpr uint32_t kStatusNoMoreFiles = 0x80000006;
    constexpr uint32_t kStatusInvalidInfoClass = 0xC0000003;
    constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004;
    constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
    constexpr uint32_t kStatusNoSuchFile = 0xC000000F;
    constexpr uint32_t kStatusEndOfFile = 0xC0000011;
    constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
    constexpr uint32_t kStatusObjectNameCollision = 0xC0000035;
    constexpr uint32_t kStatusObjectPathNotFound = 0xC000003A;
    constexpr uint32_t kStatusFileIsADirectory = 0xC00000BA;
    constexpr uint32_t kStatusNotADirectory = 0xC0000103;

    // Access mask bits that imply writing.
    constexpr uint32_t kWriteAccess = 0x40000000 /* GENERIC_WRITE */ | 0x10000000 /* GENERIC_ALL */
        | 0x2 /* FILE_WRITE_DATA */ | 0x4 /* FILE_APPEND_DATA */;

    // Create dispositions and the IO_STATUS_BLOCK.Information they report.
    enum : uint32_t { kSupersede, kOpen, kCreate, kOpenIf, kOverwrite, kOverwriteIf };
    enum : uint32_t { kSuperseded, kOpened, kCreated, kOverwritten };

    // Create options.
    constexpr uint32_t kDirectoryFile = 0x1;
    constexpr uint32_t kNonDirectoryFile = 0x40;

    constexpr uint32_t kAttributeReadOnly = 0x01;
    constexpr uint32_t kAttributeDirectory = 0x10;
    constexpr uint32_t kAttributeNormal = 0x80;

    // ByteOffset values that mean "use the file pointer" / "append".
    constexpr uint64_t kUseFilePointer = 0xFFFFFFFFFFFFFFFEull;
    constexpr uint64_t kWriteToEnd = 0xFFFFFFFFFFFFFFFFull;

    const bool g_logFiles = getenv("H3_LOG_FILES") != nullptr;
    const bool g_logReads = getenv("H3_LOG_READS") != nullptr;

    struct File : KernelObject
    {
        File() : KernelObject(ObjectType::File) {}
        ~File() override
        {
            if (stream != nullptr)
                fclose(stream);
            if (deleteOnClose)
            {
                std::error_code error;
                std::filesystem::remove(hostPath, error);
            }
        }

        bool IsSignaled(uint32_t) const override { return true; }
        int32_t SignalState() const override { return 1; }

        std::string guestPath;  // with links resolved
        std::filesystem::path hostPath;
        bool directory = false;
        bool writable = false;
        bool deleteOnClose = false;

        // FileCompletionInformation: I/O on the file posts packets here.
        std::shared_ptr<IoCompletion> completionPort;
        uint32_t completionKey = 0;

        std::mutex mutex;
        std::FILE* stream = nullptr;
        uint64_t position = 0;

        // NtQueryDirectoryFile state.
        std::vector<std::filesystem::directory_entry> entries;
        size_t nextEntry = 0;
        bool listed = false;
    };

    int Seek(std::FILE* stream, uint64_t offset)
    {
#ifdef _WIN32
        return _fseeki64(stream, int64_t(offset), SEEK_SET);
#else
        return fseeko(stream, off_t(offset), SEEK_SET);
#endif
    }

    // FILETIME: 100 ns intervals since 1601-01-01.
    uint64_t ToFileTime(std::filesystem::file_time_type time)
    {
        constexpr uint64_t kUnixEpochAsFileTime = 116444736000000000ull;
        auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
        auto sinceEpoch = std::chrono::duration_cast<std::chrono::nanoseconds>(system.time_since_epoch()).count();
        return kUnixEpochAsFileTime + uint64_t(sinceEpoch / 100);
    }

    struct FileInfo
    {
        uint64_t time = 0;  // used for creation, access, write and change
        uint64_t size = 0;
        uint32_t attributes = kAttributeNormal;
        bool directory = false;
    };

    FileInfo QueryFileInfo(const std::filesystem::path& path, bool writable)
    {
        std::error_code error;
        FileInfo info;
        info.directory = std::filesystem::is_directory(path, error);
        info.time = ToFileTime(std::filesystem::last_write_time(path, error));
        if (!info.directory)
            info.size = std::filesystem::file_size(path, error);
        info.attributes = info.directory ? kAttributeDirectory : kAttributeNormal;
        if (!writable)
            info.attributes |= kAttributeReadOnly;
        return info;
    }

    // FILE_NETWORK_OPEN_INFORMATION (0x38 bytes).
    void WriteNetworkOpenInfo(uint32_t out, const FileInfo& info)
    {
        for (uint32_t i = 0; i < 4; i++)
            GuestRef<uint64_t>(out + i * 8) = info.time;
        GuestRef<uint64_t>(out + 0x20) = (info.size + 0xFFF) & ~0xFFFull;
        GuestRef<uint64_t>(out + 0x28) = info.size;
        GuestRef<uint32_t>(out + 0x30) = info.attributes;
        GuestRef<uint32_t>(out + 0x34) = 0;
    }

    void CompleteIo(uint32_t iosb, uint32_t status, uint32_t information, uint32_t eventHandle)
    {
        if (iosb != 0)
        {
            GuestRef<uint32_t>(iosb + 0) = status;
            GuestRef<uint32_t>(iosb + 4) = information;
        }
        if (eventHandle != 0)
        {
            if (auto event = LookupHandleAs<Event>(eventHandle))
                SetEvent(*event, true);
        }
    }

    // Reads and writes on a file bound to a completion port also queue a
    // packet there, which is how the game learns its I/O finished.
    void PostFileCompletion(File& file, uint32_t apcContext, uint32_t status, uint32_t information)
    {
        if (file.completionPort)
            PostCompletion(*file.completionPort, { file.completionKey, apcContext, status, information });
    }

    // I/O with a completion routine queues routine(apcContext, iosb, 0) to
    // the issuing thread, which runs it during its next alertable wait. The
    // XDK's file API sets the routine's low bit as a flag; Xenia masks it
    // off too.
    void QueueIoApc(uint32_t routine, uint32_t apcContext, uint32_t iosb)
    {
        routine &= ~1u;
        if (routine != 0)
            QueueUserApc(*CurrentThreadObject(), { routine, apcContext, iosb, 0 });
    }

    // The path in an OBJECT_ATTRIBUTES, relative to its RootDirectory handle
    // if it has one.
    std::string GuestPath(uint32_t attributes)
    {
        std::string name = ObjectAttributesName(attributes);
        uint32_t root = attributes != 0 ? GuestRef<uint32_t>(attributes).get() : 0;
        if (root != 0)
        {
            if (auto directory = LookupHandleAs<File>(root))
                name = directory->guestPath + (name.empty() || name.front() == '\\' ? "" : "\\") + name;
        }
        return name;
    }

    // NtCreateFile and NtOpenFile.
    uint32_t OpenFile(uint32_t handlePtr, uint32_t access, uint32_t attributes, uint32_t iosb,
        uint32_t disposition, uint32_t options)
    {
        std::string guestPath = GuestPath(attributes);
        auto mapped = MapGuestPath(guestPath);

        auto finish = [&](uint32_t status, uint32_t information)
        {
            if (g_logFiles || (status != kStatusSuccess && status != kStatusObjectNameNotFound))
                fprintf(stderr, "[io] open %s (disposition %u, options 0x%X, access 0x%08X) -> 0x%08X\n", guestPath.c_str(),
                    disposition, options, access, status);
            CompleteIo(iosb, status, information, 0);
            return status;
        };

        if (!mapped)
            return finish(kStatusObjectPathNotFound, 0);

        std::error_code error;
        bool exists = mapped->exists;
        bool isDirectory = exists && std::filesystem::is_directory(mapped->path, error);
        bool wantsWrite = (access & kWriteAccess) != 0;

        if (exists && disposition == kCreate)
            return finish(kStatusObjectNameCollision, 0);
        if (!exists && (disposition == kOpen || disposition == kOverwrite))
            return finish(kStatusObjectNameNotFound, 0);
        if (isDirectory && (options & kNonDirectoryFile))
            return finish(kStatusFileIsADirectory, 0);
        if (exists && !isDirectory && (options & kDirectoryFile))
            return finish(kStatusNotADirectory, 0);

        bool create = !exists;
        bool truncate = exists && !isDirectory && (disposition == kSupersede || disposition == kOverwrite || disposition == kOverwriteIf);
        if ((create || truncate || wantsWrite) && !mapped->writable)
            return finish(kStatusAccessDenied, 0);

        auto file = std::make_shared<File>();
        file->guestPath = mapped->devicePath;
        file->hostPath = mapped->path;
        file->writable = mapped->writable;
        file->directory = isDirectory || (create && (options & kDirectoryFile));

        if (create && file->directory)
        {
            if (!std::filesystem::create_directory(file->hostPath, error))
                return finish(kStatusAccessDenied, 0);
        }
        else if (!file->directory)
        {
            const char* mode = (create || truncate) ? "w+b" : (mapped->writable && wantsWrite ? "r+b" : "rb");
            file->stream = fopen(file->hostPath.string().c_str(), mode);
            if (file->stream == nullptr)
                return finish(kStatusAccessDenied, 0);
        }

        uint32_t information = create ? kCreated
            : truncate ? (disposition == kSupersede ? kSuperseded : kOverwritten)
            : kOpened;
        GuestRef<uint32_t>(handlePtr) = CreateHandle(file);
        return finish(kStatusSuccess, information);
    }

    // Case-insensitive match with * and ? wildcards.
    bool MatchPattern(std::string_view pattern, std::string_view name)
    {
        if (pattern.empty())
            return name.empty();
        if (pattern.front() == '*')
        {
            for (size_t i = 0; i <= name.size(); i++)
            {
                if (MatchPattern(pattern.substr(1), name.substr(i)))
                    return true;
            }
            return false;
        }
        if (name.empty())
            return false;
        bool same = pattern.front() == '?'
            || std::tolower(static_cast<unsigned char>(pattern.front())) == std::tolower(static_cast<unsigned char>(name.front()));
        return same && MatchPattern(pattern.substr(1), name.substr(1));
    }

    std::string AnsiString(uint32_t string)
    {
        if (string == 0)
            return {};
        uint16_t length = GuestRef<uint16_t>(string);
        uint32_t buffer = GuestRef<uint32_t>(string + 4);
        return std::string(GuestPtr<const char>(buffer), length);
    }
}

// NTSTATUS NtCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
//     PIO_STATUS_BLOCK iosb, PLARGE_INTEGER allocationSize, ULONG fileAttributes,
//     ULONG shareAccess, ULONG disposition, ULONG options)
PPC_FUNC(__imp__NtCreateFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, StackArgument(ctx, 8));
}

// NTSTATUS NtOpenFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
//     PIO_STATUS_BLOCK iosb, ULONG shareAccess, ULONG options)
PPC_FUNC(__imp__NtOpenFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, kOpen, ctx.r8.u32);
}

// NTSTATUS NtReadFile(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apcRoutine,
//     PVOID apcContext, PIO_STATUS_BLOCK iosb, PVOID buffer, ULONG length,
//     PLARGE_INTEGER byteOffset)
PPC_FUNC(__imp__NtReadFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (!file || file->stream == nullptr)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }

    uint32_t length = ctx.r9.u32;
    uint32_t offsetPtr = ctx.r10.u32;

    size_t read;
    {
        std::lock_guard lock(file->mutex);
        uint64_t offset = offsetPtr != 0 ? GuestRef<uint64_t>(offsetPtr).get() : kUseFilePointer;
        if (offset == kUseFilePointer)
            offset = file->position;
        Seek(file->stream, offset);
        read = fread(GuestPtr<uint8_t>(ctx.r8.u32), 1, length, file->stream);
        file->position = offset + read;
    }

    uint32_t status = (read == 0 && length != 0) ? kStatusEndOfFile : kStatusSuccess;
    if (g_logReads)
        fprintf(stderr, "[io] read %s @0x%llX len 0x%X -> 0x%08X (%zu bytes)\n", file->guestPath.c_str(),
            (unsigned long long)(file->position - read), length, ctx.r8.u32, read);
    if (g_logFiles && read != length)
        fprintf(stderr, "[io] short read of %s: %zu of %u bytes at 0x%llX\n", file->guestPath.c_str(), read, length,
            (unsigned long long)(file->position - read));
    CompleteIo(ctx.r7.u32, status, uint32_t(read), ctx.r4.u32);
    PostFileCompletion(*file, ctx.r6.u32, status, uint32_t(read));
    QueueIoApc(ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = status;
}

// NTSTATUS NtReadFileScatter(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apcRoutine,
//     PVOID apcContext, PIO_STATUS_BLOCK iosb, PFILE_SEGMENT_ELEMENT segments,
//     ULONG length, PLARGE_INTEGER byteOffset)
// Each segment element points at one 4 KiB page.
PPC_FUNC(__imp__NtReadFileScatter)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (!file || file->stream == nullptr)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }

    uint32_t segments = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    uint32_t offsetPtr = ctx.r10.u32;

    size_t total = 0;
    {
        std::lock_guard lock(file->mutex);
        uint64_t offset = offsetPtr != 0 ? GuestRef<uint64_t>(offsetPtr).get() : kUseFilePointer;
        if (offset == kUseFilePointer)
            offset = file->position;
        Seek(file->stream, offset);
        for (uint32_t i = 0; total < length; i++)
        {
            uint32_t page = GuestRef<uint32_t>(segments + i * 4);
            size_t chunk = std::min<size_t>(kPageSize4K, length - total);
            size_t read = fread(GuestPtr<uint8_t>(page), 1, chunk, file->stream);
            total += read;
            if (read < chunk)
                break;
        }
        file->position = offset + total;
    }
    if (g_logReads)
        fprintf(stderr, "[io] scatter read %s @0x%llX len 0x%X (%zu bytes)\n", file->guestPath.c_str(),
            (unsigned long long)(file->position - total), length, total);

    uint32_t status = (total == 0 && length != 0) ? kStatusEndOfFile : kStatusSuccess;
    CompleteIo(ctx.r7.u32, status, uint32_t(total), ctx.r4.u32);
    PostFileCompletion(*file, ctx.r6.u32, status, uint32_t(total));
    QueueIoApc(ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = status;
}

// NTSTATUS NtWriteFile(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apcRoutine,
//     PVOID apcContext, PIO_STATUS_BLOCK iosb, PVOID buffer, ULONG length,
//     PLARGE_INTEGER byteOffset)
PPC_FUNC(__imp__NtWriteFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (!file || file->stream == nullptr)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (!file->writable)
    {
        ctx.r3.u64 = kStatusAccessDenied;
        return;
    }

    uint32_t length = ctx.r9.u32;
    uint32_t offsetPtr = ctx.r10.u32;

    size_t written;
    {
        std::lock_guard lock(file->mutex);
        uint64_t offset = offsetPtr != 0 ? GuestRef<uint64_t>(offsetPtr).get() : kUseFilePointer;
        if (offset == kUseFilePointer)
            offset = file->position;
        if (offset == kWriteToEnd)
        {
            fseek(file->stream, 0, SEEK_END);
            offset = uint64_t(ftell(file->stream));
        }
        Seek(file->stream, offset);
        written = fwrite(GuestPtr<uint8_t>(ctx.r8.u32), 1, length, file->stream);
        file->position = offset + written;
    }

    CompleteIo(ctx.r7.u32, kStatusSuccess, uint32_t(written), ctx.r4.u32);
    PostFileCompletion(*file, ctx.r6.u32, kStatusSuccess, uint32_t(written));
    QueueIoApc(ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtFlushBuffersFile(HANDLE handle, PIO_STATUS_BLOCK iosb)
PPC_FUNC(__imp__NtFlushBuffersFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (!file)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    {
        std::lock_guard lock(file->mutex);
        if (file->stream != nullptr)
            fflush(file->stream);
    }
    CompleteIo(ctx.r4.u32, kStatusSuccess, 0, 0);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryInformationFile(HANDLE handle, PIO_STATUS_BLOCK iosb, PVOID info,
//     ULONG length, FILE_INFORMATION_CLASS infoClass)
PPC_FUNC(__imp__NtQueryInformationFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (!file)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t iosb = ctx.r4.u32;
    uint32_t out = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    {
        std::lock_guard lock(file->mutex);
        if (file->stream != nullptr)
            fflush(file->stream);
    }
    FileInfo info = QueryFileInfo(file->hostPath, file->writable);

    uint32_t size;
    switch (infoClass)
    {
    case 4: size = 0x28; break;   // FileBasicInformation
    case 5: size = 0x18; break;   // FileStandardInformation
    case 6: size = 0x08; break;   // FileInternalInformation
    case 14: size = 0x08; break;  // FilePositionInformation
    case 34: size = 0x38; break;  // FileNetworkOpenInformation
    default:
        fprintf(stderr, "[io] NtQueryInformationFile: unsupported class %u for %s\n", infoClass, file->guestPath.c_str());
        ctx.r3.u64 = kStatusInvalidInfoClass;
        return;
    }
    if (length < size)
    {
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }

    switch (infoClass)
    {
    case 4:
        for (uint32_t i = 0; i < 4; i++)
            GuestRef<uint64_t>(out + i * 8) = info.time;
        GuestRef<uint32_t>(out + 0x20) = info.attributes;
        GuestRef<uint32_t>(out + 0x24) = 0;
        break;
    case 5:
        GuestRef<uint64_t>(out + 0x00) = (info.size + 0xFFF) & ~0xFFFull;
        GuestRef<uint64_t>(out + 0x08) = info.size;
        GuestRef<uint32_t>(out + 0x10) = 1;
        *GuestPtr<uint8_t>(out + 0x14) = file->deleteOnClose;
        *GuestPtr<uint8_t>(out + 0x15) = info.directory;
        break;
    case 6:
        GuestRef<uint64_t>(out) = std::hash<std::string>{}(file->guestPath);
        break;
    case 14:
        GuestRef<uint64_t>(out) = file->position;
        break;
    case 34:
        WriteNetworkOpenInfo(out, info);
        break;
    }
    CompleteIo(iosb, kStatusSuccess, size, 0);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtSetInformationFile(HANDLE handle, PIO_STATUS_BLOCK iosb, PVOID info,
//     ULONG length, FILE_INFORMATION_CLASS infoClass)
PPC_FUNC(__imp__NtSetInformationFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (!file)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t iosb = ctx.r4.u32;
    uint32_t in = ctx.r5.u32;
    uint32_t infoClass = ctx.r7.u32;

    std::lock_guard lock(file->mutex);
    switch (infoClass)
    {
    case 4:  // FileBasicInformation: times and attributes, ignored
        break;
    case 13:  // FileDispositionInformation
        if (!file->writable)
        {
            ctx.r3.u64 = kStatusAccessDenied;
            return;
        }
        file->deleteOnClose = *GuestPtr<uint8_t>(in) != 0;
        break;
    case 14:  // FilePositionInformation
        file->position = GuestRef<uint64_t>(in);
        break;
    case 30:  // FileCompletionInformation: port handle, key
    {
        auto port = LookupHandleAs<IoCompletion>(GuestRef<uint32_t>(in));
        if (!port)
        {
            ctx.r3.u64 = kStatusInvalidHandle;
            return;
        }
        file->completionPort = std::move(port);
        file->completionKey = GuestRef<uint32_t>(in + 4);
        break;
    }
    case 19:  // FileAllocationInformation
    case 20:  // FileEndOfFileInformation
    {
        if (!file->writable || file->stream == nullptr)
        {
            ctx.r3.u64 = kStatusAccessDenied;
            return;
        }
        fflush(file->stream);
        std::error_code error;
        std::filesystem::resize_file(file->hostPath, GuestRef<uint64_t>(in), error);
        break;
    }
    default:
        fprintf(stderr, "[io] NtSetInformationFile: unsupported class %u for %s\n", infoClass, file->guestPath.c_str());
        ctx.r3.u64 = kStatusInvalidInfoClass;
        return;
    }
    CompleteIo(iosb, kStatusSuccess, 0, 0);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryFullAttributesFile(POBJECT_ATTRIBUTES attributes,
//     PFILE_NETWORK_OPEN_INFORMATION info)
PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    std::string guestPath = GuestPath(ctx.r3.u32);
    auto mapped = MapGuestPath(guestPath);
    if (g_logFiles)
        fprintf(stderr, "[io] attributes %s -> %s\n", guestPath.c_str(), mapped && mapped->exists ? "found" : "missing");
    if (!mapped || !mapped->exists)
    {
        ctx.r3.u64 = mapped ? kStatusObjectNameNotFound : kStatusObjectPathNotFound;
        return;
    }
    WriteNetworkOpenInfo(ctx.r4.u32, QueryFileInfo(mapped->path, mapped->writable));
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apcRoutine,
//     PVOID apcContext, PIO_STATUS_BLOCK iosb, PFILE_DIRECTORY_INFORMATION info,
//     ULONG length, PANSI_STRING pattern, BOOLEAN restartScan)
//
// Returns one entry per call, like Xenia. FILE_DIRECTORY_INFORMATION:
// next offset, index, four times, end of file, allocation size,
// attributes, name length (0x3C), name (0x40, ANSI, not terminated).
PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (!file || !file->directory)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }

    uint32_t iosb = ctx.r7.u32;
    uint32_t out = ctx.r8.u32;
    uint32_t length = ctx.r9.u32;
    std::string pattern = AnsiString(ctx.r10.u32);
    bool restart = (StackArgument(ctx, 8) & 0xFF) != 0;

    std::lock_guard lock(file->mutex);
    bool first = !file->listed || restart;
    if (first)
    {
        if (pattern.empty())
            pattern = "*";
        file->entries.clear();
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(file->hostPath, error))
        {
            if (MatchPattern(pattern, entry.path().filename().string()))
                file->entries.push_back(entry);
        }
        std::ranges::sort(file->entries, {}, [](const auto& entry) { return entry.path().filename().string(); });
        file->nextEntry = 0;
        file->listed = true;
    }

    if (file->nextEntry >= file->entries.size())
    {
        uint32_t status = first ? kStatusNoSuchFile : kStatusNoMoreFiles;
        CompleteIo(iosb, status, 0, ctx.r4.u32);
        ctx.r3.u64 = status;
        return;
    }

    const auto& entry = file->entries[file->nextEntry];
    std::string name = entry.path().filename().string();
    uint32_t size = 0x40 + uint32_t(name.size());
    if (length < size)
    {
        CompleteIo(iosb, kStatusBufferOverflow, 0, ctx.r4.u32);
        ctx.r3.u64 = kStatusBufferOverflow;
        return;
    }
    file->nextEntry++;

    FileInfo info = QueryFileInfo(entry.path(), file->writable);
    GuestRef<uint32_t>(out + 0x00) = 0;
    GuestRef<uint32_t>(out + 0x04) = uint32_t(file->nextEntry - 1);
    for (uint32_t i = 0; i < 4; i++)
        GuestRef<uint64_t>(out + 0x08 + i * 8) = info.time;
    GuestRef<uint64_t>(out + 0x28) = info.size;
    GuestRef<uint64_t>(out + 0x30) = (info.size + 0xFFF) & ~0xFFFull;
    GuestRef<uint32_t>(out + 0x38) = info.attributes;
    GuestRef<uint32_t>(out + 0x3C) = uint32_t(name.size());
    memcpy(GuestPtr<char>(out + 0x40), name.data(), name.size());

    CompleteIo(iosb, kStatusSuccess, size, ctx.r4.u32);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE handle, PIO_STATUS_BLOCK iosb,
//     PVOID info, ULONG length, FS_INFORMATION_CLASS infoClass)
PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    auto file = LookupHandleAs<File>(ctx.r3.u32);
    if (!file)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t iosb = ctx.r4.u32;
    uint32_t out = ctx.r5.u32;
    uint32_t length = ctx.r6.u32;
    uint32_t infoClass = ctx.r7.u32;

    // FATX-like geometry: 512-byte sectors, 16 KiB clusters.
    constexpr uint32_t kBytesPerSector = 0x200;
    constexpr uint32_t kSectorsPerUnit = 0x20;
    constexpr uint64_t kUnitSize = kBytesPerSector * kSectorsPerUnit;
    // Sizes of a console volume, not the host disk's: a real cache
    // partition is 2 GB, and titles size their caches from these numbers
    // (a host disk's hundreds of GB overflow 32-bit math). Free space is
    // what the folder doesn't use yet.
    constexpr uint64_t kVolumeBytes = 2ull << 30;
    std::error_code error;
    uint64_t used = 0;
    if (file->writable)
        for (const auto& entry : std::filesystem::recursive_directory_iterator(file->hostPath, error))
            if (entry.is_regular_file(error))
                used += entry.file_size(error);
    uint64_t totalUnits = kVolumeBytes / kUnitSize;
    uint64_t freeUnits = file->writable ? (kVolumeBytes - std::min(used, kVolumeBytes)) / kUnitSize : 0;
    std::string_view fsName = file->writable ? "FATX" : "GDFX";

    uint32_t size;
    switch (infoClass)
    {
    case 1:  // FileFsVolumeInformation: creation time, serial, label length, supports objects, label
        size = 0x14;
        if (length < size)
            break;
        GuestRef<uint64_t>(out + 0x00) = 0;
        GuestRef<uint32_t>(out + 0x08) = 0x48334D33;
        GuestRef<uint32_t>(out + 0x0C) = 0;
        GuestRef<uint32_t>(out + 0x10) = 0;
        break;
    case 3:  // FileFsSizeInformation
        size = 0x18;
        if (length < size)
            break;
        GuestRef<uint64_t>(out + 0x00) = totalUnits;
        GuestRef<uint64_t>(out + 0x08) = freeUnits;
        GuestRef<uint32_t>(out + 0x10) = kSectorsPerUnit;
        GuestRef<uint32_t>(out + 0x14) = kBytesPerSector;
        break;
    case 4:  // FileFsDeviceInformation: device type, characteristics
        size = 0x08;
        if (length < size)
            break;
        GuestRef<uint32_t>(out + 0x00) = file->writable ? 0x07 /* disk */ : 0x02 /* CD-ROM */;
        GuestRef<uint32_t>(out + 0x04) = 0;
        break;
    case 5:  // FileFsAttributeInformation: attributes, max name length, name length, name
        size = 0x0C + uint32_t(fsName.size());
        if (length < size)
            break;
        GuestRef<uint32_t>(out + 0x00) = 0;
        GuestRef<uint32_t>(out + 0x04) = 255;
        GuestRef<uint32_t>(out + 0x08) = uint32_t(fsName.size());
        memcpy(GuestPtr<char>(out + 0x0C), fsName.data(), fsName.size());
        break;
    case 7:  // FileFsFullSizeInformation
        size = 0x20;
        if (length < size)
            break;
        GuestRef<uint64_t>(out + 0x00) = totalUnits;
        GuestRef<uint64_t>(out + 0x08) = freeUnits;
        GuestRef<uint64_t>(out + 0x10) = freeUnits;
        GuestRef<uint32_t>(out + 0x18) = kSectorsPerUnit;
        GuestRef<uint32_t>(out + 0x1C) = kBytesPerSector;
        break;
    default:
        fprintf(stderr, "[io] NtQueryVolumeInformationFile: unsupported class %u for %s\n", infoClass, file->guestPath.c_str());
        ctx.r3.u64 = kStatusInvalidInfoClass;
        return;
    }
    if (length < size)
    {
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }
    CompleteIo(iosb, kStatusSuccess, size, 0);
    ctx.r3.u64 = kStatusSuccess;
}

// --- Symbolic links and volumes -------------------------------------------

// NTSTATUS ObCreateSymbolicLink(PANSI_STRING link, PANSI_STRING target)
PPC_FUNC(__imp__ObCreateSymbolicLink)
{
    std::string link = AnsiString(ctx.r3.u32);
    std::string target = AnsiString(ctx.r4.u32);
    if (g_logFiles)
        fprintf(stderr, "[io] link %s -> %s\n", link.c_str(), target.c_str());
    AddSymbolicLink(link, target);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS ObDeleteSymbolicLink(PANSI_STRING link)
PPC_FUNC(__imp__ObDeleteSymbolicLink)
{
    ctx.r3.u64 = RemoveSymbolicLink(AnsiString(ctx.r3.u32)) ? kStatusSuccess : kStatusObjectNameNotFound;
}

// NTSTATUS IoDismountVolume(PDEVICE_OBJECT device)
// Host directories don't need flushing beyond what the files do.
PPC_FUNC(__imp__IoDismountVolume)
{
    ctx.r3.u64 = kStatusSuccess;
}
