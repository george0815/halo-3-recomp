// xboxkrnl's C string exports: sprintf and _vsnprintf. Reference: Xenia's
// xboxkrnl_strings.cc.
//
// Guest arguments are 64-bit slots, big-endian in memory: integers sit in
// the low 32 bits (64 for %ll / %I64), doubles take a whole slot, and
// pointers (strings) are 32-bit guest addresses. A va_list is a guest
// pointer to such slots (the caller's spilled registers, then its stack
// arguments); the varargs of sprintf itself start in a register.

#include <ppc_recomp_shared.h>

#include "../memory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

using namespace h3;

namespace
{
    // Where the next argument comes from.
    class ArgumentSource
    {
    public:
        virtual ~ArgumentSource() = default;
        virtual uint64_t Next() = 0;
    };

    // A va_list: consecutive 8-byte big-endian slots in guest memory.
    class VaListArguments : public ArgumentSource
    {
    public:
        explicit VaListArguments(uint32_t address) : m_address(address) {}
        uint64_t Next() override
        {
            uint64_t value = GuestRef<uint64_t>(m_address);
            m_address += kSlotBytes;
            return value;
        }

    private:
        static constexpr uint32_t kSlotBytes = 8;
        uint32_t m_address;
    };

    // Varargs of a call: registers r<first>..r10, then the caller's stack
    // parameter area (Xenia's SHIM_GET_ARG_64: r1 + 0x54 + 8 per slot past
    // the eighth argument).
    class RegisterArguments : public ArgumentSource
    {
    public:
        RegisterArguments(PPCContext& ctx, uint32_t firstRegister) : m_ctx(ctx), m_index(firstRegister - kFirstArgumentRegister) {}
        uint64_t Next() override
        {
            uint32_t index = m_index++;
            // PPCContext doesn't keep r3-r10 in order (r3 comes first).
            switch (index)
            {
            case 0: return m_ctx.r3.u64;
            case 1: return m_ctx.r4.u64;
            case 2: return m_ctx.r5.u64;
            case 3: return m_ctx.r6.u64;
            case 4: return m_ctx.r7.u64;
            case 5: return m_ctx.r8.u64;
            case 6: return m_ctx.r9.u64;
            case 7: return m_ctx.r10.u64;
            default: break;
            }
            return GuestRef<uint64_t>(m_ctx.r1.u32 + kStackArgumentOffset + (index - kRegisterArguments) * 8);
        }

    private:
        static constexpr uint32_t kFirstArgumentRegister = 3;  // r3
        static constexpr uint32_t kRegisterArguments = 8;      // r3-r10
        static constexpr uint32_t kStackArgumentOffset = 0x54;
        PPCContext& m_ctx;
        uint32_t m_index;
    };

    // Reads a guest UTF-16 (big-endian) string as narrow text (non-ASCII
    // characters become '?'): %S and %ls.
    std::string NarrowFromGuestWide(uint32_t address, int maxChars)
    {
        std::string text;
        for (int i = 0; maxChars < 0 || i < maxChars; i++)
        {
            uint16_t c = GuestRef<uint16_t>(address + i * 2);
            if (c == 0)
                break;
            text += c < 0x80 ? char(c) : '?';
        }
        return text;
    }

