# J250F read-only ext4 filesystem driver

This ARM32 EDK II DXE driver exposes ext4 GPT partitions as standard
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` volumes, so applications can
`OpenVolume`, `Open`, `Read`, and enumerate directories. The only storage
method referenced is `EFI_BLOCK_IO_PROTOCOL.ReadBlocks`. All writes,
metadata mutations, file creation and deletion are denied. No journal replay,
recovery, repair, mount-state update, or block switching is attempted.

The J250F platform loads the driver in DXE APRIORI and, after publishing
the existing read-only eMMC disk, runs a recursive `ConnectController` so
`DiskIoDxe`, `PartitionDxe`, and this driver can attach *before* the
long-running firmware ADB session. The GPT driver provides bounded logical
Block I/O child handles. All ext4 reads are relative to those handles, not
the raw user-area disk.

For the debugging interface (when a new image is actually booted):

```sh
adb -s J250F-UEFI shell ext4 volumes
adb -s J250F-UEFI shell ext4 ls 0 /
adb -s J250F-UEFI shell ext4 ls 0 /recovery
adb -s J250F-UEFI shell ext4 cat 0 /example.txt
```

Numbers are indices of ext4 volumes *in the current boot*, **not GPT
partition numbers**. The commands browse mounted ext4 volumes only, not FAT or
the firmware FV. `cat` intentionally emits at most 1KiB of sanitized text;
use a separate EFI file reader if you need a binary export. Non-ASCII names are
shown as `?` and cannot be opened through this initial ASCII-only path layer.

Supported: ext4 with 1/2/4KiB blocks, extent trees, sparse/unwritten extents,
regular files and directories, classic direct/single/double/triple-indirect
block maps, large inode sizes, group descriptors, and 64-bit block addresses.
Unrecognized incompat features, bigalloc, inline data, encryption, special
file access and invalid directory entries are refused. Metadata checksums
and journal consistency are **not** verified or replayed. This is a bounded
inspection driver, not a write-capable or general-purpose production ext4
implementation. An unclean filesystem may expose inconsistent data.

There is no proof of successful phone mounting until a new firmware image is
built, booted and `ext4 volumes` shows a volume. This commit changes firmware
source, not the phone's actual eMMC sectors.
