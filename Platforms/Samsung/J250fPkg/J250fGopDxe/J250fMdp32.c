/** @file
  Experimental J250F 24-bit -> native BGRX scanout handoff.

  Hardware reference:
    Samsung MSM8917 DTS: MDSS MDP 0x01A00000, RGB SSPP 0x15000/0x17000,
    VIG0 0x5000, DMA0 0x25000, CTL 0x2000/0x2200/0x2400.
    Samsung Linux mdss_mdp_hwio.h, mdss_mdp_formats.h, mdss_mdp_ctl.c.

  This function only changes an already-running, full-screen, single-pipe
  MDP video-mode splash when the source pointer, pitch and source BPP
  exactly match the known-working 24-bit J250F framebuffer. In any other
  case it returns an error WITHOUT modifying MDP registers or storage.

  The extra 32-bit framebuffer is allocated as EfiReservedMemoryType so it
  remains allocated after ExitBootServices, separately from the old
  framebuffer-based firmware debug log. Native scanout must be confirmed
  on real hardware before this can be called Windows-ready.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define J250F_MDP_BASE           0x01A00000u
#define J250F_NEW_FB_BASE        0xA8300000u
#define J250F_NEW_FB_CAPACITY    0x00300000u
#define J250F_SRC_SIZE          0x000
#define J250F_SRC_ADDR          0x014
#define J250F_SRC_YSTRIDE       0x024
#define J250F_SRC_FORMAT        0x030
#define J250F_SRC_UNPACK        0x034
#define J250F_SRC_OP_MODE       0x038
#define J250F_CTL_TOP           0x014
#define J250F_CTL_FLUSH         0x018

/* MDP_BGRX_8888, derived from Samsung's MDSS FMT_RGB_8888 and MDP
   format_setup(): 4 bytes, RGB colors 8 bits each, interleaved,
   tightly unpacked, no source alpha, and B,G,R,X order. */
#define J250F_MDP_BGRX_FORMAT    0x000236FFu
#define J250F_MDP_BGRX_UNPACK    0x03020001u

typedef struct {
  UINT32 PipeOffset;
  UINT32 FlushBit;
  UINT32 StageShift;
} J250F_MDP_PIPE;

STATIC CONST J250F_MDP_PIPE mPipes[] = {
  { 0x05000u,  0u,  0u }, /* VIG0 */
  { 0x15000u,  3u,  9u }, /* RGB0 */
  { 0x17000u,  4u, 12u }, /* RGB1 */
  { 0x25000u, 11u, 18u }  /* DMA0 */
};

STATIC CONST UINT32 mCtlOffsets[] = { 0x2000u, 0x2200u, 0x2400u };

