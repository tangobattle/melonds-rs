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

#ifndef ARMINTERPRETER_ALU_H
#define ARMINTERPRETER_ALU_H

// Every handler in this file, as a list; see
// ARMInterpreter_LoadStore_Names.h for why.
#define MELONDS_ALU_OP_NAMES(F, x) \
    F(A_##x##_IMM) F(A_##x##_REG_LSL_IMM) F(A_##x##_REG_LSR_IMM) F(A_##x##_REG_ASR_IMM) F(A_##x##_REG_ROR_IMM) F(A_##x##_REG_LSL_REG) F(A_##x##_REG_LSR_REG) F(A_##x##_REG_ASR_REG) F(A_##x##_REG_ROR_REG) F(A_##x##_IMM_S) F(A_##x##_REG_LSL_IMM_S) F(A_##x##_REG_LSR_IMM_S) F(A_##x##_REG_ASR_IMM_S) F(A_##x##_REG_ROR_IMM_S) F(A_##x##_REG_LSL_REG_S) F(A_##x##_REG_LSR_REG_S) F(A_##x##_REG_ASR_REG_S) F(A_##x##_REG_ROR_REG_S)

#define MELONDS_ALU_TEST_NAMES(F, x) \
    F(A_##x##_IMM) F(A_##x##_REG_LSL_IMM) F(A_##x##_REG_LSR_IMM) F(A_##x##_REG_ASR_IMM) F(A_##x##_REG_ROR_IMM) F(A_##x##_REG_LSL_REG) F(A_##x##_REG_LSR_REG) F(A_##x##_REG_ASR_REG) F(A_##x##_REG_ROR_REG)

#define MELONDS_ALU_NAMES(F) \
    MELONDS_ALU_OP_NAMES(F, AND) \
    MELONDS_ALU_OP_NAMES(F, EOR) \
    MELONDS_ALU_OP_NAMES(F, SUB) \
    MELONDS_ALU_OP_NAMES(F, RSB) \
    MELONDS_ALU_OP_NAMES(F, ADD) \
    MELONDS_ALU_OP_NAMES(F, ADC) \
    MELONDS_ALU_OP_NAMES(F, SBC) \
    MELONDS_ALU_OP_NAMES(F, RSC) \
    MELONDS_ALU_OP_NAMES(F, ORR) \
    MELONDS_ALU_OP_NAMES(F, MOV) \
    MELONDS_ALU_OP_NAMES(F, BIC) \
    MELONDS_ALU_OP_NAMES(F, MVN) \
    MELONDS_ALU_TEST_NAMES(F, TST) \
    MELONDS_ALU_TEST_NAMES(F, TEQ) \
    MELONDS_ALU_TEST_NAMES(F, CMP) \
    MELONDS_ALU_TEST_NAMES(F, CMN) \
    F(A_MOV_REG_LSL_IMM_DBG) F(A_MUL) F(A_MLA) F(A_UMULL) F(A_UMLAL) F(A_SMULL) F(A_SMLAL) F(A_SMLAxy) F(A_SMLAWy) F(A_SMULxy) F(A_SMULWy) F(A_SMLALxy) F(A_CLZ) F(A_QADD) F(A_QSUB) F(A_QDADD) F(A_QDSUB) F(T_LSL_IMM) F(T_LSR_IMM) F(T_ASR_IMM) F(T_ADD_REG_) F(T_SUB_REG_) F(T_ADD_IMM_) F(T_SUB_IMM_) F(T_MOV_IMM) F(T_CMP_IMM) F(T_ADD_IMM) F(T_SUB_IMM) F(T_AND_REG) F(T_EOR_REG) F(T_LSL_REG) F(T_LSR_REG) F(T_ASR_REG) F(T_ADC_REG) F(T_SBC_REG) F(T_ROR_REG) F(T_TST_REG) F(T_NEG_REG) F(T_CMP_REG) F(T_CMN_REG) F(T_ORR_REG) F(T_MUL_REG) F(T_BIC_REG) F(T_MVN_REG) F(T_ADD_HIREG) F(T_CMP_HIREG) F(T_MOV_HIREG) F(T_ADD_PCREL) F(T_ADD_SPREL) F(T_ADD_SP)

namespace melonDS
{
namespace ARMInterpreter
{

// Templated on the CPU for the reason the load/store family is;
// see ARMInterpreter_LoadStore.h. These reach memory only for
// their own cycle count, but there are a lot of them.

#define A_PROTO_ALU_OP(x) \
\
template<class T> void A_##x##_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_LSL_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_LSR_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_ASR_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_ROR_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_LSL_REG(ARM* cpu); \
template<class T> void A_##x##_REG_LSR_REG(ARM* cpu); \
template<class T> void A_##x##_REG_ASR_REG(ARM* cpu); \
template<class T> void A_##x##_REG_ROR_REG(ARM* cpu); \
template<class T> void A_##x##_IMM_S(ARM* cpu); \
template<class T> void A_##x##_REG_LSL_IMM_S(ARM* cpu); \
template<class T> void A_##x##_REG_LSR_IMM_S(ARM* cpu); \
template<class T> void A_##x##_REG_ASR_IMM_S(ARM* cpu); \
template<class T> void A_##x##_REG_ROR_IMM_S(ARM* cpu); \
template<class T> void A_##x##_REG_LSL_REG_S(ARM* cpu); \
template<class T> void A_##x##_REG_LSR_REG_S(ARM* cpu); \
template<class T> void A_##x##_REG_ASR_REG_S(ARM* cpu); \
template<class T> void A_##x##_REG_ROR_REG_S(ARM* cpu);

#define A_PROTO_ALU_TEST(x) \
\
template<class T> void A_##x##_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_LSL_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_LSR_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_ASR_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_ROR_IMM(ARM* cpu); \
template<class T> void A_##x##_REG_LSL_REG(ARM* cpu); \
template<class T> void A_##x##_REG_LSR_REG(ARM* cpu); \
template<class T> void A_##x##_REG_ASR_REG(ARM* cpu); \
template<class T> void A_##x##_REG_ROR_REG(ARM* cpu);

A_PROTO_ALU_OP(AND)
A_PROTO_ALU_OP(EOR)
A_PROTO_ALU_OP(SUB)
A_PROTO_ALU_OP(RSB)
A_PROTO_ALU_OP(ADD)
A_PROTO_ALU_OP(ADC)
A_PROTO_ALU_OP(SBC)
A_PROTO_ALU_OP(RSC)
A_PROTO_ALU_TEST(TST)
A_PROTO_ALU_TEST(TEQ)
A_PROTO_ALU_TEST(CMP)
A_PROTO_ALU_TEST(CMN)
A_PROTO_ALU_OP(ORR)
A_PROTO_ALU_OP(MOV)
A_PROTO_ALU_OP(BIC)
A_PROTO_ALU_OP(MVN)

template<class T> void A_MOV_REG_LSL_IMM_DBG(ARM* cpu);

template<class T> void A_MUL(ARM* cpu);
template<class T> void A_MLA(ARM* cpu);
template<class T> void A_UMULL(ARM* cpu);
template<class T> void A_UMLAL(ARM* cpu);
template<class T> void A_SMULL(ARM* cpu);
template<class T> void A_SMLAL(ARM* cpu);
template<class T> void A_SMLAxy(ARM* cpu);
template<class T> void A_SMLAWy(ARM* cpu);
template<class T> void A_SMULxy(ARM* cpu);
template<class T> void A_SMULWy(ARM* cpu);
template<class T> void A_SMLALxy(ARM* cpu);

template<class T> void A_CLZ(ARM* cpu);
template<class T> void A_QADD(ARM* cpu);
template<class T> void A_QSUB(ARM* cpu);
template<class T> void A_QDADD(ARM* cpu);
template<class T> void A_QDSUB(ARM* cpu);


template<class T> void T_LSL_IMM(ARM* cpu);
template<class T> void T_LSR_IMM(ARM* cpu);
template<class T> void T_ASR_IMM(ARM* cpu);

template<class T> void T_ADD_REG_(ARM* cpu);
template<class T> void T_SUB_REG_(ARM* cpu);
template<class T> void T_ADD_IMM_(ARM* cpu);
template<class T> void T_SUB_IMM_(ARM* cpu);

template<class T> void T_MOV_IMM(ARM* cpu);
template<class T> void T_CMP_IMM(ARM* cpu);
template<class T> void T_ADD_IMM(ARM* cpu);
template<class T> void T_SUB_IMM(ARM* cpu);

template<class T> void T_AND_REG(ARM* cpu);
template<class T> void T_EOR_REG(ARM* cpu);
template<class T> void T_LSL_REG(ARM* cpu);
template<class T> void T_LSR_REG(ARM* cpu);
template<class T> void T_ASR_REG(ARM* cpu);
template<class T> void T_ADC_REG(ARM* cpu);
template<class T> void T_SBC_REG(ARM* cpu);
template<class T> void T_ROR_REG(ARM* cpu);
template<class T> void T_TST_REG(ARM* cpu);
template<class T> void T_NEG_REG(ARM* cpu);
template<class T> void T_CMP_REG(ARM* cpu);
template<class T> void T_CMN_REG(ARM* cpu);
template<class T> void T_ORR_REG(ARM* cpu);
template<class T> void T_MUL_REG(ARM* cpu);
template<class T> void T_BIC_REG(ARM* cpu);
template<class T> void T_MVN_REG(ARM* cpu);

template<class T> void T_ADD_HIREG(ARM* cpu);
template<class T> void T_CMP_HIREG(ARM* cpu);
template<class T> void T_MOV_HIREG(ARM* cpu);

template<class T> void T_ADD_PCREL(ARM* cpu);
template<class T> void T_ADD_SPREL(ARM* cpu);
template<class T> void T_ADD_SP(ARM* cpu);


#ifdef JIT_ENABLED
#define MELONDS_DECLARE_DISPATCH(name) void name(ARM* cpu);
MELONDS_ALU_NAMES(MELONDS_DECLARE_DISPATCH)
#undef MELONDS_DECLARE_DISPATCH
#endif
}

}
#endif
