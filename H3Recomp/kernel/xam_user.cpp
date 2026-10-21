// xam.xex user and profile exports (one local profile, signed in on user 0)
// and messages to the system apps (XMsg*). Reference: Xenia's
// src/xenia/kernel/xam/xam_user.cc and apps/.
//
// The profile's gamertag and standard options come from settings.ini
// ([profile], edited with the launcher). Profile settings the game writes
// (Halo 3 keeps its whole player profile, controls and emblem included, in
// the title-specific settings) are saved to hdd/profile_settings.bin and
// read back on later runs.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "config/env.h"
#include "config/settings.h"
#include "vfs.h"
#include "objects.h"
#include "../memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <vector>

using namespace h3;
using namespace h3::kernel;

namespace
{
    constexpr uint32_t kErrorSuccess = 0;
    constexpr uint32_t kErrorInsufficientBuffer = 0x7A;
    constexpr uint32_t kErrorInvalidParameter = 0x57;
    constexpr uint32_t kErrorNoSuchUser = 0x525;
    // XamUserGetXUID and XamUserGetSigninInfo return HRESULTs; callers test
    // the sign bit, so the plain Win32 code would read as success (Halo
    // then counts every controller slot as a signed-in player).
    constexpr uint32_t kHresultNoSuchUser = 0x80070525;

    constexpr uint32_t kUserIndexAny = 0xFF;

    // The gamertag from settings.ini.
    const char* Gamertag() { return config::Active().profile.gamertag.c_str(); }

    enum : uint32_t
    {
        kSigninStateNotSignedIn = 0,
        kSigninStateSignedInLocally = 1,
    };

    bool IsSignedIn(uint32_t userIndex) { return userIndex == 0 || userIndex == kUserIndexAny; }

    // Standard profile settings and their defaults (XSOURCE_DEFAULT), as a
    // new profile has them. Title-specific settings (the title's own
    // blobs) start without a value.
    // The options the launcher sets (invert look, vibration, auto-aim and
    // auto-center) come from settings.ini.
    bool DefaultSetting(uint32_t id, uint32_t& value)
    {
        const config::ProfileSettings& profile = config::Active().profile;
        switch (id)
        {
        case 0x10040002: value = profile.invertLook; return true;        // XPROFILE_GAMER_YAXIS_INVERSION
        case 0x10040003: value = profile.vibration ? 3 : 0; return true;  // XPROFILE_OPTION_CONTROLLER_VIBRATION (3 = on)
        case 0x10040004: value = 0; return true;  // XPROFILE_GAMERCARD_ZONE
        case 0x10040005: value = 0; return true;  // XPROFILE_GAMERCARD_REGION
        case 0x10040006: value = 0; return true;  // XPROFILE_GAMERCARD_CRED
        case 0x1004000C: value = 0; return true;  // XPROFILE_OPTION_VOICE_MUTED
        case 0x1004000D: value = 0; return true;  // XPROFILE_OPTION_VOICE_THRU_SPEAKERS
        case 0x1004000E: value = 5; return true;  // XPROFILE_OPTION_VOICE_VOLUME
        case 0x10040012: value = 0; return true;  // XPROFILE_GAMERCARD_TITLES_PLAYED
        case 0x10040013: value = 0; return true;  // XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED
        case 0x10040015: value = 0; return true;  // XPROFILE_GAMER_DIFFICULTY: normal
        case 0x10040018: value = 0; return true;  // XPROFILE_GAMER_CONTROL_SENSITIVITY: medium
        case 0x1004001D: value = 0; return true;  // XPROFILE_GAMER_PREFERRED_COLOR_FIRST
        case 0x1004001E: value = 0; return true;  // XPROFILE_GAMER_PREFERRED_COLOR_SECOND
        case 0x10040022: value = profile.autoAim; return true;     // XPROFILE_GAMER_ACTION_AUTO_AIM
        case 0x10040023: value = profile.autoCenter; return true;  // XPROFILE_GAMER_ACTION_AUTO_CENTER
        case 0x10040024: value = 0; return true;  // XPROFILE_GAMER_ACTION_MOVEMENT_CONTROL
        default: return false;
        }
    }

