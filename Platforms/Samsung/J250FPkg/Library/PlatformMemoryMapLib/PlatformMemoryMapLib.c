#include <Library/BaseLib.h>
#include <Library/PlatformMemoryMapLib.h>

static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
    /* Name               Address     Length      HobOption        ResourceAttribute    ArmAttributes
                                                          ResourceType          MemoryType */

    /* DDR Regions */
    /* Low DRAM: Quarantined & Protected Handoff Staging Area (Bootloader BootShim, Staged Payload, ATAGS/DTB at 0x81E00000) */
    {"Bootloader Handoff",0x80000000, 0x02000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
    {"Low DRAM Reserved", 0x82000000, 0x02300000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},

    /* High DRAM: UEFI Execution Regions (Quarantined away from Qualcomm TrustZone 0x84300000-0x86000000) */
    {"UEFI FD",           0x99000000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"CPU Vectors",       0x99300000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"UEFI Stack",        0x99310000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
    {"Log Buffer",        0x99390000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
    {"Info Blk",          0x993a0000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
    {"DXE Heap",          0x993a1000, 0x0ec5f000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

    /* Continuous Splash Hardware Framebuffer (Samsung J250F Rev00-Rev04, 540x960 32bpp) */
    {"Display Reserved",  0xa8000000, 0x00300000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},

    /* Upper DRAM: Conservative 256 MB slice above Framebuffer */
    {"RAM Partition 2",   0xa8300000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

    // Samsung DTS SMEM: reserved shared APPS/RPM memory, uncached device map.
    {"RPM SMEM", 0x86300000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO, NS_DEVICE},

    /* Register regions (Qualcomm Snapdragon 425 MSM8917 MMIO) */
    {"Registers",         0x00000000, 0x80000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},

    /* Terminator for MMU */
    {"Terminator", 0, 0, 0, 0, 0, 0, 0}};

ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap()
{
    return gDeviceMemoryDescriptorEx;
}
