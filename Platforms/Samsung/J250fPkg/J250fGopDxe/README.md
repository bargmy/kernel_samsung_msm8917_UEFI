# Samsung J250F inherited-framebuffer GOP (experimental)

**Scope:** This branch changes the J250F UEFI GOP driver and includes a separate
non-destructive EFI graphics self-test. It does **not** touch eMMC, GPT,
partition layout, RPM, Samsung display controller registers, or Windows files.

## Physical configuration and limits

The J250F DSC currently specifies a Samsung bootloader-inherited **540x960,
RGB888 packed 24-bit** buffer, 540 pixels/line, starting at physical
`0xA800035A`. The board memory map reserves `0xA8000000..0xA82FFFFF`.
The actual base/format/active panel scanout must be confirmed on the phone.

This driver deliberately exposes an accurate **`PixelBltOnly`** GOP for the
24-bit scanout and implements BLT video fill, buffer-to-video,
video-to-buffer, and overlapping video-to-video copy, with bounds checking.
The 24-bit path has no advertised linear 32-bit framebuffer. This is correct
for the reported 24-bit format but **does not satisfy Windows' BGRX linear
framebuffer requirement**. It is primarily an EFI graphics and boot-manager
bring-up milestone, *not* proof of a Windows-ready GOP.

For native 32-bit scanout, **first** measure/reconfigure the MSM8917 MDP to
scan out a genuine word-aligned 32-bit physical buffer, and update the J250F
PCDs (BPP, base, stride) to match. Only if the actual byte order is B,G,R,X
should the build set `J250F_GOP_NATIVE_BGRX=1` for this module. Do not change
only the PCD to 32 or claim the 24-bit buffer is 32-bit: that can corrupt
neighboring pixels or display memory. Changing these PCDs alone cannot change
MDP hardware scanout. This driver never guesses or reprograms MDP registers.

## Build integration

- `J250FPkg/Include/APRIORI.inc` and `DXE.inc` use this DXE instead of the
  shared `QcomPkg/Drivers/SimpleFbDxe`.
- `J250FPkg/J250F.dsc` builds the new DXE. The shared Qualcomm source is
  unchanged for other boards.
- `J250fGopTest.inf` is **compiled, not embedded into the firmware**. Its
  standalone ARM32 EFI executable can be uploaded with the existing ADB
  diagnostics flow after the image has been built.

The existing framebuffer-based SerialPortLib still writes diagnostic text to
the physical buffer. Windows handoff requires stopping such writes before the
boot application takes ownership.

## Phone verification

With the experimental firmware, capture the serial/framebuffer log line
`J250F GOP: 540x960 stride=540 BPP=24 ... fmt=3 status=Success`.
Then test the separately built `J250fGopTest.efi` from the ADB shell:

```sh
adb push J250fGopTest.efi /ram/J250fGopTest.efi
adb shell efi run /ram/J250fGopTest.efi
```

The application reads the bottom 16 rows, fills three color bars, reads them
back through GOP, checks the colors and **restores the original pixels**.
A successful roundtrip verifies the GOP software path; it does **not** verify
actual MDP refresh, native 32-bit scanout, Windows graphics, or the physical
pixel channel order. An LCD observation is still needed.

**Status:** Source committed, not built with the full ARM32 Mu toolchain here,
and not tested on the J250F hardware. Keep recovery access and retain the
previous known-working firmware image until verified.
