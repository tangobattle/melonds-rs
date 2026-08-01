/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef ARMINTERPRETER_LOADSTORE_NAMES_H
#define ARMINTERPRETER_LOADSTORE_NAMES_H

// Every handler in ARMInterpreter_LoadStore, as a list.
//
// `MELONDS_LOADSTORE_NAMES(F)` expands `F(name)` once per handler. Three
// places need the list — the explicit instantiations, the per-CPU
// instruction tables, and (with the JIT built in) the dispatching forms
// its fallback interpreter keeps — and reading it from here is what
// stops them drifting apart. The groupings mirror the prototype macros
// in ARMInterpreter_LoadStore.h; a handler added there and not here is
// a link error, not a silent omission.

#define MELONDS_WB_LDRSTR_NAMES(F, x)                                     \
    F(A_##x##_IMM) F(A_##x##_REG_LSL) F(A_##x##_REG_LSR)                  \
    F(A_##x##_REG_ASR) F(A_##x##_REG_ROR)                                 \
    F(A_##x##_POST_IMM) F(A_##x##_POST_REG_LSL) F(A_##x##_POST_REG_LSR)   \
    F(A_##x##_POST_REG_ASR) F(A_##x##_POST_REG_ROR)

#define MELONDS_HD_LDRSTR_NAMES(F, x)                                     \
    F(A_##x##_IMM) F(A_##x##_REG) F(A_##x##_POST_IMM) F(A_##x##_POST_REG)

#define MELONDS_LOADSTORE_NAMES(F)                                        \
    MELONDS_WB_LDRSTR_NAMES(F, STR)                                       \
    MELONDS_WB_LDRSTR_NAMES(F, STRB)                                      \
    MELONDS_WB_LDRSTR_NAMES(F, LDR)                                       \
    MELONDS_WB_LDRSTR_NAMES(F, LDRB)                                      \
    MELONDS_HD_LDRSTR_NAMES(F, STRH)                                      \
    MELONDS_HD_LDRSTR_NAMES(F, LDRD)                                      \
    MELONDS_HD_LDRSTR_NAMES(F, STRD)                                      \
    MELONDS_HD_LDRSTR_NAMES(F, LDRH)                                      \
    MELONDS_HD_LDRSTR_NAMES(F, LDRSB)                                     \
    MELONDS_HD_LDRSTR_NAMES(F, LDRSH)                                     \
    F(A_LDM) F(A_STM)                                                     \
    F(A_SWP) F(A_SWPB)                                                    \
    F(T_LDR_PCREL)                                                        \
    F(T_STR_REG) F(T_STRB_REG) F(T_LDR_REG) F(T_LDRB_REG)                 \
    F(T_STRH_REG) F(T_LDRSB_REG) F(T_LDRH_REG) F(T_LDRSH_REG)             \
    F(T_STR_IMM) F(T_LDR_IMM) F(T_STRB_IMM) F(T_LDRB_IMM)                 \
    F(T_STRH_IMM) F(T_LDRH_IMM)                                           \
    F(T_STR_SPREL) F(T_LDR_SPREL)                                         \
    F(T_PUSH) F(T_POP) F(T_STMIA) F(T_LDMIA)

#endif // ARMINTERPRETER_LOADSTORE_NAMES_H