    // Formats like the MS CRT's printf, reading arguments from args.
    // Supports flags, width and precision (including '*'), the length
    // prefixes h, hh, l, ll, I64, I32, z, and the conversions d i u o x X c
    // s S p n e E f g G a A %.
    std::string FormatGuest(const char* format, ArgumentSource& args)
    {
        std::string out;
        for (const char* p = format; *p; p++)
        {
            if (*p != '%')
            {
                out += *p;
                continue;
            }
            // A host format spec rebuilt piece by piece.
            std::string spec = "%";
            p++;
            while (*p && strchr("-+ #0", *p))
                spec += *p++;
            if (*p == '*')
            {
                spec += std::to_string(int32_t(args.Next()));
                p++;
            }
            else
                while (*p >= '0' && *p <= '9')
                    spec += *p++;
            int precision = -1;
            if (*p == '.')
            {
                spec += *p++;
                if (*p == '*')
                {
                    precision = int32_t(args.Next());
                    p++;
                }
                else
                {
                    precision = 0;
                    while (*p >= '0' && *p <= '9')
                        precision = precision * 10 + (*p++ - '0');
                }
                spec += std::to_string(std::max(precision, 0));
            }
            // Length prefixes.
            bool wide64 = false, wideChars = false, half = false;
            for (;;)
            {
                if (*p == 'l' && p[1] == 'l')
                    wide64 = true, p += 2;
                else if (*p == 'I' && p[1] == '6' && p[2] == '4')
                    wide64 = true, p += 3;
                else if (*p == 'I' && p[1] == '3' && p[2] == '2')
                    p += 3;
                else if (*p == 'l' || *p == 'w')
                    wideChars = true, p++;
                else if (*p == 'h')
                    half = true, p++;
                else if (*p == 'z' || *p == 'I' || *p == 'L' || *p == 'j' || *p == 't')
                    p++;
                else
                    break;
            }
            char conversion = *p;
            if (!conversion)
                break;
            char buffer[512];
            switch (conversion)
            {
            case '%':
                out += '%';
                break;
            case 'd':
            case 'i':
            {
                uint64_t raw = args.Next();
                long long value = wide64 ? (long long)(int64_t)raw : (long long)(int32_t)raw;
                snprintf(buffer, sizeof(buffer), (spec + "lld").c_str(), value);
                out += buffer;
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X':
            {
                uint64_t raw = args.Next();
                unsigned long long value = wide64 ? raw : (unsigned long long)uint32_t(raw);
                snprintf(buffer, sizeof(buffer), (spec + "ll" + conversion).c_str(), value);
                out += buffer;
                break;
            }
            case 'p':
                snprintf(buffer, sizeof(buffer), "%08X", uint32_t(args.Next()));
                out += buffer;
                break;
            case 'c':
            case 'C':
            {
                char c = char(args.Next());
                snprintf(buffer, sizeof(buffer), (spec + "c").c_str(), c);
                out += buffer;
                break;
            }
            case 's':
            case 'S':
            {
                // %s is narrow unless 'l'/'w'; %S is the opposite (unless 'h').
                bool wide = conversion == 's' ? wideChars : !half;
                uint32_t address = uint32_t(args.Next());
                std::string text = address == 0 ? "(null)"
                    : wide                     ? NarrowFromGuestWide(address, precision)
                                               : std::string(GuestPtr<const char>(address),
                                                     precision < 0 ? strlen(GuestPtr<const char>(address))
                                                                   : strnlen(GuestPtr<const char>(address), size_t(precision)));
                // Width/flags only (the precision was applied above).
                std::string stringSpec = spec.substr(0, spec.find('.'));
                snprintf(buffer, sizeof(buffer), (stringSpec + "s").c_str(), text.c_str());
                out += buffer;
                break;
            }
            case 'n':
                args.Next();  // writing the count back isn't needed by any caller seen
                break;
            case 'e':
            case 'E':
            case 'f':
            case 'F':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                uint64_t raw = args.Next();
                double value;
                memcpy(&value, &raw, sizeof(value));
                snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), value);
                out += buffer;
                break;
            }
            default:
                // Unknown conversion: copy it through.
                out += spec;
                out += conversion;
                break;
            }
        }
        return out;
    }

    // Copies text into a guest buffer of `count` chars with _snprintf
    // semantics: a terminator only if it fits; returns the length, or -1
    // if the text was cut short.
    int32_t StoreTruncated(uint32_t buffer, uint32_t count, const std::string& text)
    {
        if (text.size() < count)
        {
            memcpy(GuestPtr<char>(buffer), text.c_str(), text.size() + 1);
            return int32_t(text.size());
        }
        memcpy(GuestPtr<char>(buffer), text.data(), count);
        return text.size() == count ? int32_t(count) : -1;
    }
}

// int sprintf(char* buffer, const char* format, ...)
PPC_FUNC(__imp__sprintf)
{
    uint32_t buffer = ctx.r3.u32;
    constexpr uint32_t kFirstVararg = 5;  // r5, after buffer and format
    RegisterArguments args(ctx, kFirstVararg);
    std::string text = FormatGuest(GuestPtr<const char>(ctx.r4.u32), args);
    memcpy(GuestPtr<char>(buffer), text.c_str(), text.size() + 1);
    ctx.r3.u64 = uint32_t(text.size());
}

// int _vsnprintf(char* buffer, size_t count, const char* format, va_list args)
PPC_FUNC(__imp___vsnprintf)
{
    VaListArguments args(ctx.r6.u32);
    std::string text = FormatGuest(GuestPtr<const char>(ctx.r5.u32), args);
    ctx.r3.u64 = uint64_t(int64_t(StoreTruncated(ctx.r3.u32, ctx.r4.u32, text)));
}
