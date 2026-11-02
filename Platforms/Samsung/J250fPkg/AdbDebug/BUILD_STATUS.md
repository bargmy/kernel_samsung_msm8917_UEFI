# Persistent ADB experimental build

ARM32 DEBUG Project Mu build passed on 2026-11-02. Checked the actual EFI
module/application machine types and subsystems, boot header, BootShim, full
firmware payload, QCDT and Samsung footer. Host protocol interoperability uses
Google adb with a TCP harness. USB register/DMA tests use mocks; the phone has
not been tested with this build.

Image SHA-256: `39b75497bf5a5eb7a048f2aa652e3331cf81160e46ae76fb968d0dd397ac67d9`

Passed locally:
- Real adb connection, one-shot and interactive shell, multi-frame SYNC upload,
  packet fragmentation and malformed headers; UBSan enabled.
- Actual USB code header/payload splitting, descriptor configuration, reset
  fallback, retained DMA and blocked restart after failed teardown.
- Actual EFI upload and LoadImage/StartImage adapter with mock UEFI services,
  returning-application option cleanup, resident unload and rejected images.
- Existing RPM/SMEM/read-only storage tests and persistent-session exit tests.
- ARM compilation, ASan/UBSan host checks, full Mu build and image packaging.

Unverified: physical USB bulk transfers, Windows WinUSB/ADB binding, running
uploaded ARM32 EFI code on the phone, and eMMC recognition. Startup still
requires the existing GOP/CPU/power DEPEX and bootloader USB PHY/clock handoff.
