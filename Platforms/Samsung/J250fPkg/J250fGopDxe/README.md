# Samsung J250F GOP: working 24-bit path + experimental native 32-bit MDP

**Branch:** `experimental/j250f-gop-safe` (draft PR #14).

The older 24-bit GOP works on the physical J250F. This branch keeps that
working path; it does **not** try to set the old RGB888 pointer to 32-bit or
rewrite Samsung's DSI panel initialization.

## Native 32-bit BGRX attempt

J250F's original Samsung DTS describes a **24-bit DSI video panel** but its
Qualcomm MDSS MDP can fetch a **32-bit BGRX8888 source framebuffer**. The panel
link and memory framebuffer pixel formats are different things.

On GOP initialization, the new `J250fMdp32.c`:

1. **Reads first; no blind writes.** Matches exactly one active full-screen,
   video-mode MDP pipe displaying the known 540x960 RGB888 source at
   `0xA800035A`, with expected source stride, unpack and BPP.
2. Verifies exactly one applicable MDP CTL and refuses ambiguous or unsupported
   modes (including UBWC, tiled input and command-mode panels).
3. Reserves a distinct 540x960x4 buffer at `0xA8300000` via
   `AllocatePages(AllocateAddress, EfiReservedMemoryType,...)`. This is already
   listed as DRAM in the J250F memory map; the reservation must succeed.
4. Copies the working 24-bit splash into **B, G, R, X** physical pixel order.
5. Updates **only that identified MDP source** to a real BGRX8888 fetch, with
   corresponding stride and address, readback verification and CTL flush.
6. Publishes GOP `PixelBlueGreenRedReserved8BitPerColor` with
   `FrameBufferBase=0xA8300000` and a full nonzero `FrameBufferSize`.

If probe or allocation fails, the firmware **remains in the old working
packed-RGB888 mode** (GOP `PixelBltOnly`) without touching MDP registers.
If register readback fails, the driver restores the original source registers
and keeps the 24-bit GOP.

This 32-bit path is **not yet hardware validated**; the controller may use
another pipe/address/format than the known framebuffer pointer, or need an
additional platform-specific MDP command. The original 24-bit fallback protects
early firmware usability, but an unsuccessful MDP switch can temporarily blank
the screen. Keep a working firmware and a recovery/ADB path.

## Verify before claiming Windows graphics works

The self-test is a standalone ARM32 EFI application built but not embedded
into the phone image:

```sh
adb push J250fGopTest.efi /ram/J250fGopTest.efi
adb shell efi run /ram/J250fGopTest.efi
```

It draws RGB test bars, reads them back, and restores the original scanlines.
It returns **`EFI_UNSUPPORTED`** if the final GOP is still `PixelBltOnly`
or lacks a physical BGRX8888 framebuffer. The `EFI_SUCCESS` result means
software BLT + GOP memory layout checks passed; **you must also visually
verify that bars appeared and the panel remained alive**. A readback alone
cannot prove that the MDP is genuinely scanning out from the new buffer.

The original `FrameBufferSerialPortLib` continues logging to the **old**
24-bit buffer, rather than corrupting the new 32-bit scanout. Windows'
firmware-to-OS handoff still requires disabling framebuffer logging before
launching Windows boot files. All other boot blockers (ACPI, OS loader and
storage continuity) remain outside this patch.

## Changes and risk

- J250F GOP driver + Qualcomm MDP BGRX handoff and self-test.
- J250F DSC / APRIORI / DXE select this module.
- Generic `QcomPkg/SimpleFbDxe`, all eMMC code, RPM, storage and GPT untouched.
- No full ARM32 Mu build or J250F phone test has been performed in this
  session. **Do not merge as confirmed working before testing on the phone.**
