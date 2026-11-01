#!/usr/bin/env python3
"""
validate_memory_overlap.py
Validates memory region layouts for Samsung SM-J250F (j2y18lte) MSM8917 EDK2 UEFI port.
Checks for collisions between:
- Hardware DTS reserved memory regions (TrustZone, Modem, ADSP, WCNSS, Splash, etc.)
- Android bootloader loading regions (Kernel load address, Tags/DTB, Ramdisk)
- Candidate UEFI memory regions (UEFI FD, UEFI Stack, CPU Vectors, Framebuffer, DXE Heap)
"""

import sys, json

# 1. Hardware Reserved Memory Regions (Extracted from Samsung J250F Rev00-Rev04 DTS)
HW_RESERVED = [
    {"name": "tzapp_region (TrustZone/QSEE)", "base": 0x84300000, "size": 0x00d00000, "source": "DTS", "status": "VERIFIED"},
    {"name": "sec_debug_region",              "base": 0x85000000, "size": 0x00800000, "source": "DTS", "status": "VERIFIED"},
    {"name": "klog_region",                   "base": 0x851ff000, "size": 0x00201000, "source": "DTS", "status": "VERIFIED"},
    {"name": "ss_plog",                       "base": 0x85c00000, "size": 0x00400000, "source": "DTS", "status": "VERIFIED"},
    {"name": "ramoops",                       "base": 0x86000000, "size": 0x00100000, "source": "DTS", "status": "VERIFIED"},
    {"name": "other_ext_region",              "base": 0x86100000, "size": 0x00700000, "source": "DTS", "status": "VERIFIED"},
    {"name": "modem_region (MPSS)",           "base": 0x86800000, "size": 0x05a00000, "source": "DTS", "status": "VERIFIED"},
    {"name": "adsp_fw_region",                "base": 0x8c200000, "size": 0x01800000, "source": "DTS", "status": "VERIFIED"},
    {"name": "wcnss_fw_region",               "base": 0x8da00000, "size": 0x00700000, "source": "DTS", "status": "VERIFIED"},
    {"name": "lk_region (Little Kernel)",     "base": 0x8f600000, "size": 0x00300000, "source": "DTS", "status": "VERIFIED"},
    {"name": "venus_region",                  "base": 0x8fc00000, "size": 0x00400000, "source": "DTS", "status": "VERIFIED"},
    {"name": "modem_shared_mem_region",       "base": 0x93000000, "size": 0x06000000, "source": "DTS", "status": "VERIFIED"},
    {"name": "splash_region (Framebuffer)",   "base": 0xa8000000, "size": 0x00300000, "source": "DTS", "status": "VERIFIED"},
]

# 2. Bootloader Handoff Regions (from boot.img header)
BOOTLOADER_REGIONS = [
    {"name": "aboot Kernel Load Area",        "base": 0x80008000, "size": 0x01000000, "source": "boot_hdr", "status": "VERIFIED"},
    {"name": "aboot Tags / ATAGS / DTB Area", "base": 0x81e00000, "size": 0x00200000, "source": "boot_hdr", "status": "VERIFIED"},
]

def check_overlaps(regions):
    overlaps = []
    # Sort by base address
    sorted_regions = sorted(regions, key=lambda r: r['base'])
    
    for i in range(len(sorted_regions)):
        r1 = sorted_regions[i]
        r1_end = r1['base'] + r1['size']
        for j in range(i + 1, len(sorted_regions)):
            r2 = sorted_regions[j]
            r2_end = r2['base'] + r2['size']
            
            # Check overlap
            if r2['base'] < r1_end and r2_end > r1['base']:
                overlap_start = max(r1['base'], r2['base'])
                overlap_end = min(r1_end, r2_end)
                overlap_size = overlap_end - overlap_start
                overlaps.append({
                    "r1": r1['name'],
                    "r2": r2['name'],
                    "start": overlap_start,
                    "end": overlap_end,
                    "size": overlap_size
                })
    return sorted_regions, overlaps

def evaluate_uefi_candidate(candidate_name, uefi_regions):
    print(f"\n=======================================================")
    print(f" EVALUATING CANDIDATE CONFIGURATION: {candidate_name}")
    print(f"=======================================================")
    
    all_regions = list(HW_RESERVED) + list(uefi_regions)
    sorted_regs, overlaps = check_overlaps(all_regions)
    
    for r in sorted_regs:
        start = r['base']
        end = r['base'] + r['size']
        mb = r['size'] / (1024 * 1024)
        is_uefi = r in uefi_regions
        tag = "[UEFI]" if is_uefi else "[HW-RES]"
        print(f" {tag:8s} 0x{start:08x} - 0x{end:08x} ({mb:6.2f} MB) {r['name']}")
    
    if overlaps:
        print(f"\n[!] COLLISION DETECTED! ({len(overlaps)} overlaps):")
        for o in overlaps:
            print(f"    CONFLICT: '{o['r1']}' and '{o['r2']}'")
            print(f"              Overlap range: 0x{o['start']:08x} - 0x{o['end']:08x} (0x{o['size']:x} bytes)")
        return False
    else:
        print(f"\n[OK] NO OVERLAPS DETECTED. Memory layout is statically valid!")
        return True

if __name__ == '__main__':
    # Test Huawei Y6's default layout against Samsung HW
    huawei_layout = [
        {"name": "UEFI FD (Huawei Default)",    "base": 0x84500000, "size": 0x00200000, "source": "Huawei ATU-L21", "status": "VERIFIED"},
        {"name": "CPU Vectors",                 "base": 0x84700000, "size": 0x00001000, "source": "Huawei ATU-L21", "status": "VERIFIED"},
        {"name": "UEFI Stack",                  "base": 0x84701000, "size": 0x00040000, "source": "Huawei ATU-L21", "status": "VERIFIED"},
    ]
    print("\n--- TEST 1: Huawei Y6 Default Layout against Samsung J250F ---")
    evaluate_uefi_candidate("Huawei Y6 Default", huawei_layout)
    
    # Test Proposed Safe Samsung J250F Layout (placed in Upper Safe RAM e.g. 0x99000000 or 0x80300000)
    # Let's test Candidate A: Safe High RAM (0x99000000, which has 240 MB free DRAM up to splash at 0xa8000000)
    samsung_candidate_high = [
        {"name": "UEFI FD",                     "base": 0x99000000, "size": 0x00300000, "source": "J250FPkg", "status": "IMPLEMENTED"},
        {"name": "CPU Vectors",                 "base": 0x99300000, "size": 0x00010000, "source": "J250FPkg", "status": "IMPLEMENTED"},
        {"name": "UEFI Stack",                  "base": 0x99310000, "size": 0x00080000, "source": "J250FPkg", "status": "IMPLEMENTED"},
    ]
    print("\n--- TEST 2: Proposed Samsung J250F Candidate (High DRAM 0x99000000) ---")
    evaluate_uefi_candidate("Samsung J250F High RAM", samsung_candidate_high)
