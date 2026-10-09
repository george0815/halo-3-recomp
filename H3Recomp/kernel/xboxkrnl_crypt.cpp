// xboxkrnl XeCrypt exports. Reference: Xenia's xboxkrnl_crypt.cc.

#include <ppc_recomp_shared.h>

#include "../memory.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

using namespace h3;

namespace
{
    // XECRYPT_SHA_STATE (0x58 bytes, big-endian): bytes hashed so far, the
    // five SHA-1 state words, then the pending partial block. Kept in guest
    // memory so the state is exactly where the game expects it.
    constexpr uint32_t kShaCount = 0x00;
    constexpr uint32_t kShaState = 0x04;
    constexpr uint32_t kShaBuffer = 0x18;

    void ShaTransform(uint32_t state, const uint8_t* block)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t h[5];
        for (int i = 0; i < 5; i++)
            h[i] = GuestRef<uint32_t>(state + kShaState + i * 4);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = t;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        for (int i = 0; i < 5; i++)
            GuestRef<uint32_t>(state + kShaState + i * 4) = h[i];
    }

    void ShaInit(uint32_t state)
    {
        constexpr uint32_t kInitial[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        GuestRef<uint32_t>(state + kShaCount) = 0;
        for (int i = 0; i < 5; i++)
            GuestRef<uint32_t>(state + kShaState + i * 4) = kInitial[i];
        memset(GuestPtr<uint8_t>(state + kShaBuffer), 0, 64);
    }

    void ShaUpdate(uint32_t state, const uint8_t* input, uint32_t size)
    {
        uint32_t count = GuestRef<uint32_t>(state + kShaCount);
        uint8_t* buffer = GuestPtr<uint8_t>(state + kShaBuffer);
        GuestRef<uint32_t>(state + kShaCount) = count + size;

        uint32_t used = count & 63;
        while (size > 0)
        {
            uint32_t chunk = std::min(64 - used, size);
            memcpy(buffer + used, input, chunk);
            used += chunk;
            input += chunk;
            size -= chunk;
            if (used == 64)
            {
                ShaTransform(state, buffer);
                used = 0;
            }
        }
    }

    void ShaFinal(uint32_t state, uint8_t* out, uint32_t outSize)
    {
        // The count is 32-bit, as in the console's state struct.
        uint64_t bits = uint64_t(GuestRef<uint32_t>(state + kShaCount).get()) * 8;
        uint8_t padding[72] = { 0x80 };
        uint32_t used = GuestRef<uint32_t>(state + kShaCount).get() & 63;
        uint32_t padLength = (used < 56 ? 56 : 120) - used;
        for (int i = 0; i < 8; i++)
            padding[padLength + i] = uint8_t(bits >> (56 - i * 8));
        ShaUpdate(state, padding, padLength + 8);

        uint8_t digest[20];
        for (int i = 0; i < 5; i++)
        {
            uint32_t word = GuestRef<uint32_t>(state + kShaState + i * 4);
            digest[i * 4 + 0] = uint8_t(word >> 24);
            digest[i * 4 + 1] = uint8_t(word >> 16);
            digest[i * 4 + 2] = uint8_t(word >> 8);
            digest[i * 4 + 3] = uint8_t(word);
        }
        memcpy(out, digest, std::min<uint32_t>(outSize, sizeof(digest)));
    }
}

// void XeCryptShaInit(PXECRYPT_SHA_STATE state)
PPC_FUNC(__imp__XeCryptShaInit)
{
    ShaInit(ctx.r3.u32);
}

// void XeCryptShaUpdate(PXECRYPT_SHA_STATE state, PBYTE input, DWORD size)
PPC_FUNC(__imp__XeCryptShaUpdate)
{
    ShaUpdate(ctx.r3.u32, GuestPtr<const uint8_t>(ctx.r4.u32), ctx.r5.u32);
}

// void XeCryptShaFinal(PXECRYPT_SHA_STATE state, PBYTE out, DWORD outSize)
PPC_FUNC(__imp__XeCryptShaFinal)
{
    ShaFinal(ctx.r3.u32, GuestPtr<uint8_t>(ctx.r4.u32), ctx.r5.u32);
}

