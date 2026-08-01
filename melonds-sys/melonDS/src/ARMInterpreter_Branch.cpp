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

#include "ARM.h"
#include "NDS.h"
#include "ARMInterpreter_Branch.h"
#include "Platform.h"

namespace melonDS::ARMInterpreter
{
using Platform::Log;
using Platform::LogLevel;


template<class T> void A_B(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    s32 offset = (s32)(cpu->CurInstr << 8) >> 6;
    cpu->JumpTo(cpu->R[15] + offset);
}

template<class T> void A_BL(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    s32 offset = (s32)(cpu->CurInstr << 8) >> 6;
    cpu->R[14] = cpu->R[15] - 4;
    cpu->JumpTo(cpu->R[15] + offset);
}

template<class T> void A_BLX_IMM(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    s32 offset = (s32)(cpu->CurInstr << 8) >> 6;
    if (cpu->CurInstr & 0x01000000) offset += 2;
    cpu->R[14] = cpu->R[15] - 4;
    cpu->JumpTo(cpu->R[15] + offset + 1);
}

template<class T> void A_BX(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    cpu->JumpTo(cpu->R[cpu->CurInstr & 0xF]);
}

template<class T> void A_BLX_REG(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    u32 lr = cpu->R[15] - 4;
    cpu->JumpTo(cpu->R[cpu->CurInstr & 0xF]);
    cpu->R[14] = lr;
}



template<class T> void T_BCOND(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    if (cpu->CheckCondition((cpu->CurInstr >> 8) & 0xF))
    {
        s32 offset = (s32)(cpu->CurInstr << 24) >> 23;
        cpu->JumpTo(cpu->R[15] + offset + 1);
    }
    else
        cpu->AddCycles_C();
}

template<class T> void T_BX(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    cpu->JumpTo(cpu->R[(cpu->CurInstr >> 3) & 0xF]);
}

template<class T> void T_BLX_REG(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    if (cpu->Num==1)
    {
        Log(LogLevel::Warn, "!! THUMB BLX_REG ON ARM7\n");
        return;
    }

    u32 lr = cpu->R[15] - 1;
    cpu->JumpTo(cpu->R[(cpu->CurInstr >> 3) & 0xF]);
    cpu->R[14] = lr;
}

template<class T> void T_B(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    s32 offset = (s32)((cpu->CurInstr & 0x7FF) << 21) >> 20;
    cpu->JumpTo(cpu->R[15] + offset + 1);
}

template<class T> void T_BL_LONG_1(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    s32 offset = (s32)((cpu->CurInstr & 0x7FF) << 21) >> 9;
    cpu->R[14] = cpu->R[15] + offset;
    cpu->AddCycles_C();
}

template<class T> void T_BL_LONG_2(ARM* cpu_)
{
    T* cpu = static_cast<T*>(cpu_);
    s32 offset = (cpu->CurInstr & 0x7FF) << 1;
    u32 pc = cpu->R[14] + offset;

    if ((cpu->Num==1) || (cpu->CurInstr & (1<<12))) // BL
    {
        pc |= 1;
    }
    else // BLX
    {
        if (cpu->CurInstr & 1) // lsb of immediate is set, implying halfword offset; this raises undefined.
            return T_UNK(cpu);

        // instruction always switches to arm mode
        // interworking bit should be cleared.
        pc &= ~1;
    }

    cpu->R[14] = (cpu->R[15] - 2) | 1;
    cpu->JumpTo(pc);
}





#define MELONDS_INSTANTIATE(name)        \
    template void name<ARMv5>(ARM* cpu); \
    template void name<ARMv4>(ARM* cpu);
MELONDS_BRANCH_NAMES(MELONDS_INSTANTIATE)
#undef MELONDS_INSTANTIATE

#ifdef JIT_ENABLED
#define MELONDS_DEFINE_DISPATCH(name)                       \
    void name(ARM* cpu)                                     \
    {                                                       \
        if (cpu->Num == 0) name<ARMv5>(cpu);                \
        else               name<ARMv4>(cpu);                \
    }
MELONDS_BRANCH_NAMES(MELONDS_DEFINE_DISPATCH)
#undef MELONDS_DEFINE_DISPATCH
#endif

}

