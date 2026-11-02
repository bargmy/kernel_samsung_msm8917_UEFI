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