// void XeCryptSha(PBYTE input1, DWORD size1, PBYTE input2, DWORD size2,
//     PBYTE input3, DWORD size3, PBYTE out, DWORD outSize)
// Hashes up to three buffers in one go. Uses a scratch state in guest memory.
PPC_FUNC(__imp__XeCryptSha)
{
    static thread_local uint32_t scratch = SystemAlloc(0x58);
    ShaInit(scratch);
    if (ctx.r3.u32 != 0)
        ShaUpdate(scratch, GuestPtr<const uint8_t>(ctx.r3.u32), ctx.r4.u32);
    if (ctx.r5.u32 != 0)
        ShaUpdate(scratch, GuestPtr<const uint8_t>(ctx.r5.u32), ctx.r6.u32);
    if (ctx.r7.u32 != 0)
        ShaUpdate(scratch, GuestPtr<const uint8_t>(ctx.r7.u32), ctx.r8.u32);
    ShaFinal(scratch, GuestPtr<uint8_t>(ctx.r9.u32), ctx.r10.u32);
}

// --- Big numbers -------------------------------------------------------------
//
// XeCryptBnQw* numbers are arrays of 64-bit words in guest (big-endian) byte
// order, least significant word first. Only what RSA signature checks need.

namespace
{
    // Little-endian 32-bit limbs.
    using BigNum = std::vector<uint32_t>;

    BigNum LoadQwNe(uint32_t address, uint32_t qwords)
    {
        BigNum value(qwords * 2);
        for (uint32_t i = 0; i < qwords; i++)
        {
            uint64_t qword = GuestRef<uint64_t>(address + i * 8);
            value[i * 2] = uint32_t(qword);
            value[i * 2 + 1] = uint32_t(qword >> 32);
        }
        return value;
    }

    void StoreQwNe(uint32_t address, const BigNum& value, uint32_t qwords)
    {
        for (uint32_t i = 0; i < qwords; i++)
        {
            uint64_t low = i * 2 < value.size() ? value[i * 2] : 0;
            uint64_t high = i * 2 + 1 < value.size() ? value[i * 2 + 1] : 0;
            GuestRef<uint64_t>(address + i * 8) = (high << 32) | low;
        }
    }

    // a >= b, for equal-length numbers.
    bool GreaterOrEqual(const BigNum& a, const BigNum& b)
    {
        for (size_t i = a.size(); i-- > 0;)
        {
            if (a[i] != b[i])
                return a[i] > b[i];
        }
        return true;
    }

    void SubtractInPlace(BigNum& a, const BigNum& b)
    {
        uint64_t borrow = 0;
        for (size_t i = 0; i < a.size(); i++)
        {
            uint64_t difference = uint64_t(a[i]) - b[i] - borrow;
            a[i] = uint32_t(difference);
            borrow = (difference >> 32) & 1;
        }
    }

    // (a * b) mod m, with a, b < m. Reduces the double-width product one bit
    // at a time: slow per bit, but fine for the few multiplies a small
    // public exponent needs.
    BigNum MulMod(const BigNum& a, const BigNum& b, const BigNum& m)
    {
        size_t n = m.size();
        BigNum product(n * 2, 0);
        for (size_t i = 0; i < n; i++)
        {
            uint64_t carry = 0;
            for (size_t j = 0; j < n; j++)
            {
                uint64_t t = uint64_t(a[i]) * b[j] + product[i + j] + carry;
                product[i + j] = uint32_t(t);
                carry = t >> 32;
            }
            product[i + n] = uint32_t(carry);
        }

        // One spare limb so the shifted remainder can't overflow.
        BigNum remainder(n + 1, 0);
        BigNum modulus(m);
        modulus.push_back(0);
        for (size_t bit = n * 64; bit-- > 0;)
        {
            uint32_t carry = (product[bit / 32] >> (bit % 32)) & 1;
            for (size_t i = 0; i < remainder.size(); i++)
            {
                uint32_t next = remainder[i] >> 31;
                remainder[i] = (remainder[i] << 1) | carry;
                carry = next;
            }
            if (GreaterOrEqual(remainder, modulus))
                SubtractInPlace(remainder, modulus);
        }
        remainder.pop_back();
        return remainder;
    }
}

