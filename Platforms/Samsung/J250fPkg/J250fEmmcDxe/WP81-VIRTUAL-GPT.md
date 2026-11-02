# Windows Phone 8.1: read-only virtual GPT prototype

This implements **only the UEFI-side boot experiment** on the Samsung
SM-J250F. It does NOT modify any partition or provide Windows storage
isolation after `ExitBootServices`.

## Why it is opt-in

The existing firmware publishes read-only physical eMMC and ext4. The
`wp mount` command looks ONLY for the physical GPT **userdata** partition,
requires the ext4 read-only marker, and then opens three **existing**
read-only files under `/wp81`:

| Path in userdata ext4 | Required exact byte length | Virtual partition |
| --- | ---: | --- |
| `/wp81/26.EFIESP.fat` | 33,554,432 | `EFIESP` |
| `/wp81/27.MainOS.ntfs` | 2,648,719,360 | `MainOS` |
| `/wp81/28.Data.ntfs` | 4,552,916,992 | `Data` |

**The firmware cannot create or copy these files.** They must be staged by a
separate, independently reviewed recovery/host process after backing up data.
No files are currently assumed to exist. In particular, this change never
formats userdata, changes the real GPT, flashes an image, or calls WriteBlocks.

The virtual Block I/O device synthesizes a 512-byte-sector protective MBR,
primary/backup GPT headers and entry arrays with valid CRC32 checksums,
1 MiB-aligned partitions, and zero-filled gaps. Reads from virtual partitions
are served from their file's `SetPosition`/`Read`. All virtual writes
return `EFI_WRITE_PROTECTED`. Reads are limited to the exact file size and
virtual disk length; file sizes and FAT/NTFS boot sectors are checked.
`PartitionDxe` and the FAT driver can discover the synthetic ESP.

The generated GPT uses the BLU disk GUID printed by 7-Zip but synthesizes
**different partition GUIDs** and 1 MiB-aligned addresses. It does NOT
reproduce the complete BLU GPT or its unique partition IDs. BCD references
to donor-specific GUIDs may therefore fail: provide the donor GPT metadata
for a fidelity update rather than guessing. Optional DPP/PLAT partitions are
not included in this initial boot test.

## Commands (on new firmware image)

```sh
adb -s J250F-UEFI shell wp status
adb -s J250F-UEFI shell wp mount
adb -s J250F-UEFI shell wp probe
adb -s J250F-UEFI shell wp boot CONFIRM
```

`wp mount` only installs a virtual disk when all three files exist, their
sizes match, and the physical userdata is exposed as read-only ext4.
`wp probe` attempts UEFI `LoadImage` for standard ARM32 Windows Boot
Manager paths on the virtual ESP and immediately unloads it; **it does not
execute the image or boot Windows**.

`wp boot CONFIRM` explicitly calls `StartImage`. This is *dangerous*:
Windows may take control of hardware and its own storage controller after
ExitBootServices, with potential to write physical eMMC outside userdata.
**Do not run it while physical storage is accessible to an untrusted Windows
driver if retaining Samsung partitions is a hard safety requirement.**
There is currently no Windows boot-critical virtual storage driver, no
IOMMU/MMIO enforcement, and no physical eMMC isolation.

Even when `wp probe` succeeds, this proves only that UEFI located and
loaded an EFI image; it does not establish Windows bootability, compatibility
of the BLU ACPI tables, NTFS reading, signing policies or BCD correctness.

## Validation

Both DEBUG and RELEASE ARM32 firmware builds must pass in GitHub Actions
before this prototype is merged into `main`. Mounting and booting require
the actual phone and staged files; CI cannot prove those runtime conditions.
