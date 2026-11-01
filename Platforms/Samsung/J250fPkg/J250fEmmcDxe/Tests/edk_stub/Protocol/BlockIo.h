#include <Uefi.h>
typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;
typedef struct {
 UINT32 MediaId;
 BOOLEAN RemovableMedia,MediaPresent,LogicalPartition,ReadOnly,WriteCaching;
 UINT32 BlockSize,IoAlign;
 EFI_LBA LastBlock,LowestAlignedLba;
 UINT32 LogicalBlocksPerPhysicalBlock,OptimalTransferLengthGranularity;
} EFI_BLOCK_IO_MEDIA;
#define EFI_BLOCK_IO_PROTOCOL_REVISION3 0x0002001F
struct _EFI_BLOCK_IO_PROTOCOL {
 UINT64 Revision;
 EFI_BLOCK_IO_MEDIA *Media;
 EFI_STATUS (EFIAPI *Reset)(EFI_BLOCK_IO_PROTOCOL *,BOOLEAN);
 EFI_STATUS (EFIAPI *ReadBlocks)(EFI_BLOCK_IO_PROTOCOL *,UINT32,EFI_LBA,UINTN,VOID *);
 EFI_STATUS (EFIAPI *WriteBlocks)(EFI_BLOCK_IO_PROTOCOL *,UINT32,EFI_LBA,UINTN,VOID *);
 EFI_STATUS (EFIAPI *FlushBlocks)(EFI_BLOCK_IO_PROTOCOL *);
};
extern EFI_GUID gEfiBlockIoProtocolGuid;