// void XeCryptBnQw_SwapDwQwLeBe(PQWORD in, PQWORD out, DWORD qwords)
// Byte-swaps each 64-bit word.
PPC_FUNC(__imp__XeCryptBnQw_SwapDwQwLeBe)
{
    for (uint32_t i = 0; i < ctx.r5.u32; i++)
    {
        uint64_t value = *GuestPtr<uint64_t>(ctx.r3.u32 + i * 8);
        *GuestPtr<uint64_t>(ctx.r4.u32 + i * 8) = ByteSwap(value);
    }
}

// BOOL XeCryptBnQwNeRsaPubCrypt(PQWORD in, PQWORD out, PXECRYPT_RSA key)
// out = in ^ e mod n. XECRYPT_RSA: DWORD qwords, DWORD public exponent,
// QWORD reserved, then the modulus (qwords long).
PPC_FUNC(__imp__XeCryptBnQwNeRsaPubCrypt)
{
    uint32_t key = ctx.r5.u32;
    uint32_t qwords = GuestRef<uint32_t>(key + 0);
    uint32_t exponent = GuestRef<uint32_t>(key + 4);
    if (qwords == 0 || qwords > 64 || exponent == 0)
    {
        ctx.r3.u64 = 0;
        return;
    }

    BigNum modulus = LoadQwNe(key + 0x10, qwords);
    BigNum message = LoadQwNe(ctx.r3.u32, qwords);
    if (GreaterOrEqual(message, modulus))
    {
        ctx.r3.u64 = 0;
        return;
    }

    BigNum result(modulus.size(), 0);
    result[0] = 1;
    for (int bit = 31; bit >= 0; bit--)
    {
        result = MulMod(result, result, modulus);
        if ((exponent >> bit) & 1)
            result = MulMod(result, message, modulus);
    }
    StoreQwNe(ctx.r4.u32, result, qwords);
    ctx.r3.u64 = 1;
}

namespace
{
    // PKCS#1 v1.5 signature block for a SHA-1 hash, in the "DwLe" layout
    // (the big-endian block byte-reversed): hash, DigestInfo (unless
    // withDigestInfo is false), 00, FF padding, 01, 00.
    void FormatPkcs1(const uint8_t* hash, bool withDigestInfo, uint8_t* out, uint32_t size)
    {
        constexpr uint8_t kSha1DigestInfo[] = { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E,
            0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14 };
        memset(out, 0xFF, size);
        out[size - 1] = 0x00;
        out[size - 2] = 0x01;
        for (uint32_t i = 0; i < 20; i++)
            out[i] = hash[19 - i];
        uint32_t next = 20;
        if (withDigestInfo)
        {
            for (uint32_t i = 0; i < sizeof(kSha1DigestInfo); i++)
                out[next + i] = kSha1DigestInfo[sizeof(kSha1DigestInfo) - 1 - i];
            next += sizeof(kSha1DigestInfo);
        }
        out[next] = 0x00;
    }
}

// BOOL XeCryptBnDwLePkcs1Verify(PBYTE hash, PBYTE signature, DWORD size)
// Checks a decrypted signature against the PKCS#1 block for a SHA-1 hash.
PPC_FUNC(__imp__XeCryptBnDwLePkcs1Verify)
{
    const uint8_t* hash = GuestPtr<const uint8_t>(ctx.r3.u32);
    const uint8_t* signature = GuestPtr<const uint8_t>(ctx.r4.u32);
    uint32_t size = ctx.r5.u32;
    if (size < 64 || size > 512)
    {
        ctx.r3.u64 = 0;
        return;
    }

    uint8_t expected[512];
    bool valid = false;
    for (bool withDigestInfo : { true, false })
    {
        FormatPkcs1(hash, withDigestInfo, expected, size);
        valid = valid || memcmp(expected, signature, size) == 0;
    }
    ctx.r3.u64 = valid;
}
