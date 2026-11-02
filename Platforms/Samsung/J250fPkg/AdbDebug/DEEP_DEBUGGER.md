# J250F general-purpose ADB firmware debugger

This is a **UEFI DXE debugger** accessible via the phone's existing ADB
transport; it is **not** Android/Linux and cannot run Linux shell commands.
It exposes generic EFI state instead of adding a new command for every issue.

## First checks

```sh
adb -s J250F-UEFI shell help
adb -s J250F-UEFI shell dbg help
adb -s J250F-UEFI shell gop status
adb -s J250F-UEFI shell gop repair
adb -s J250F-UEFI shell gop status
adb -s J250F-UEFI shell dbg handles 0
adb -s J250F-UEFI shell dbg locate 9042a9de-23dc-4a38-96fb-7aded080516a
```

**GOP fix:** The original GOP module published its vendor DevicePath before
GOP in one `InstallMultipleProtocolInterfaces` call. EDK II refuses an
already-installed device path with `EFI_ALREADY_STARTED` and does not reach
GOP. The revised driver uses a J250F-specific vendor device-path GUID, and
installs GOP **first on its own**, making the device-path registration
independent. A device-path collision can no longer block GOP.

## Generic read-only commands

| Command | Purpose |
| --- | --- |
| `dbg help` | Current debugger command discovery |
| `dbg system` | EFI system, boot, runtime pointers, revisions |
| `dbg handles 0` | All EFI handles with 0-based indices and protocol counts |
| `dbg protocols 15 0` | Every protocol GUID and interface on handle #15 |
| `dbg locate GUID` | Any GUID: LocateProtocol and the handles publishing it |
| `dbg tables 0` | EFI configuration-table GUIDs and pointers |
| `dbg images 0` | EFI loaded image bases, sizes and parent handles |
| `dbg disks` | Block I/O handles and media attributes, no writes |
| `dbg memory 0` | UEFI memory map, types, attributes, physical ranges |
| `dbg ram 0xADDR 64` | Bounded read in EFI-managed non-MMIO RAM |
| `dbg fb 0 64` | Bounded read from inherited 24-bit display memory |
| `dbg mmio 0x01a05014 1` | Bounded read from selected J250F hardware registers |
| `dbg vars 0` | Read-only enumeration of UEFI variable names and vendor GUIDs |

Handle listings, protocol arrays, memory maps, EFI tables, images and
variable listings are paged. Increase the last index by eight to continue.

## Extensibility

The existing firmware loader supports up to 16 resident EFI image slots.
For new operations that cannot be represented as read-only shell commands,
build an **ARM32 UEFI PE/COFF module** with EDK II, then upload and run it:

```sh
adb -s J250F-UEFI push custom-debug.efi /ram/custom-debug.efi
adb -s J250F-UEFI shell efi run /ram/custom-debug.efi
```

The loaded module can use native UEFI services and registers, rather than
requiring the firmware shell to ship one command for every API. It must be
developed with care: **uploaded native code is not sandboxed** and can
circumvent the built-in read-only protections. In particular, never access
or modify the physical eMMC GPT, EFS, modem, or Samsung bootloader partitions.
Only the previously authorized physical partitions `boot`, `system`,
`userdata` may ever be changed, if explicitly requested.

## Security and hardware limitations

The current ADB service is unauthenticated. Keep this debugging firmware on
an isolated, physically controlled development phone. Do not connect it to
unknown hosts; it gives deep firmware visibility. There is deliberately no
generic register-write, physical storage-write, or raw arbitrary MMIO command
in the built-in shell. Arbitrary MMIO **reads** can themselves hang a
clock-gated device; the built-in command restricts the ranges and counts.

A successful firmware build does not prove GOP scanout or Windows Phone boot.
After `gop status` reports a handle, run `gop test`, watch the screen, then
validate native BGRX separately. The 24-bit fallback reports `format=3`
(`PixelBltOnly`); Windows requires physical 32-bit BGRX, `format=1`.