    // --- Saved profile settings --------------------------------------------
    //
    // XUSER_PROFILE_SETTING (0x28 bytes): DWORD source, user index or XUID
    // at 8, DWORD setting id at 0x10, XUSER_DATA at 0x18. XUSER_DATA: BYTE
    // type, then at +8 either the value (8 bytes: int32, int64, double,
    // float, date) or, for binary and unicode data, DWORD size and a pointer
    // to the bytes.
    constexpr uint32_t kSettingSize = 0x28;
    constexpr uint32_t kSettingSource = 0x00;
    constexpr uint32_t kSettingUser = 0x08;
    constexpr uint32_t kSettingId = 0x10;
    constexpr uint32_t kSettingType = 0x18;
    constexpr uint32_t kSettingValue = 0x20;      // inline value (8 bytes)
    constexpr uint32_t kSettingDataSize = 0x20;   // binary/unicode: byte count
    constexpr uint32_t kSettingDataPointer = 0x24;

    // XUSER_DATA types that keep their bytes outside the setting.
    constexpr uint8_t kDataTypeUnicode = 4;
    constexpr uint8_t kDataTypeBinary = 6;
    bool HasExternalData(uint8_t type) { return type == kDataTypeUnicode || type == kDataTypeBinary; }

    // XUSER_PROFILE_SETTING source values.
    constexpr uint32_t kSourceNoValue = 0;
    constexpr uint32_t kSourceDefault = 1;
    constexpr uint32_t kSourceTitle = 2;  // written by the title

    // Header of the X_USER_READ_PROFILE_SETTINGS result: DWORD count,
    // pointer to the settings that follow.
    constexpr uint32_t kReadResultHeader = 8;

    // A value the game wrote: the XUSER_DATA type and its bytes in guest
    // (big-endian) order, either the 8-byte inline value or the external
    // data.
    struct StoredSetting
    {
        uint8_t type = 0;
        std::vector<uint8_t> bytes;
    };

    // The file format: "H3PS", version, count, then per setting: id, type,
    // byte count, bytes (header fields little-endian).
    constexpr uint32_t kStoreMagic = 0x53503348;  // "H3PS"
    constexpr uint32_t kStoreVersion = 1;
    constexpr const char* kStoreFile = "profile_settings.bin";

    std::mutex g_storeMutex;
    std::map<uint32_t, StoredSetting> g_store;  // by setting id
    bool g_storeLoaded = false;

    std::filesystem::path StorePath() { return UserDataRoot() / kStoreFile; }

    // Loads the saved settings the first time they're needed. Caller holds
    // g_storeMutex.
    void LoadStore()
    {
        if (g_storeLoaded)
            return;
        g_storeLoaded = true;
        std::ifstream file(StorePath(), std::ios::binary);
        if (!file)
            return;
        auto read32 = [&](uint32_t& value) { return bool(file.read(reinterpret_cast<char*>(&value), 4)); };
        uint32_t magic = 0, version = 0, count = 0;
        if (!read32(magic) || !read32(version) || !read32(count) || magic != kStoreMagic || version != kStoreVersion)
        {
            fprintf(stderr, "[xam] ignoring unreadable %s\n", StorePath().string().c_str());
            return;
        }
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t id = 0, type = 0, size = 0;
            if (!read32(id) || !read32(type) || !read32(size))
                break;
            StoredSetting setting{ uint8_t(type), std::vector<uint8_t>(size) };
            if (!file.read(reinterpret_cast<char*>(setting.bytes.data()), size))
                break;
            g_store[id] = std::move(setting);
        }
    }

    // Writes g_store to disk (through a temporary file, so a crash never
    // leaves half a file). Caller holds g_storeMutex.
    void SaveStore()
    {
        std::filesystem::path path = StorePath(), temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            auto write32 = [&](uint32_t value) { file.write(reinterpret_cast<const char*>(&value), 4); };
            write32(kStoreMagic);
            write32(kStoreVersion);
            write32(uint32_t(g_store.size()));
            for (const auto& [id, setting] : g_store)
            {
                write32(id);
                write32(setting.type);
                write32(uint32_t(setting.bytes.size()));
                file.write(reinterpret_cast<const char*>(setting.bytes.data()), std::streamsize(setting.bytes.size()));
            }
            if (!file.flush())
            {
                fprintf(stderr, "[xam] can't write %s\n", temporary.string().c_str());
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
    }

    // H3_LOG_USER=1: log profile and user calls.
    bool LogUser()
    {
        static const bool log = EnvFlag("H3_LOG_USER");
        return log;
    }

}

