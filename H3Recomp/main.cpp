#include <ppc_recomp_shared.h>

#include "memory.h"

#include <file.h>
#include <image.h>

#include <cstdio>
#include <cstring>

// The recompiler's function lookup table lives right after the image and
// must fit inside the 4 GiB guest range.
static_assert(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + PPC_CODE_SIZE * 2 <= h3::kGuestMemorySize);

static bool LoadImage(const char* path)
{
    std::vector<uint8_t> file = LoadFile(path);
    if (file.empty())
    {
        fprintf(stderr, "Failed to read %s\n", path);
        return false;
    }

    Image image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        fprintf(stderr, "Failed to parse %s\n", path);
        return false;
    }

    // The recompiled code hardcodes addresses, so the XEX must be the exact
    // one that was fed to XenonRecomp.
    if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
    {
        fprintf(stderr, "%s does not match the recompiled executable "
            "(base 0x%zX size 0x%X, expected base 0x%llX size 0x%llX)\n",
            path, image.base, image.size, PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
        return false;
    }

    memcpy(h3::g_base + image.base, image.data.get(), image.size);

    for (PPCFuncMapping* mapping = PPCFuncMappings; mapping->host != nullptr; mapping++)
        PPC_LOOKUP_FUNC(h3::g_base, mapping->guest) = mapping->host;

    return true;
}

int main(int argc, char* argv[])
{
    // Point this at default_patched.xex when recompiling with a title update.
    const char* xexPath = argc > 1 ? argv[1] : "private/default.xex";

    if (!h3::InitGuestMemory())
    {
        fprintf(stderr, "Failed to reserve guest memory\n");
        return 1;
    }

    if (!LoadImage(xexPath))
        return 1;

    PPCContext ctx{};
    ctx.r1.u64 = h3::kMainStackBase + h3::kMainStackSize - 0x100;

    // TODO: r13 points at the processor control region (KPCR). Games read
    // TLS, the current thread and stack bounds from it. Fill it in following
    // Xenia's X_KPCR layout before expecting much to run.
    ctx.r13.u64 = h3::kPcrAddress;

    printf("Entering guest at _xstart\n");
    _xstart(ctx, h3::g_base);
    printf("Guest returned\n");

    return 0;
}