EFI_STATUS EFIAPI
J250fTryNativeBgrx(
  IN UINT64 OldAddress,
  IN UINT32 Width,
  IN UINT32 Height,
  IN UINT32 Stride,
  OUT EFI_PHYSICAL_ADDRESS *NewAddress
  )
{
  UINTN Index;
  UINTN CtlIndex;
  UINTN MatchCount = 0;
  UINTN CtlMatchCount = 0;
  UINT32 PipeBase = 0;
  UINT32 FlushBit = 0;
  UINT32 StageShift = 0;
  UINT32 CtlBase = 0;
  UINT32 Format;
  UINT32 Unpack;
  UINT32 SourceStride;
  UINT32 SourceSize;
  UINT32 OldOpMode;
  UINT32 OldCtlTop;
  UINT32 FrameBytes;
  UINT32 OldPitch;
  UINT32 NewPitch;
  UINT8 *OldPixels;
  UINT8 *NewPixels;
  UINTN Pixel;
  EFI_PHYSICAL_ADDRESS FrameAddress;
  EFI_STATUS Status;

  if (NewAddress == NULL)
    return EFI_INVALID_PARAMETER;
  *NewAddress = 0;

  /* Restrict this experiment to the physically known J250F configuration.
     There must be no assumptions that the panel DSI link is itself 32-bit. */
  if (OldAddress != 0xA800035AULL || Width != 540 ||
      Height != 960 || Stride != 540)
    return EFI_UNSUPPORTED;

  FrameBytes = Width * Height * 4u;
  OldPitch = Stride * 3u;
  NewPitch = Stride * 4u;
  if (FrameBytes > J250F_NEW_FB_CAPACITY)
    return EFI_OUT_OF_RESOURCES;

  /* Discover the ONE SSPP actually scanning out our old RGB888 pixels.
     No register writes occur during the probe. */
  for (Index = 0; Index < sizeof(mPipes)/sizeof(mPipes[0]); ++Index) {
    UINT32 Reg = J250F_MDP_BASE + mPipes[Index].PipeOffset;
    UINT32 Src = MmioRead32(Reg + J250F_SRC_ADDR);
    if (Src != (UINT32)OldAddress)
      continue;

    SourceSize = MmioRead32(Reg + J250F_SRC_SIZE);
    Format = MmioRead32(Reg + J250F_SRC_FORMAT);
    SourceStride = MmioRead32(Reg + J250F_SRC_YSTRIDE);
    Unpack = MmioRead32(Reg + J250F_SRC_UNPACK);
    OldOpMode = MmioRead32(Reg + J250F_SRC_OP_MODE);

    if (SourceSize != (Height << 16 | Width) ||
        (SourceStride & 0xFFFFu) != OldPitch ||
        ((Format >> 9) & 3u) != 2u || /* exactly 24-bit, 3-byte source */
        (Format & (BIT31 | BIT30)) != 0 || /* no UBWC/tile */
        ((Format >> 19) & 3u) != 0u || /* single interleaved plane */
        ((Format >> 12) & 3u) != 2u || /* 3 unpacked components */
        (OldOpMode & BIT0) != 0u || /* no bandwidth compression */
        (Unpack & 0xFFFFFFu) != 0x00010002u)
      continue;

    ++MatchCount;
    PipeBase = Reg;
    FlushBit = mPipes[Index].FlushBit;
    StageShift = mPipes[Index].StageShift;
  }

  if (MatchCount != 1) {
    DEBUG((DEBUG_WARN,
      "J250F MDP32: inherited 24-bit source not uniquely identified (%u)\n",
      (UINT32)MatchCount));
    return EFI_NOT_FOUND;
  }

  /* Find the single live video-mode CTL using the pipe's stage bits.
     Unknown mixers, multiple CTLs and command-mode panels are NOT touched. */
  for (CtlIndex = 0; CtlIndex < sizeof(mCtlOffsets)/sizeof(mCtlOffsets[0]); ++CtlIndex) {
    UINT32 Base = J250F_MDP_BASE + mCtlOffsets[CtlIndex];
    UINT32 Layer0 = MmioRead32(Base);
    UINT32 Layer1 = MmioRead32(Base + 4u);
    UINT32 Top = MmioRead32(Base + J250F_CTL_TOP);

    if ((Top & BIT17) != 0u) /* command mode requires an extra DSI kickoff */
      continue;
    if (((Layer0 >> StageShift) & 7u) == 0u &&
        ((Layer1 >> StageShift) & 7u) == 0u)
      continue;

    ++CtlMatchCount;
    CtlBase = Base;
    OldCtlTop = Top;
  }

  if (CtlMatchCount != 1) {
    DEBUG((DEBUG_WARN,
      "J250F MDP32: 24-bit pipe has no unique video CTL (%u)\n",
      (UINT32)CtlMatchCount));
    return EFI_NOT_FOUND;
  }

  /* Exact address allocation protects the new framebuffer for the OS
     WITHOUT overlapping the existing 24-bit firmware log framebuffer. */
  FrameAddress = J250F_NEW_FB_BASE;
  Status = gBS->AllocatePages(
    AllocateAddress,
    EfiReservedMemoryType,
    EFI_SIZE_TO_PAGES(FrameBytes),
    &FrameAddress
    );
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_WARN, "J250F MDP32: reserved framebuffer: %r\n", Status));
    return Status;
  }

  OldPixels = (UINT8 *)(UINTN)OldAddress;
  NewPixels = (UINT8 *)(UINTN)FrameAddress;
  for (Pixel = 0; Pixel < (UINTN)Width * Height; ++Pixel) {
    /* Old RGB888 -> native Windows BGRX8888, actual physical scanout. */
    NewPixels[4u*Pixel + 0u] = OldPixels[3u*Pixel + 2u];
    NewPixels[4u*Pixel + 1u] = OldPixels[3u*Pixel + 1u];
    NewPixels[4u*Pixel + 2u] = OldPixels[3u*Pixel + 0u];
    NewPixels[4u*Pixel + 3u] = 0;
  }
  WriteBackInvalidateDataCacheRange(NewPixels, FrameBytes);

  /* Switch only the discovered existing pipe, not the DSI panel timings.
     Keep other SSPP state intact, and flush the correct CTL pipe bit. */
  MmioWrite32(PipeBase + J250F_SRC_ADDR, (UINT32)FrameAddress);
  MmioWrite32(PipeBase + J250F_SRC_YSTRIDE,
              (SourceStride & 0xFFFF0000u) | NewPitch);
  MmioWrite32(PipeBase + J250F_SRC_UNPACK, J250F_MDP_BGRX_UNPACK);
  MmioWrite32(PipeBase + J250F_SRC_FORMAT, J250F_MDP_BGRX_FORMAT);
  MemoryFence();

  if (MmioRead32(PipeBase + J250F_SRC_ADDR) != (UINT32)FrameAddress ||
      (MmioRead32(PipeBase + J250F_SRC_YSTRIDE) & 0xFFFFu) != NewPitch ||
      MmioRead32(PipeBase + J250F_SRC_FORMAT) != J250F_MDP_BGRX_FORMAT ||
      MmioRead32(PipeBase + J250F_SRC_UNPACK) != J250F_MDP_BGRX_UNPACK) {
    /* Restore the original 24-bit source before freeing the new pages. */
    MmioWrite32(PipeBase + J250F_SRC_ADDR, (UINT32)OldAddress);
    MmioWrite32(PipeBase + J250F_SRC_YSTRIDE, SourceStride);
    MmioWrite32(PipeBase + J250F_SRC_UNPACK, Unpack);
    MmioWrite32(PipeBase + J250F_SRC_FORMAT, Format);
    MemoryFence();
    MmioWrite32(CtlBase + J250F_CTL_FLUSH, (1u << FlushBit));
    (VOID)gBS->FreePages(FrameAddress, EFI_SIZE_TO_PAGES(FrameBytes));
    return EFI_DEVICE_ERROR;
  }

  (VOID)OldCtlTop;
  MmioWrite32(CtlBase + J250F_CTL_FLUSH, (1u << FlushBit));
  MemoryFence();

  DEBUG((DEBUG_WARN,
    "J250F MDP32: EXPERIMENTAL BGRX scanout at 0x%Lx, 540x960, pipe 0x%x CTL 0x%x\n",
    (UINT64)FrameAddress, PipeBase, CtlBase));

  *NewAddress = FrameAddress;
  return EFI_SUCCESS;
}
