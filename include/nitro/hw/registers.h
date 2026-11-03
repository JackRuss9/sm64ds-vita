#ifndef NITRO_HW_REGISTERS_H
#define NITRO_HW_REGISTERS_H

#include "types.h"

/*
 * Nintendo DS ARM9 memory-mapped I/O used by the current decomp.
 *
 * Keep these as volatile lvalues or simple address expressions. Some matched
 * functions depend on the exact source expression used to materialize an MMIO
 * address, so higher-level helpers belong beside the functions that need them.
 */

/*
 * Every address below was read out of the cartridge, not looked up: the object
 * file's relocation says which word of a function carries the register address,
 * and the retail word at that offset holds it. See notes/phantom-references.md.
 */

/* 2D display engines */
#define REG_DISPCNT      (*(volatile u32 *)0x04000000)
#define REG_DISPCNT_SUB  (*(volatile u32 *)0x04001000)

#define DISPLAY_CONTROL_BG_EXT_PALETTE  (1u << 30)
#define DISPLAY_CONTROL_OBJ_EXT_PALETTE (1u << 31)

/* BG control: engine A's BG0CNT..BG3CNT are 0x04000008..0x0400000e, engine B's
   0x04001008..0x0400100e. Bits 0-1 are the priority and bit 6 the mosaic enable. */
#define REG_BG1CNT    (*(volatile u16 *)0x0400000a)
#define REG_BG2CNT    (*(volatile u16 *)0x0400000c)
#define REG_BG3CNT    (*(volatile u16 *)0x0400000e)
#define REG_DB_BG0CNT (*(volatile u16 *)0x04001008)
#define REG_DB_BG1CNT (*(volatile u16 *)0x0400100a)
#define REG_DB_BG2CNT (*(volatile u16 *)0x0400100c)
#define REG_DB_BG3CNT (*(volatile u16 *)0x0400100e)

/* Colour special effects (alpha blending and brightness), one per engine. */
#define REG_BLDCNT    (*(volatile u16 *)0x04000050)
#define REG_DB_BLDCNT (*(volatile u16 *)0x04001050)

#define REG_VCOUNT (*(volatile u16 *)0x04000006)

/* BG0HOFS is 0x04000010 and BG0VOFS 0x04000012; the ROM sets the pair with one
   32-bit store, so this is typed to match that access rather than split in two. */
#define REG_BG0OFS (*(volatile u32 *)0x04000010)

/* Engine B's BG3 affine parameters. Taken by address, not assigned: G2x_SetBGyAffine
   walks PA/PB/PC/PD from the pointer it is handed. */
#define REG_DB_BG3PA ((volatile u16 *)0x04001030)

/* Timer 0. The counter and the control word share an address: reads see the live
   count, writes set the reload value. */
#define REG_TM0CNT_L (*(volatile u16 *)0x04000100)

/* Memory control */
#define REG_EXMEMCNT (*(volatile u16 *)0x04000204)
#define REG_WRAMCNT  (*(volatile u8  *)0x04000247)

/* Hardware square root. SQRTCNT bit 15 is the busy flag; the ROM spins on it
   through a pointer, so the address form is what that loop needs. */
#define REG_SQRTCNT     (*(volatile u16 *)0x040002b0)
#define REG_SQRT_RESULT (*(volatile s32 *)0x040002b4)

#define SQRT_CONTROL_BUSY (1u << 15)

/* Power control: bit 0 is the master enable for the 2D/3D hardware. */
#define REG_POWCNT1 (*(volatile u16 *)0x04000304)

#define POWER_CONTROL_ENABLE (1u << 0)

/* DMA channels: source, destination, and control are three u32 registers each. */
#define REG_DMA_BASE ((volatile u32 *)0x040000b0)
#define REG_DMA_CNT_PTR(channel) \
    (REG_DMA_BASE + ((channel) * 3 + 2))
#define REG_DMA_FILL_ADDR(channel) \
    ((u32)0x040000e0 + ((channel) << 2))

#define DMA_CONTROL_SRC_FIXED (1u << 24)
#define DMA_CONTROL_32_BIT    (1u << 26)
#define DMA_CONTROL_ENABLE    (1u << 31)

