# J250F DEBUG RAM EFI execution fix

The ADB shell calls LoadImage(FALSE, Parent, NULL, Buffer, Size, ...).
The platform links DxeImageVerificationLib into SecurityStubDxe, so these RAM
images go through signature verification even though they have no device path.
That verification can return EFI_ACCESS_DENIED before the PE loader is reached.
The prior shell incorrectly described every LoadImage error as an ARM32 issue.
The reported phone error alone does not prove the value of SecureBoot or which
verification check rejected the image.

This development build explicitly permits nonempty buffer-backed image loads
with File==NULL and BootPolicy==FALSE in the verification handler. The J250F DSC
sets J250F_ADB_RAM_EXECUTION only for DEBUG. Other image origins continue through
the existing handler; RELEASE does not enable this exception. The exception is
for all matching RAM callers, not cryptographically restricted to the ADB caller.
This intentionally permits unsigned RAM EFI programs in the development firmware.
The DXE core still performs PE/COFF format, architecture, allocation, relocation
and executable loading checks. No direct jump or manual PE loader is used.
No SecureBoot variables/keys are changed, and no policy hook is replaced at runtime.

The diagnostic session runs inside a DXE driver before EndOfDxe. This fix does
not signal EndOfDxe early: SecurityStub's built-in deferral already exempts NULL
device paths. The normal file-loading lifecycle remains unchanged.

The HC2 initialization workaround is included. EFI hello tests do not access eMMC.

After booting Mu-J250F-EFI-FIX.img:

```sh
adb -s J250F-UEFI shell emmc status
adb -s J250F-UEFI push J250fAdbHello.efi /ram/hello.efi
adb -s J250F-UEFI shell efi run /ram/hello.efi
adb -s J250F-UEFI shell emmc status
```

Expected: StartImage returned: Success. The hello message appears on the phone
console and may not be forwarded through ADB. No eMMC initialization retry is
needed for this test. If another status appears, preserve it verbatim.

Build from a clean dependency checkout: apply MuPatches/J250F_ADB_RAM_Execution.patch
with git -C Common/Mu_Tiano_Plus apply ../../MuPatches/J250F_ADB_RAM_Execution.patch
before invoking stuart_build. build_uefi.sh applies/checks the patch automatically.
The source ZIP also contains the exact modified verification source.

Validation: host tests with mocked UEFI image services, ARM syntax checks,
ASan/UBSan (leak detection disabled under ptrace), full ARM32 DEBUG Mu build,
compiled security-policy presence, PE headers for the sample app, boot header,
BootShim entry and size, exact FD payload, QCDT and Samsung footer.
Execution on the physical phone remains to be confirmed.