// Shared by the xam exports that take an XOVERLAPPED.
uint32_t h3::kernel::CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length)
{
    return CompleteOverlappedEx(overlapped, result, result, length);
}

uint32_t h3::kernel::CompleteOverlappedEx(uint32_t overlapped, uint32_t result, uint32_t extendedError, uint32_t length)
{
    if (overlapped == 0)
        return result;
    GuestRef<uint32_t>(overlapped) = result;                // InternalLow
    GuestRef<uint32_t>(overlapped + 4) = length;            // InternalHigh
    GuestRef<uint32_t>(overlapped + 0x18) = extendedError;  // dwExtendedError
    uint32_t eventHandle = GuestRef<uint32_t>(overlapped + 0xC);
    if (eventHandle != 0)
        if (auto event = LookupHandleAs<Event>(eventHandle))
            SetEvent(*event, true);
    return 0x3E5;  // ERROR_IO_PENDING
}

// DWORD XamUserGetSigninState(DWORD userIndex)
PPC_FUNC(__imp__XamUserGetSigninState)
{
    if (LogUser())
    {
        static std::set<uint32_t> seen;
        static std::mutex mutex;
        std::lock_guard lock(mutex);
        if (seen.insert(ctx.r3.u32).second)
            fprintf(stderr, "[xam] XamUserGetSigninState(%u) (lr=%08X)\n", ctx.r3.u32, uint32_t(ctx.lr));
    }
    ctx.r3.u64 = IsSignedIn(ctx.r3.u32) ? kSigninStateSignedInLocally : kSigninStateNotSignedIn;
}

// DWORD XamUserGetXUID(DWORD userIndex, DWORD type, PXUID xuid)
PPC_FUNC(__imp__XamUserGetXUID)
{
    if (LogUser())
        fprintf(stderr, "[xam] XamUserGetXUID(%u, type %u)\n", ctx.r3.u32, ctx.r4.u32);
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = kHresultNoSuchUser;
        return;
    }
    if (ctx.r5.u32 != 0)
        GuestRef<uint64_t>(ctx.r5.u32) = kLocalUserXuid;
    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamUserGetSigninInfo(DWORD userIndex, DWORD flags, PX_USER_SIGNIN_INFO info)
// X_USER_SIGNIN_INFO (0x28): QWORD xuid, DWORD flags, DWORD signin state,
// DWORD guest number, DWORD sponsor user index, char name[16].
PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    if (LogUser())
        fprintf(stderr, "[xam] XamUserGetSigninInfo(%u, flags %u)\n", ctx.r3.u32, ctx.r4.u32);
    uint32_t info = ctx.r5.u32;
    if (info == 0)
    {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    memset(GuestPtr<uint8_t>(info), 0, 0x28);
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = kHresultNoSuchUser;
        return;
    }
    GuestRef<uint64_t>(info) = kLocalUserXuid;
    GuestRef<uint32_t>(info + 0xC) = kSigninStateSignedInLocally;
    strncpy(GuestPtr<char>(info + 0x18), Gamertag(), config::kMaxGamertag);
    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamUserGetName(DWORD userIndex, LPSTR buffer, DWORD size)
