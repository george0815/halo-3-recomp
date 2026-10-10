#include <ppc_recomp_shared.h>

#include "memory.h"
#include "gpu/command_processor.h"
#include "gpu/renderer.h"
#include "kernel/module.h"
#include "kernel/modules.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"
#include "video/window.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

// The recompiler's function lookup table lives right after the image and
// must fit inside the 4 GiB guest range.
static_assert(PPC_IMAGE_BASE + PPC_IMAGE_SIZE + PPC_CODE_SIZE * 2 <= h3::kGuestMemorySize);

int main(int argc, char* argv[])
{
    // Point this at default_patched.xex when recompiling with a title update.
    const char* xexPath = argc > 1 ? argv[1] : "private/default.xex";

    // game:\ is the disc contents: private/game/ next to the XEX (see
    // private/README.md), or the XEX's own directory if it's in there. The
    // hard drive (cache partitions, saves) lives in hdd/ next to the XEX.
    std::filesystem::path xexDir = std::filesystem::path(xexPath).parent_path();
    h3::kernel::SetContentRoot(std::filesystem::is_directory(xexDir / "game") ? xexDir / "game" : xexDir);
    std::filesystem::create_directories(xexDir / "hdd");
    h3::kernel::SetUserDataRoot(xexDir / "hdd");

    // On the console Halo 3 reads its cache header from the raw partition,
    // mounts its own cache file system and links cache0:/cache1: to it. We
    // don't emulate the raw partition, so back the caches with host folders
    // and link them up front.
    for (int i = 0; i < 2; i++)
    {
        std::string cache = "Cache" + std::to_string(i);
        std::filesystem::create_directories(xexDir / "hdd" / cache);
        h3::kernel::AddSymbolicLink("cache" + std::to_string(i) + ":", "\\Device\\Harddisk0\\" + cache);
    }

    // Compiled host shaders, kept between runs.
    h3::gpu::renderer::SetShaderCacheDirectory(xexDir / "shadercache");

    if (!h3::InitGuestMemory())
    {
        fprintf(stderr, "Failed to reserve guest memory\n");
        return 1;
    }

    if (!h3::video::CreateMainWindow())
        return 1;

    h3::gpu::Initialize();

    if (!h3::kernel::LoadExecutable(xexPath))
        return 1;

    // The guest's main thread runs beside the window's event loop, which SDL
    // wants on the host main thread.
    std::thread([]
    {
        // Halo 3's header asks for a 64 KiB main stack. Give it at least 1 MiB:
        // the headroom is free, and an overflow would silently corrupt memory.
        PPCContext ctx{};
        h3::kernel::AdoptHostThread(ctx, std::max(h3::kernel::g_module.stackSize, 0x100000u));

        printf("Entering guest at _xstart\n");
        fflush(stdout);
        _xstart(ctx, h3::g_base);
        printf("Guest returned\n");
        h3::video::RequestQuit(0);
    }).detach();

    // Guest threads can't be stopped cleanly, so leave without running
    // destructors once the window closes.
    int exitCode = h3::video::RunEventLoop();
    fflush(stdout);
    fflush(stderr);
    std::_Exit(exitCode);
}
