# Samsung J250F SDCC1 platform port

Status: partial source port, linked into the J250F recognition build. Not a complete
UEFI or Windows Phone driver. Do not interpret host simulations as device tests.

Primary source: bargmy/android_kernel_samsung_msm8917_forOneUI,
commit d8e6e0d79bf4e26a11d5f69c843a6ad096b2b181, branch lineage-19.1.

## Ported and compiled for ARM32

- SDCC1 GCC root rate: `clock-gcc-8952.c` (matches `qcom,gcc-8917`)
  `ftbl_gcc_sdcc1_apps_clk`: F(400000, xo, 12, 1, 4).
- M/N/D programming and update: `clock-local2.c::__set_rate_mnd` and
  `rcg_update_config`. Linux spinlocks are unnecessary for this standalone
  firmware library's serialized initialization; bounded timeout is returned.
- Registers and F macro: `msm-clocks-hwio-8952.h`; GCC base from J250F DTS.
- GCC AHB and APPS branch enable/halt polling.
- SDCC1 active pads: `pinctrl-msm8917.c`, generic field encoding from
  `pinctrl-msm.c`, drive/pull values from revision-04 J250F DTS.

These are GPL-2.0-only adaptations, with upstream copyright and COPYING retained.
They do not write eMMC card commands or sectors.

## Not ported yet

- RPM-SMD transport and SMEM channel access. DTS names `rpm_requests`, edge 15.
  Linux `drivers/soc/qcom/rpm-smd.c` uses request/message headers and KVPs;
  `smd.c`, `smd_private.c`, `smem.c` implement the underlying transport.
- RPM regulator voting and response handling. DTS maps eMMC VDD to
  pm8917_l8 (resource ldoa, ID 8, 2.85–2.9 V, init 2.9 V) and VQMMC
  to pm8917_l5 (ldoa, ID 5, fixed 1.8 V). Read the exact source properties
  when implementing requests; do not infer PMIC register writes from IDs.
- VDD_DIG and interconnect/RPM bus votes required by the Linux clock path.
- Complete BUS_ON/OFF and IO_LOW/HIGH sequencing after successful regulator
  operations, then controller acknowledgment. Do not ACK success early.
- Reset/timing integration with the read-only Block I/O implementation,
  failure unwinding, and a physical device validation.

The existing ClockDxe has an unpopulated static msm_clk_list; it is not an
implementation of these J250F clock tables. The current provisional Block I/O
module inherits rails/clocks and does not fulfill the dependencies above.

## Validation run

Host test: `tests/j250f_sdcc_platform_test.c` checks exact F macro values,
branch enables, TLMM field preservation, and failures when updates/halt state
never complete. ARM32 cross-compilation checks EDK2 types/calling convention.
A recognition build now uses these local clock and pad functions.
RPM/PMIC/interconnect prerequisites are still inherited from aboot.

    gcc -std=c11 -DMDEPKG_NDEBUG -fshort-wchar \
      -I Mu_Basecore/MdePkg/Include -I Mu_Basecore/MdePkg/Include/X64 \
      -I Silicon/Qualcomm/QcomPkg/Include \
      tests/j250f_sdcc_platform_test.c -o /tmp/j250f-sdcc-platform-test
    /tmp/j250f-sdcc-platform-test

    clang -target arm-linux-gnueabihf -march=armv7-a -mthumb \
      -fshort-wchar -ffreestanding -Wall -Werror \
      -I Mu_Basecore/MdePkg/Include -I Mu_Basecore/MdePkg/Include/Arm \
      -I Silicon/Qualcomm/QcomPkg/Include \
      -c GPLDrivers/Library/J250fSdccPlatformLib/J250fSdccPlatformLib.c \
      -o /tmp/j250f-sdcc-platform-arm.obj