PPC_FUNC(__imp__XamUserGetName)
{
    if (!IsSignedIn(ctx.r3.u32))
    {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32;
    if (buffer != 0 && size != 0)
    {
        char* out = GuestPtr<char>(buffer);
        strncpy(out, Gamertag(), size);
        out[size - 1] = '\0';
    }
    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamUserCheckPrivilege(DWORD userIndex, DWORD privilege, PBOOL result)
// Everything is allowed.
PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    if (ctx.r5.u32 != 0)
        GuestRef<uint32_t>(ctx.r5.u32) = IsSignedIn(ctx.r3.u32) ? 1 : 0;
    ctx.r3.u64 = IsSignedIn(ctx.r3.u32) ? kErrorSuccess : kErrorNoSuchUser;
}

// DWORD XamUserAreUsersFriends(DWORD userIndex, PXUID xuids, DWORD count, PBOOL result, PXOVERLAPPED overlapped)
PPC_FUNC(__imp__XamUserAreUsersFriends)
{
    if (ctx.r6.u32 != 0)
        GuestRef<uint32_t>(ctx.r6.u32) = 0;
    ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, kErrorSuccess);
}

// DWORD XamUserReadProfileSettings(DWORD titleId, DWORD userIndex, DWORD xuidCount, PXUID xuids,
//     DWORD settingCount, PDWORD settingIds, PDWORD bufferSize, PXUSER_READ_PROFILE_SETTING_RESULT buffer,
//     PXOVERLAPPED overlapped)
//
// For the local profile, settings the game saved come back as it wrote
// them (source 2); standard settings come back with their defaults (source
// 1); anything else has no value (source 0), so the game uses its own
// defaults. Other XUIDs (remote players) get defaults only. The result is
// the X_USER_READ_PROFILE_SETTINGS header, the 0x28-byte settings, then the
// binary/unicode data they point to.
PPC_FUNC(__imp__XamUserReadProfileSettings)
{
    uint32_t userIndex = ctx.r4.u32, xuidCount = ctx.r5.u32, xuids = ctx.r6.u32;
    uint32_t settingCount = ctx.r7.u32, settingIds = ctx.r8.u32, bufferSize = ctx.r9.u32, buffer = ctx.r10.u32;
    uint32_t overlapped = StackArgument(ctx, 8);
    if (bufferSize == 0 || (settingCount != 0 && settingIds == 0))
    {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    if (xuidCount == 0 && !IsSignedIn(userIndex))
    {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }

    std::lock_guard lock(g_storeMutex);
    LoadStore();

    // Which users are asking: the local profile gets its saved settings.
    uint32_t users = xuidCount != 0 ? xuidCount : 1;
    auto isLocal = [&](uint32_t u) { return xuidCount == 0 || uint64_t(GuestRef<uint64_t>(xuids + u * 8)) == kLocalUserXuid; };
    auto stored = [&](uint32_t u, uint32_t id) -> const StoredSetting*
    {
        if (!isLocal(u))
            return nullptr;
        auto found = g_store.find(id);
        return found != g_store.end() ? &found->second : nullptr;
    };

    // The size needed: header, settings, then the external data.
    uint32_t results = users * settingCount;
    uint32_t needed = kReadResultHeader + results * kSettingSize;
    for (uint32_t u = 0; u < users; u++)
        for (uint32_t i = 0; i < settingCount; i++)
            if (const StoredSetting* setting = stored(u, GuestRef<uint32_t>(settingIds + i * 4)); setting && HasExternalData(setting->type))
                needed += uint32_t(setting->bytes.size());
    if (LogUser())
        fprintf(stderr, "[xam] XamUserReadProfileSettings user %u xuids %u settings %u buffer %u/%u\n", userIndex, xuidCount,
            settingCount, uint32_t(GuestRef<uint32_t>(bufferSize)), needed);
    if (buffer == 0 || GuestRef<uint32_t>(bufferSize) < needed)
    {
        GuestRef<uint32_t>(bufferSize) = needed;
        ctx.r3.u64 = kErrorInsufficientBuffer;
        return;
    }

    memset(GuestPtr<uint8_t>(buffer), 0, needed);
    GuestRef<uint32_t>(buffer) = results;
    GuestRef<uint32_t>(buffer + 4) = buffer + kReadResultHeader;
    uint32_t data = buffer + kReadResultHeader + results * kSettingSize;  // next free byte for external data
    for (uint32_t u = 0; u < users; u++)
    {
        for (uint32_t i = 0; i < settingCount; i++)
        {
            uint32_t setting = buffer + kReadResultHeader + (u * settingCount + i) * kSettingSize;
            uint32_t id = GuestRef<uint32_t>(settingIds + i * 4);
            if (xuidCount != 0)
                GuestRef<uint64_t>(setting + kSettingUser) = GuestRef<uint64_t>(xuids + u * 8);
            else
                GuestRef<uint32_t>(setting + kSettingUser) = userIndex;
            GuestRef<uint32_t>(setting + kSettingId) = id;
            *GuestPtr<uint8_t>(setting + kSettingType) = uint8_t(id >> 28);  // the setting's data type

            uint32_t value = 0;
            if (const StoredSetting* saved = stored(u, id))
            {
                GuestRef<uint32_t>(setting + kSettingSource) = kSourceTitle;
                *GuestPtr<uint8_t>(setting + kSettingType) = saved->type;
                if (HasExternalData(saved->type))
                {
                    memcpy(GuestPtr<uint8_t>(data), saved->bytes.data(), saved->bytes.size());
                    GuestRef<uint32_t>(setting + kSettingDataSize) = uint32_t(saved->bytes.size());
                    GuestRef<uint32_t>(setting + kSettingDataPointer) = data;
                    data += uint32_t(saved->bytes.size());
                }
                else
                    memcpy(GuestPtr<uint8_t>(setting + kSettingValue), saved->bytes.data(), std::min<size_t>(saved->bytes.size(), 8));
            }
            else if (DefaultSetting(id, value))
            {
                GuestRef<uint32_t>(setting + kSettingSource) = kSourceDefault;
                GuestRef<uint32_t>(setting + kSettingValue) = value;  // int32 settings
            }
            else
                GuestRef<uint32_t>(setting + kSettingSource) = kSourceNoValue;
            if (LogUser() && u == 0)
                fprintf(stderr, "[xam]   setting %08X (title %08X) source %u\n", id, ctx.r3.u32,
                    uint32_t(GuestRef<uint32_t>(setting + kSettingSource)));
        }
    }
    ctx.r3.u64 = CompleteOverlapped(overlapped, kErrorSuccess);
}

// DWORD XamUserWriteProfileSettings(DWORD titleId, DWORD userIndex, DWORD settingCount,
//     PXUSER_PROFILE_SETTING settings, PXOVERLAPPED overlapped)
// Saves each setting (see XamUserReadProfileSettings) to
// hdd/profile_settings.bin.
PPC_FUNC(__imp__XamUserWriteProfileSettings)
{
    uint32_t settingCount = ctx.r5.u32, settings = ctx.r6.u32;
    if (LogUser())
        fprintf(stderr, "[xam] XamUserWriteProfileSettings user %u settings %u\n", ctx.r4.u32, settingCount);
    if (!IsSignedIn(ctx.r4.u32))
    {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }
    if (settingCount != 0 && settings == 0)
    {
        ctx.r3.u64 = kErrorInvalidParameter;
        return;
    }
    {
        std::lock_guard lock(g_storeMutex);
        LoadStore();
        for (uint32_t i = 0; i < settingCount; i++)
        {
            uint32_t setting = settings + i * kSettingSize;
            uint32_t id = GuestRef<uint32_t>(setting + kSettingId);
            StoredSetting saved{ *GuestPtr<uint8_t>(setting + kSettingType) };
            if (HasExternalData(saved.type))
            {
                uint32_t size = GuestRef<uint32_t>(setting + kSettingDataSize);
                uint32_t pointer = GuestRef<uint32_t>(setting + kSettingDataPointer);
                if (pointer != 0)
                    saved.bytes.assign(GuestPtr<uint8_t>(pointer), GuestPtr<uint8_t>(pointer) + size);
            }
            else
                saved.bytes.assign(GuestPtr<uint8_t>(setting + kSettingValue), GuestPtr<uint8_t>(setting + kSettingValue) + 8);
            if (LogUser())
                fprintf(stderr, "[xam]   save %08X type %u, %zu bytes\n", id, saved.type, saved.bytes.size());
            g_store[id] = std::move(saved);
        }
        SaveStore();
    }
    ctx.r3.u64 = CompleteOverlapped(ctx.r7.u32, kErrorSuccess);
}

// DWORD XamShowSigninUI(DWORD panes, DWORD flags)
// User 0 is always signed in; nothing to show.
PPC_FUNC(__imp__XamShowSigninUI)
{
    if (LogUser())
        fprintf(stderr, "[xam] XamShowSigninUI panes %u flags %08X\n", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = kErrorSuccess;
}

// --- System app messages -----------------------------------------------------
//
// XMsg* send a message (with a buffer) to a system app: 0xFA XMP (the music
// player), 0xFB XGI (presence, contexts), 0xFC XLiveBase. Halo asks XMP
// whether a soundtrack is playing before the campaign; garbage there shows
// a "music is playing" warning.

namespace
{
    constexpr uint32_t kResultSuccess = 0;
    constexpr uint32_t kResultFail = 0x80004005;

    constexpr uint32_t kAppXmp = 0xFA;
    constexpr uint32_t kAppXgi = 0xFB;

    // Handles a message to one of the system apps (XMP music player, XGI game info)
    // sent with XMsgSyncMessage/XMsgStartIORequest. Returns an HRESULT-style status.
    uint32_t DispatchAppMessage(uint32_t app, uint32_t message, uint32_t buffer, uint32_t length)
    {
        {
            static std::mutex mutex;
            static std::set<uint64_t> seen;
            std::lock_guard lock(mutex);
            if (seen.insert((uint64_t(app) << 32) | message).second)
                fprintf(stderr, "[xam] app message %02X:%08X (buffer %08X, %u bytes)\n", app, message, buffer, length);
        }
        if (app == kAppXmp)
        {
            // Who controls playback. Halo sets 0 for itself and shows the
            // "your music is playing" warning for anything else, so the
            // title holds it from the start.
            static uint32_t controller = 0, locked = 0;
            auto store = [](uint32_t pointer, uint32_t value) {
                if (pointer != 0)
                    GuestRef<uint32_t>(pointer) = value;
            };
            if (buffer == 0)
                return kResultSuccess;
            switch (message)
            {
            case 0x0007000B:  // XMPGetStatus { client, PDWORD status }: 0 = idle
                store(GuestRef<uint32_t>(buffer + 4), 0);
                break;
            case 0x0007001A:  // XMPSetPlaybackController { client, controller, locked }
                controller = GuestRef<uint32_t>(buffer + 4);
                locked = GuestRef<uint32_t>(buffer + 8);
                break;
            case 0x0007001B:  // XMPGetPlaybackController { client, PDWORD controller, PDWORD locked }
                store(GuestRef<uint32_t>(buffer + 4), controller);
                store(GuestRef<uint32_t>(buffer + 8), locked);
                break;
            default:
                break;
            }
            return kResultSuccess;
        }
        if (app == kAppXgi)
            return kResultSuccess;  // contexts, properties, presence: nothing to do offline
        return kResultFail;         // Xbox Live and the rest
    }
}

// DWORD XMsgInProcessCall(DWORD app, DWORD message, PVOID buffer, DWORD length)
PPC_FUNC(__imp__XMsgInProcessCall)
{
    ctx.r3.u64 = DispatchAppMessage(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// DWORD XMsgStartIORequest(DWORD app, DWORD message, PXOVERLAPPED overlapped, PVOID buffer, DWORD length)
PPC_FUNC(__imp__XMsgStartIORequest)
{
    uint32_t result = DispatchAppMessage(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, result);
}

// DWORD XMsgStartIORequestEx(DWORD app, DWORD message, PXOVERLAPPED overlapped, PVOID buffer, DWORD length, PVOID unknown)
PPC_FUNC(__imp__XMsgStartIORequestEx)
{
    uint32_t result = DispatchAppMessage(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u64 = CompleteOverlapped(ctx.r5.u32, result);
}

// DWORD XMsgCancelIORequest(PXOVERLAPPED overlapped, BOOL wait)
// Requests complete immediately, so there's nothing to cancel.
PPC_FUNC(__imp__XMsgCancelIORequest)
{
    ctx.r3.u64 = kResultSuccess;
}
