/***************************************************************************************
* Copyright (c) 2014-2024 Zihao Yu, Nanjing University
*
* NEMU is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
*
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*
* See the Mulan PSL v2 for more details.
***************************************************************************************/

#ifndef __ISA_RISCV_H__
#define __ISA_RISCV_H__

#include <common.h>

typedef struct {
  word_t gpr[MUXDEF(CONFIG_RVE, 16, 32)];
  vaddr_t pc;
  // CSR registers
  word_t mstatus;
  word_t mtvec;
  word_t mscratch;
  word_t mepc;
  word_t mcause;
  word_t mie;
  word_t mip;
} MUXDEF(CONFIG_RV64, riscv64_CPU_state, riscv32_CPU_state);

// CSR addresses
#define CSR_MSTATUS   0x300
#define CSR_MTVEC     0x305
#define CSR_MSCRATCH  0x340
#define CSR_MEPC      0x341
#define CSR_MCAUSE    0x342
#define CSR_MIE       0x304
#define CSR_MIP       0x344

// mstatus bits
#define MSTATUS_MIE   (1 << 3)   // Machine Interrupt Enable
#define MSTATUS_MPIE  (1 << 7)   // Machine Previous Interrupt Enable

// mie/mip bits
#define MIP_MTIP      (1 << 7)   // Machine Timer Interrupt Pending
#define MIE_MTIE      (1 << 7)   // Machine Timer Interrupt Enable

// decode
typedef struct {
  uint32_t inst;
} MUXDEF(CONFIG_RV64, riscv64_ISADecodeInfo, riscv32_ISADecodeInfo);

#define isa_mmu_check(vaddr, len, type) (MMU_DIRECT)

#endif
