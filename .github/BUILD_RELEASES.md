# Automatic J250F firmware releases

The **Build and release J250F images** workflow runs on every push to `main`,
on version tags matching `v*`, and through **Actions → Run workflow**.

Each run builds ARM32 DEBUG and RELEASE in separate jobs. Publishing waits for
both builds and image validation to pass. Each release contains four files:

- `Mu-J250F-<version>-DEBUG.img` and its SHA-256 checksum.
- `Mu-J250F-<version>-RELEASE.img` and its SHA-256 checksum.

Main/manual builds get their own prerelease named `j250f-build-<run number>`.
For named versions, push a tag such as `v1.0.0`; that version gets its own release.
Older releases remain available. Reruns can replace only the same version's
assets for the same commit. Logs are retained as Actions artifacts even on failure.

DEBUG permits unsigned RAM EFI applications in the experimental ADB loader.
RELEASE retains normal image verification. These are Project Mu UEFI boot images,
not Linux kernels. A successful build does not replace testing on the phone.

Publishing uses the automatic GitHub Actions token with `contents: write` only
in the release job. No personal token secret is needed.
