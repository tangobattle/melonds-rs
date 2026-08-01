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

#ifndef ARMINTERPRETER_LOADSTORE_H
#define ARMINTERPRETER_LOADSTORE_H

#include "ARMInterpreter_LoadStore_Names.h"

namespace melonDS::ARMInterpreter
{

// Every load and store is templated on the CPU that runs it.
//
// The interpreter reaches memory through `ARM`, which used to mean a
// virtual call and now means a branch on `ARM::Num` — and either way
// the caller cannot see which of the two implementations it is about
// to enter. These are the instructions that make that call, several
// times over for a block transfer, and between them they were about a
// tenth of a tick in dispatch alone. Instantiated once per CPU class,
// each copy calls one implementation directly and inlines it.
//
// `ARMInterpreter.cpp` binds the two instantiations into two
// instruction tables; ARMv5::Execute and ARMv4::Execute each dispatch
// through their own. Everything else in those tables is the one shared
// handler, as before.

#define A_PROTO_WB_LDRSTR(x) \
\
template<class T> void A_##x##_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_LSL(ARM* cpu); \
template<class T> void A_##x##_REG_LSR(ARM* cpu); \
template<class T> void A_##x##_REG_ASR(ARM* cpu); \
template<class T> void A_##x##_REG_ROR(ARM* cpu); \
template<class T> void A_##x##_POST_IMM(ARM* cpu); \
template<class T> void A_##x##_POST_REG_LSL(ARM* cpu); \
template<class T> void A_##x##_POST_REG_LSR(ARM* cpu); \
template<class T> void A_##x##_POST_REG_ASR(ARM* cpu); \
template<class T> void A_##x##_POST_REG_ROR(ARM* cpu);

A_PROTO_WB_LDRSTR(STR)
A_PROTO_WB_LDRSTR(STRB)
A_PROTO_WB_LDRSTR(LDR)
A_PROTO_WB_LDRSTR(LDRB)

#define A_PROTO_HD_LDRSTR(x) \
\
template<class T> void A_##x##_IMM(ARM* cpu); \
template<class T> void A_##x##_REG(ARM* cpu); \
template<class T> void A_##x##_POST_IMM(ARM* cpu); \
template<class T> void A_##x##_POST_REG(ARM* cpu);

A_PROTO_HD_LDRSTR(STRH)
A_PROTO_HD_LDRSTR(LDRD)
A_PROTO_HD_LDRSTR(STRD)
A_PROTO_HD_LDRSTR(LDRH)
A_PROTO_HD_LDRSTR(LDRSB)
A_PROTO_HD_LDRSTR(LDRSH)

template<class T> void A_LDM(ARM* cpu);
template<class T> void A_STM(ARM* cpu);

template<class T> void A_SWP(ARM* cpu);
template<class T> void A_SWPB(ARM* cpu);


template<class T> void T_LDR_PCREL(ARM* cpu);

template<class T> void T_STR_REG(ARM* cpu);
template<class T> void T_STRB_REG(ARM* cpu);
template<class T> void T_LDR_REG(ARM* cpu);
template<class T> void T_LDRB_REG(ARM* cpu);

template<class T> void T_STRH_REG(ARM* cpu);
template<class T> void T_LDRSB_REG(ARM* cpu);
template<class T> void T_LDRH_REG(ARM* cpu);
template<class T> void T_LDRSH_REG(ARM* cpu);

template<class T> void T_STR_IMM(ARM* cpu);
template<class T> void T_LDR_IMM(ARM* cpu);
template<class T> void T_STRB_IMM(ARM* cpu);
template<class T> void T_LDRB_IMM(ARM* cpu);

template<class T> void T_STRH_IMM(ARM* cpu);
template<class T> void T_LDRH_IMM(ARM* cpu);

template<class T> void T_STR_SPREL(ARM* cpu);
template<class T> void T_LDR_SPREL(ARM* cpu);

template<class T> void T_PUSH(ARM* cpu);
template<class T> void T_POP(ARM* cpu);
template<class T> void T_STMIA(ARM* cpu);
template<class T> void T_LDMIA(ARM* cpu);

#ifdef JIT_ENABLED
// The JIT interprets what it does not compile, from a table it
// builds once for both CPUs — so it keeps a form of each handler
// that decides at run time, the way every caller did before.
#define MELONDS_DECLARE_DISPATCH(name) void name(ARM* cpu);
MELONDS_LOADSTORE_NAMES(MELONDS_DECLARE_DISPATCH)
#undef MELONDS_DECLARE_DISPATCH
#endif

}

#endif

