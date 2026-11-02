Historical EP0 checkpoint. See [current ADB build](../AdbDebug/BUILD_STATUS.md).

# Experimental USB build

ARM32 DEBUG Project Mu build passed on 2026-11-02. Android packaging was
checked against the exact BootShim, firmware and QCDT inputs. Client packet
decoding/rejection checks passed. USB enumeration and eMMC hardware behavior
remain unverified.

Image SHA-256: `b6d197afc9698f6933ac9eb2764627334a217c24589044b86f9741ff1d6ed8c1`

Fixes: remove unsupported 144kHz retry, explicitly select USB configuration,
separate IN/OUT descriptors onto 64-byte cache lines, and require endpoint
flush or completed reset before freeing reserved DMA pages.
