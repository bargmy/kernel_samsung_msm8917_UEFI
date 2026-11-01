# Samsung Galaxy J2 Pro 2018 (SM-J250F / j2y18lte) Hardware Memory Map Analysis

**SoC**: Qualcomm Snapdragon 425 (MSM8917)
**RAM Total**: 1.5 GiB (`0x60000000` bytes)
**RAM Base**: `0x80000000`
**RAM End**: `0xe0000000`

---

## 1. Hardware Reserved Memory Regions (Verified from Samsung Rev00-Rev04 DTS)

The following memory regions are explicitly reserved in the device tree sources across all five Samsung J2Y18LTE board revisions (`Rev00` through `Rev04`):

| Region Name | Start Address | End Address | Size (Bytes) | Size (MB) | no-map | Purpose / Impact |
|---|---|---|---|---|---|---|
| `tzapp_region` | `0x84300000` | `0x85000000` | `0x00d00000` | 13.00 MB | Yes | Samsung TrustZone / QSEE applications |
| `sec_debug_region` | `0x85000000` | `0x85800000` | `0x00800000` | 8.00 MB | Yes | Samsung Security Debug buffer |
| `klog_region` | `0x851ff000` | `0x85400000` | `0x00201000` | 2.00 MB | Yes | Kernel log buffer (carved inside `sec_debug`) |
| `ss_plog` | `0x85c00000` | `0x86000000` | `0x00400000` | 4.00 MB | Yes | Samsung persistent log buffer |
| `ramoops` | `0x86000000` | `0x86100000` | `0x00100000` | 1.00 MB | No | RAM crash oops buffer |
| `other_ext_region` | `0x86100000` | `0x86800000` | `0x00700000` | 7.00 MB | Yes | External modem / security extension |
| `modem_region` | `0x86800000` | `0x8c200000` | `0x05a00000` | 90.00 MB | No | Modem subsystem (MPSS) firmware |
| `adsp_fw_region` | `0x8c200000` | `0x8da00000` | `0x01800000` | 24.00 MB | Yes | Audio DSP firmware |
| `wcnss_fw_region`| `0x8da00000` | `0x8e100000` | `0x00700000` | 7.00 MB | Yes | Wi-Fi / Bluetooth (WCNSS) firmware |
| `lk_region` | `0x8f600000` | `0x8f900000` | `0x00300000` | 3.00 MB | Yes | Little Kernel (`aboot`) reserved scratch |
| `venus_region` | `0x8fc00000` | `0x90000000` | `0x00400000` | 4.00 MB | Yes | Venus video hardware codec engine |
| `modem_shared_mem`| `0x93000000` | `0x99000000` | `0x06000000` | 96.00 MB | Yes | Modem SMEM / shared data buffers |
| `splash_region` | `0xa8000000` | `0xa8300000` | `0x00300000` | 3.00 MB | No | Continuous splash framebuffer (`540x960` 32bpp) |

---

## 2. Samsung Bootloader (`aboot`) Loading Offsets (Verified from `boot.img` Header)

| Field | Offset / Address | Size | Description |
|---|---|---|---|
| Page Size | `2048` (`0x800`) | — | Standard Android v0 boot page size |
| Kernel Load Address | `0x80008000` | 10,464,776 bytes | Address where `aboot` copies `boot.img` kernel payload |
| Ramdisk Load Address | `0x82000000` | 1,368,482 bytes | Address where `aboot` copies ramdisk payload |
| Tags / DTB Address | `0x81e00000` | ~1.1 MiB | Address where `aboot` passes ATAGS / flattened device tree |
| QCDT Offset in File | `0x00b4a000` | 1,130,496 bytes | Appended Qualcomm QCDT table |

---

## 3. Free DRAM Zones Analysis

* **Zone 1 (`0x80000000` – `0x84300000`, 67.0 MB)**:
  * Used during initial bootloader handoff. `aboot` loads the kernel at `0x80008000`, tags at `0x81e00000`, and ramdisk at `0x82000000`.
  * BootShim executes here at `0x80008000`.
* **Zone 2 (`0x85400000` – `0x85c00000`, 8.0 MB)**:
  * Small gap between Samsung debug logs and persistent logs. Unsafe for firmware execution.
* **Zone 3 (`0x90000000` – `0x93000000`, 48.0 MB)**:
  * Gap between Venus video engine and Modem shared memory.
* **Zone 4 (`0x99000000` – `0xa8000000`, 240.0 MB)**:
  * **PRIMARY CANDIDATE**: Completely unreserved, high DRAM zone situated between Modem SMEM (`0x99000000`) and Framebuffer (`0xa8000000`).
  * Has ample room for UEFI FD (`0x99000000`–`0x99300000`, 3 MB), CPU Vectors (`0x99300000`–`0x99310000`, 64 KB), UEFI Stack (`0x99310000`–`0x99390000`, 512 KB), and DXE heap.
* **Zone 5 (`0xa8300000` – `0xe0000000`, 893.0 MB)**:
  * Upper DRAM above Framebuffer to the top of 1.5 GiB physical memory. Used for general OS memory.

---

## 4. Collision Analysis: Huawei Y6 Default vs. Samsung J250F

Huawei Y6 (`ATU-L21Pkg`) places firmware at `0x84500000`.

**Collision on Samsung J250F**:
* Huawei UEFI FD (`0x84500000` – `0x84700000`) **collides directly** into Samsung's `tzapp_region` (`0x84300000` – `0x85000000`).
* Writing to `0x84500000` triggers Qualcomm TrustZone / XPU access violation and crashes the hardware.

**Samsung J250F Safe Configuration**:
* Base Address: `0x99000000`
* Tested via `validate_memory_overlap.py`: **0 collisions**.
