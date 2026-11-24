// SHA-1 (FIPS 180-4), for checking the game executable against the
// supported build. Not for anything security-related.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace h3setup
{
    // SHA-1 (RFC 3174), to check default.xex against the supported builds.
    class Sha1
    {
    public:
        // Feeds bytes; every full 64-byte block is processed.
        void Update(const void* data, size_t size)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            m_length += uint64_t(size) * 8;
            while (size--)
            {
                m_block[m_used++] = *bytes++;
                if (m_used == 64)
                {
                    Process();
                    m_used = 0;
                }
            }
        }

        // Pads and finishes the hash, returned as 40 hex digits.
        std::string Hex()
        {
            uint64_t length = m_length;
            uint8_t pad = 0x80;
            Update(&pad, 1);
            pad = 0;
            while (m_used != 56)
                Update(&pad, 1);
            for (int i = 7; i >= 0; i--)
            {
                uint8_t byte = uint8_t(length >> (i * 8));
                Update(&byte, 1);
            }
            std::string text;
            char digit[9];
            for (uint32_t word : m_state)
            {
                snprintf(digit, sizeof(digit), "%08x", word);
                text += digit;
            }
            return text;
        }

    private:
        // Rotate left.
        static uint32_t Rol(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

        // One 64-byte block through the 80 rounds.
        void Process()
        {
            uint32_t w[80];
            for (int i = 0; i < 16; i++)
                w[i] = uint32_t(m_block[i * 4]) << 24 | uint32_t(m_block[i * 4 + 1]) << 16 | uint32_t(m_block[i * 4 + 2]) << 8 |
                       m_block[i * 4 + 3];
            for (int i = 16; i < 80; i++)
                w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4];
            for (int i = 0; i < 80; i++)
            {
                uint32_t f, k;
                if (i < 20)
                    f = (b & c) | (~b & d), k = 0x5A827999;
                else if (i < 40)
                    f = b ^ c ^ d, k = 0x6ED9EBA1;
                else if (i < 60)
                    f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
                else
                    f = b ^ c ^ d, k = 0xCA62C1D6;
                uint32_t t = Rol(a, 5) + f + e + k + w[i];
                e = d, d = c, c = Rol(b, 30), b = a, a = t;
            }
            m_state[0] += a, m_state[1] += b, m_state[2] += c, m_state[3] += d, m_state[4] += e;
        }

        std::array<uint32_t, 5> m_state{ 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        std::array<uint8_t, 64> m_block{};
        size_t m_used = 0;
        uint64_t m_length = 0;
    };
}
