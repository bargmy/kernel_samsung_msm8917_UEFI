# BOOTMGFW reboot investigation

## Verified hardware result (build 65)

The raw boot partition matched the published DEBUG image SHA256:
`720b1d55233474f51bb09c117fedb89ce2d43ab1ac0060aaca89684985098642`.
After preparing `/wp81/uefi.log` and cleanly unmounting userdata, the next run
persisted **100 checksum-valid V2 records**, 11,044 bytes of text. There were
zero invalid records and the early queue reported zero dropped bytes.

Relevant output:

```text
J250F MDP32: inherited 24-bit source not uniquely identified (0)
J250F GOP: MDP32 unavailable, retaining RGB888 BLT scanout (Not Found)
J250F GOP: GOP=Success ... handle=A55D9290 ... fmt=3
... GopOverrideDxe.efi ...
J250F GOP: fallback=1, before=Not Found install=Success after=Success
... AcpiPlatform.efi ... start failed: Aborted
GetPlatformPreferredConsole - Unable to locate platform preferred console
DisplayBootGraphic - Failed to find GOP on ConsoleOutHandle
UpdateFacsHardwareSignature Unable to locate FADT
... Loading driver ... bootmgfw.efi
ERROR [OSK]: Failed to find Simple Window Manager protocol (Not Found).
```

The user confirms a reboot after those lines. They do **not** establish whether
the application entrypoint executes, the reset caller, an exception location,
or watchdog expiry. In particular, `Loading driver ...` is printed by
`CoreLoadPeImage`, not after successful application startup.

## Concrete graphics defects repaired

- GOP publishes vendor path `75c8e1a4-5e0b-49b5-b223-6f2edac31954`, but BDS's
  preferred display path used the generic GOP protocol GUID. Both now use a
  shared J250F identity; other platforms keep their previous default.
- Mu's GopOverrideDxe removes the standard GOP from the original handle and
  expects its RenderingEngine to replace it. The log demonstrates a second GOP
  being published by the eMMC fallback while the original is hidden.
- J250F now keeps its real GOP unfiltered. This avoids that duplicate and lets
  the standard GraphicsConsole connect to the matching path. It also avoids
  connecting Mu's compositor to a PixelBltOnly mode whose FrameBufferBase is
  zero: the compositor's surface checksum code directly dereferences the base.
- This does **not** fabricate a 32-bit framebuffer, relax the native-MDP probe,
  or establish Windows-compatible native graphics. The known 24-bit display is
  retained; missing SWM can remain a warning.

## Discriminating handoff trace

While persistent logging is active and the verified WP81 marker is present,
resident wrappers log LoadImage/StartImage begin and return, ExitBootServices
entry/failure, and pre-OS ResetSystem type/status/caller. They forward all calls
unchanged, including resets and image-authentication failures. Both service-table
CRCs are updated and rollback is tested. Hooks are restored at EBS, with no disk
I/O or allocation in that callback.

The checked Basecore patch also logs immediately before an EFI application's
entrypoint branch and after a normal return. Thus the next log can distinguish
load completion, CoreStartImage preparation, application entry, reset requests,
and OS handoff. IRQ-masked/high-TPL failures can still leave queued text unsaved;
absence of a reset marker alone does not prove a hardware reset.

## ACPI is not implemented

All directives in the active J250F ACPI include are commented out, and there is
no MSM8917 ACPI table directory in this checkout. The installed AcpiPlatformDxe
cannot locate its storage container, consistent with the abort. Uncommenting the
old list is not a fix: it references Huawei ATU-L21 tables, not a reviewed Samsung
table set. No fake/donor ACPI tables are enabled by this change. Missing ACPI is
a real platform limitation, but is not yet proven to cause this specific reset.

## Next test

Host regression suites, default/J250F console identity tests, handoff forwarding,
CRC rollback, EBS restoration, and ARM compilation pass. The CI build must also
succeed before using the new image. Hardware verification of these repairs is
pending. Back up the exact existing boot image before any explicitly authorized
flash. Keep the existing log (it appends), cleanly unmount userdata, and inspect
the next `[WP81 HANDOFF]` records. No additional image-signature bypass or physical
storage exposure is introduced.
