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

#ifndef ARMINTERPRETER_BRANCH_H
#define ARMINTERPRETER_BRANCH_H

// Every handler in this file, as a list; see
// ARMInterpreter_LoadStore_Names.h for why.
#define MELONDS_BRANCH_NAMES(F) \
    F(A_B) F(A_BL) F(A_BX) F(A_BLX_REG) F(T_BCOND) F(T_BX) F(T_BLX_REG) F(T_B) F(T_BL_LONG_1) F(T_BL_LONG_2) F(A_BLX_IMM)

namespace melonDS
{
namespace ARMInterpreter
{

template<class T> void A_B(ARM* cpu);
template<class T> void A_BL(ARM* cpu);
template<class T> void A_BX(ARM* cpu);
template<class T> void A_BLX_REG(ARM* cpu);

template<class T> void T_BCOND(ARM* cpu);
template<class T> void T_BX(ARM* cpu);
template<class T> void T_BLX_REG(ARM* cpu);
template<class T> void T_B(ARM* cpu);
template<class T> void T_BL_LONG_1(ARM* cpu);
template<class T> void T_BL_LONG_2(ARM* cpu);

#ifdef JIT_ENABLED
// The JIT interprets what it does not compile, from a table it builds
// once for both CPUs — so it keeps a form of each handler that decides
// at run time, the way every caller did before.
#define MELONDS_DECLARE_DISPATCH(name) void name(ARM* cpu);
MELONDS_BRANCH_NAMES(MELONDS_DECLARE_DISPATCH)
#undef MELONDS_DECLARE_DISPATCH
#endif

}

}
#endif
