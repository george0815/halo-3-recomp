#pragma once

// Force-included into the recompiled game code before ppc_context.h: a jump
// table index outside its table aborts with the table's address and the
// index, instead of being undefined behaviour (which shows up later as
// corrupted host stacks). Implemented in H3Recomp/kernel/imports.cpp.

#include <cstdint>

[[noreturn]] void H3SwitchUnreachable(uint32_t address, uint32_t value);

#define PPC_SWITCH_UNREACHABLE(address, value) H3SwitchUnreachable(address, value)
