/*
 * BootShim for Samsung Galaxy J2 Pro 2018 (SM-J250F / j2y18lte)
 * Qualcomm MSM8917 (Snapdragon 425) - ARMv7-A / AArch32
 */

.code 32
.text

.globl _header

_header:
  /* Standard ARM Linux zImage header for bootloader compatibility */
  mov   r0, r0
  mov   r0, r0
  mov   r0, r0
  mov   r0, r0
  mov   r0, r0
  mov   r0, r0
  mov   r0, r0
  mov   r0, r0
  b     _start
  .word 0x016f2818      /* zImage Magic number */
  .word 0x00000000      /* start address */
  .word 0x00300100      /* zImage end address: BootShim (256B) + UEFI_SIZE (3MB) */

_start:
  adr   r0, _Payload    /* Load staged payload address (PC + 0xC8 = 0x100) */
  ldr   r1, UefiBase    /* Destination base (0x99000000) */
  cmp   r0, r1          /* Check if already at destination */
  beq   _FlushAndRun
  ldr   r2, UefiSize    /* Size of UEFI FD to copy (0x00300000) */

_CopyLoop:
  ldr   r3, [r0], #4
  str   r3, [r1], #4
  subs  r2, r2, #4
  bgt   _CopyLoop

_FlushAndRun:
  /* Clean Data Cache by MVA to Point of Coherency (PoC) for the copied region */
  ldr   r1, UefiBase
  ldr   r2, UefiSize

_CleanLoop:
  mcr   p15, 0, r1, c7, c10, 1   /* DCCMVAC: Clean data cache line by MVA to PoC */
  add   r1, r1, #64              /* Cortex-A53 L1 D-cache line size = 64 bytes */
  subs  r2, r2, #64
  bgt   _CleanLoop

  /* Data Synchronization Barrier */
  dsb   sy

  /* Invalidate entire instruction cache to Point of Unification */
  mov   r0, #0
  mcr   p15, 0, r0, c7, c5, 0    /* ICIALLU: Invalidate all instruction caches */
  isb   sy

  /* Jump to UEFI entry point in ARM mode */
  ldr   r5, UefiBase
  bx    r5

UefiBase:
  .word UEFI_BASE

UefiSize:
  .word UEFI_SIZE

.text
.align 8

_Payload:
  /* EDK2 FD payload is concatenated directly at offset 256 (0x100) */
