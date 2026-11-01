# Current integration validation

- Active package integrated in the real J250F DSC/FDF and built with Project Mu,
  ARM DEBUG CLANG38. Includes PE/COFF DXE modules for the RPM power provider
  and eMMC backend/frontend. This supersedes the imported archive's test status.
- `make test arm syntax sanitizer` passed locally: current recognition path,
  RPM transport/provider, target-info/GCD adapter, actual-Mu-header frontend
  integration, freestanding ARM compilation, and ASan/UBSan checks.
- Package contains old upstream test fixtures as references. The current Makefile
  selects the integration regressions; it does not claim the earlier 20MHz,
  CMD6/8-bit, write-enabled, notification, or fake-power handoff configurations.
- No physical success established. Previous image versions failed CMD0 or power
  handshakes. Passing these checks is NOT proof that this image fixes the phone.
- Output remains read-only; sector-0 must actually read before exposing a disk.
- Platform SMEM/GCC/TLMM/RPM access depends on hardware/bootloader handoff and
  is not validated by a host simulator. No sleep/suspend or OS driver support.

Latest hardware report: the DLL-correction image still fails CMD2 with command
CRC IRQ=0x00028000. This is an unresolved development checkpoint, not a working
eMMC release. See Documentation/HARDWARE_RESULTS.md.
