// xboxkrnl.exe exports. Each PPC_FUNC(__imp__Name) defined here overrides the
// generated weak stub of the same name. Xenia's src/xenia/kernel/xboxkrnl/ is
// the reference for what each export is expected to do.

#include <ppc_recomp_shared.h>

#include "../memory.h"

#include <cstdio>

// void DbgPrint(const char* format, ...)
// Prints the raw format string only; enough to see what the game is saying.
PPC_FUNC(__imp__DbgPrint)
{
    fprintf(stderr, "[guest] %s", h3::GuestPtr<const char>(ctx.r3.u32));
    ctx.r3.u64 = 0;
}
