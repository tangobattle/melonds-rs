// MSVC compatibility shims for GCC/Clang-isms in the core, force-included
// via /FI so no upstream source needs editing. Only takes effect for MSVC
// proper; clang-cl defines the builtins natively and skips all of this.
#pragma once

// ARMJIT_Memory.h includes windows.h; without this, its min/max macros
// shred every later std::min/std::max call.
#if defined(_MSC_VER) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#if defined(_MSC_VER) && !defined(__clang__) && defined(__cplusplus)

#include <bit>
#include <cstdlib>
#include <intrin.h>

// Attribute droppings. The core uses both spellings; all uses are
// optimization hints (always_inline, packed is not used in core).
#define __attribute(x)
#define __attribute__(x)

// <bit> replacements. Arguments in the core are all unsigned 32/64-bit.
#define __builtin_ctz(x) ((int)std::countr_zero((unsigned int)(x)))
#define __builtin_ctzll(x) ((int)std::countr_zero((unsigned long long)(x)))
#define __builtin_clzll(x) ((int)std::countl_zero((unsigned long long)(x)))
#define __builtin_popcount(x) ((int)std::popcount((unsigned int)(x)))

#define __builtin_unreachable() __assume(0)

#endif