/* Interrupt controller */
#define REG_IE (*(volatile u32 *)0x04000210)
#define REG_IF (*(volatile u32 *)0x04000214)

#define IRQ_GAME_CARD (1u << 19)


/*
 * Hosts that cannot map the DS regions at their real addresses (the PS Vita
 * port: NTR_RELOCATE_REGIONS) resolve every address above through
 * ntr_host_ptr (port/ntr/io.cpp). The DS definitions above are left exactly
 * as written -- matched functions depend on their spelling -- and are only
 * replaced here, for those hosts. Generated from the definitions above.
 */
#if defined(SM64DS_PLATFORM_PC) && defined(NTR_RELOCATE_REGIONS)
#ifdef __cplusplus
extern "C" void *ntr_host_ptr(unsigned int ds_address);
#else
void *ntr_host_ptr(unsigned int ds_address);
#endif
#undef REG_DISPCNT
#define REG_DISPCNT (*((volatile u32 *)ntr_host_ptr(0x04000000)))
#undef REG_DISPCNT_SUB
#define REG_DISPCNT_SUB (*((volatile u32 *)ntr_host_ptr(0x04001000)))
#undef REG_BG1CNT
#define REG_BG1CNT (*((volatile u16 *)ntr_host_ptr(0x0400000a)))
#undef REG_BG2CNT
#define REG_BG2CNT (*((volatile u16 *)ntr_host_ptr(0x0400000c)))
#undef REG_BG3CNT
#define REG_BG3CNT (*((volatile u16 *)ntr_host_ptr(0x0400000e)))
#undef REG_DB_BG0CNT
#define REG_DB_BG0CNT (*((volatile u16 *)ntr_host_ptr(0x04001008)))
#undef REG_DB_BG1CNT
#define REG_DB_BG1CNT (*((volatile u16 *)ntr_host_ptr(0x0400100a)))
#undef REG_DB_BG2CNT
#define REG_DB_BG2CNT (*((volatile u16 *)ntr_host_ptr(0x0400100c)))
#undef REG_DB_BG3CNT
#define REG_DB_BG3CNT (*((volatile u16 *)ntr_host_ptr(0x0400100e)))
#undef REG_BLDCNT
#define REG_BLDCNT (*((volatile u16 *)ntr_host_ptr(0x04000050)))
#undef REG_DB_BLDCNT
#define REG_DB_BLDCNT (*((volatile u16 *)ntr_host_ptr(0x04001050)))
#undef REG_VCOUNT
#define REG_VCOUNT (*((volatile u16 *)ntr_host_ptr(0x04000006)))
#undef REG_BG0OFS
#define REG_BG0OFS (*((volatile u32 *)ntr_host_ptr(0x04000010)))
#undef REG_DB_BG3PA
#define REG_DB_BG3PA (((volatile u16 *)ntr_host_ptr(0x04001030)))
#undef REG_TM0CNT_L
#define REG_TM0CNT_L (*((volatile u16 *)ntr_host_ptr(0x04000100)))
#undef REG_EXMEMCNT
#define REG_EXMEMCNT (*((volatile u16 *)ntr_host_ptr(0x04000204)))
#undef REG_WRAMCNT
#define REG_WRAMCNT (*((volatile u8  *)ntr_host_ptr(0x04000247)))
#undef REG_SQRTCNT
#define REG_SQRTCNT (*((volatile u16 *)ntr_host_ptr(0x040002b0)))
#undef REG_SQRT_RESULT
#define REG_SQRT_RESULT (*((volatile s32 *)ntr_host_ptr(0x040002b4)))
#undef REG_POWCNT1
#define REG_POWCNT1 (*((volatile u16 *)ntr_host_ptr(0x04000304)))
#undef REG_DMA_BASE
#define REG_DMA_BASE (((volatile u32 *)ntr_host_ptr(0x040000b0)))
#undef REG_IE
#define REG_IE (*((volatile u32 *)ntr_host_ptr(0x04000210)))
#undef REG_IF
#define REG_IF (*((volatile u32 *)ntr_host_ptr(0x04000214)))
#endif /* NTR_RELOCATE_REGIONS */

#endif /* NITRO_HW_REGISTERS_H */
