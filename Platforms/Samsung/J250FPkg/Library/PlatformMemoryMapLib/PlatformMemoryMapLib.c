#include <Library/BaseLib.h>
#include <Library/PlatformMemoryMapLib.h>

static ARM_MEMORY_REGION_DESCRIPTOR_EX gDeviceMemoryDescriptorEx[] = {
    /* Name               Address     Length      HobOption        ResourceAttribute    ArmAttributes
                                                          ResourceType          MemoryType */

    /* DDR Regions */
    /* Zone 1: Free Low DRAM (Android kernel entry is 0x80008000, BootShim copies UEFI up to 0x99000000) */
    {"DXE Heap",          0x80000000, 0x04300000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

    /* Zone 4: Free High DRAM - UEFI Firmware Execution Regions (Safe from Samsung TrustZone 0x84300000-0x86000000) */
    {"UEFI FD",           0x99000000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"CPU Vectors",       0x99300000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
    {"UEFI Stack",        0x99310000, 0x00080000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
    {"Log Buffer",        0x99390000, 0x00010000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
    {"Info Blk",          0x993a0000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
    {"RAM Partition 1",   0x993a1000, 0x0ec5f000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

    /* Continuous Splash Hardware Framebuffer (Samsung J250F Rev00-Rev04, 540x960 32bpp) */
    {"Display Reserved",  0xa8000000, 0x00300000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},

    /* Zone 5: Free Upper DRAM (above Framebuffer to 1.5GB RAM ceiling at 0xe0000000) */
    {"RAM Partition 2",   0xa8300000, 0x37d00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

    /* Register regions (Qualcomm Snapdragon 425 MSM8917 MMIO) */
    {"Registers",         0x00000000, 0x80000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},

    /* Terminator for MMU */
    {"Terminator", 0, 0, 0, 0, 0, 0, 0}};

ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap()
{
    return gDeviceMemoryDescriptorEx;
}
