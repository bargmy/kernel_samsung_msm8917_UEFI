# J250F experimental ESP-only normal-BDS boot

**Hardware risk: experimental. UEFI isolation is not Windows kernel isolation.**
No physical eMMC partitions may be modified. Virtual writes return
`EFI_WRITE_PROTECTED`. The first test intentionally has **no MainOS or Data
virtual partitions**; do not infer that Windows kernel boot is impossible.

## Boot sequence

1. J250F eMMC DXE initializes the existing read-only physical disk.
2. It connects existing partition/ext4 drivers only to find GPT `userdata`.
3. It reads **only** `/wp81/26.EFIESP.fat` (exactly 33,554,432 bytes)
   and saves ext4 inode data for subsequent private reads.
4. It disconnects the physical disk's public EFI Block I/O, Disk I/O and
   GPT partition interfaces, failing closed if anything still remains.
5. It publishes a CRC-checked protective MBR, primary/backup GPT and
   exactly one read-only FAT16 partition named `EFIESP`.
6. It confirms the topology and that the virtual FAT ESP contains
   `\\Windows\\BOOT\\EFI\\BOOTMGFW.EFI` (the donor's actual ARM32
   Windows Boot Manager). This skips `BOOTARM.EFI`, which otherwise
   tries to locate `bootmgfw.efi` using a different path.
7. It installs a UEFI `BootFF10` option pointing to that file and a
   one-entry `BootOrder`. The platform has
   `PcdEmuVariableNvModeEnable=TRUE`, so boot variables are emulated.
8. It stops ADB and **returns successfully from DXE**. Project Mu
   finishes DXE, signals EndOfDxe and enters its ordinary BDS boot flow.
   BDS then attempts `BootFF10` through standard UEFI image loading.

During this ESP-only experiment, after the virtual ESP and BootFF10
are successfully prepared, the driver publishes an **in-memory
ESP-ready protocol**. The Qualcomm DeviceBootManagerLib checks this
protocol rather than inferring the mode from the mutable BootOrder
variable. If the marker is present, BDS **does not register** the generic
Internal Storage and USB options; these instead run MsBootPolicy.efi
and probe \EFI\Microsoft\Boot\bootmgfw.efi, a path absent from the donor.
The DEBUG log prints the protocol marker status and BootOrder/BootFF10
variable values so boot-order rewrites and firmware mismatches can be seen.
Without the marker, normal default boot registration is unchanged.

The driver intentionally does **not** call `LoadImage` or `StartImage`
inside DXE entry. This matters because SecurityStubDxe can defer
third-party EFI images loaded before EndOfDxe, returning
`EFI_ACCESS_DENIED`.

## Fail-closed diagnostic fallback

If the private file staging, physical-interface hiding, virtual topology
check, file existence check, or boot-option registration fails, the firmware
does **not** return to BDS. Instead the existing ADB diagnostic loop
remains available. This prevents normal BDS enumeration of the physical
Samsung eMMC on a failed preparation path.

## Requirements and limitations

- Only `/wp81/26.EFIESP.fat` must exist; no MainOS/Data files are needed.
- This is **UEFI protocol hiding only**, not SDCC hardware access control.
- Windows Boot Manager may still touch hardware or attempt later boot stages.
  The absence of MainOS/Data is not a hard safety barrier.
- UEFI disk and GPT structures are never written to physical eMMC.
- The secure-image authentication policy is **not bypassed**.
- The software must be validated in both ARM32 DEBUG and RELEASE builds
  and tested on hardware with a backup/recovery path before relying on it.

## November ESP-only BDS error diagnostics

The J250F reported `BootFF10 attempted: YES` followed by
`EFI_ACCESS_DENIED` (0x8000000F). This proves that Project Mu attempted
the boot option, but **not** whether Windows Boot Manager executed.

To avoid guessing, `MsBootPolicyLib.LocalLoadImage` now records only
for the verified WP81 virtual-disk device-path GUID:
- whether the BDS LoadImage policy wrapper was reached;
- whether the platform path policy explicitly rejected the load;
- the status returned by the underlying UEFI `LoadImage` call.

The J250F framebuffer diagnostic screen now distinguishes denial by the
platform path policy, denial by the underlying image loader, and a successful
`LoadImage` followed by a failed `StartImage`/application return. No policy
is disabled and the ESP and backing eMMC are still read-only.
The `BootOrder` read now uses a bounded 64-entry buffer instead of a
single `UINT16`; the previous `Buffer Too Small` line was a diagnostic
buffer-sizing bug and did not itself explain the boot failure.

Use a DEBUG build of this exact PR commit to read the framebuffer messages.
The underlying cause of `EFI_ACCESS_DENIED` is still to be confirmed on hardware.

## BDS EndOfDxe fix for Access Denied

The J250F reported `BootFF10 attempted: YES`, `Policy rejected: NO`,
then `EFI_ACCESS_DENIED`. In the Project Mu SecurityStubDxe source, the
`Defer3rdPartyImageLoad` handler explicitly returns `EFI_ACCESS_DENIED`
when loading a filesystem image **before** the EndOfDxe event. This Qcom
BDS port did not previously signal EndOfDxe in its platform BDS entry.

When (and only when) the verified in-memory WP81 ESP-ready marker exists,
`DeviceBootManagerBdsEntry` now signals `gEfiEndOfDxeEventGroupGuid`
**before** normal boot option dispatch. This is a phase-transition fix,
not an authentication bypass. Windows Boot Manager is still loaded through
Project Mu's normal `LoadImage` and signature policy. DEBUG reports the
EndOfDxe signal in firmware logs. The status of this fix on real hardware
remains **unverified** until the next test.
