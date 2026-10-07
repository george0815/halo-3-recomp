#pragma once

struct PPCContext;

namespace h3::kernel
{
    // Fallback for every import without a real implementation yet. The
    // generated weak stubs (H3RecompLib/ppc/import_stubs.cpp) call this.
    // Logs the import once and returns 0 in r3, or aborts if the
    // H3_STRICT_IMPORTS environment variable is set.
    void UnimplementedImport(const char* name, PPCContext& ctx);
}
