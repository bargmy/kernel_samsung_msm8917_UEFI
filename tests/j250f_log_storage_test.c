/* Real ext4 parser + real bounded log mapper against a fake disk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#undef NULL
#include "../Platforms/Samsung/J250fPkg/J250fEmmcDxe/J250fWpPrivate.c"
#undef ASSERT
#define CHECK(X) do { if (!(X)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#X); exit(1); } } while (0)
STATIC UINT8 Disk[4096 * 1024];
STATIC UINT8 Scratch[4096];
STATIC EFI_BLOCK_IO_PROTOCOL Io;
STATIC EFI_BLOCK_IO_MEDIA Media;
STATIC EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
EFI_GUID gEfiBlockIoProtocolGuid, gEfiDevicePathProtocolGuid,
         gEfiDiskIoProtocolGuid, gEfiPartitionInfoProtocolGuid;
STATIC UINTN Writes;
STATIC EFI_LBA LastLba;
STATIC EXT4_NODE Folder;

VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D,0,N); }
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) { return memset(D,V,N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A,B,N); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
BOOLEAN EFIAPI CompareGuid(CONST GUID *A, CONST GUID *B) { return memcmp(A,B,sizeof(*A)) == 0; }
EFI_STATUS EFIAPI J250fLogBackendWrite(EFI_LBA Lba, CONST VOID *Data) {
  CHECK(Data != NULL); ++Writes; LastLba = Lba; return EFI_SUCCESS;
}
EFI_STATUS EFIAPI J250fLogInstall(UINT32 First) { (VOID)First; return EFI_SUCCESS; }

STATIC VOID Put16(UINT8 *P, UINT16 V) { P[0]=(UINT8)V; P[1]=(UINT8)(V>>8); }
STATIC VOID Put32(UINT8 *P, UINT32 V) { Put16(P,(UINT16)V); Put16(P+2,(UINT16)(V>>16)); }
STATIC EFI_STATUS EFIAPI Read(EFI_BLOCK_IO_PROTOCOL *This, UINT32 Id, EFI_LBA Lba, UINTN N, VOID *Out) {
  (VOID)This; (VOID)Id;
  if (Lba >= sizeof(Disk)/512 || N > sizeof(Disk)-Lba*512) return EFI_DEVICE_ERROR;
  memcpy(Out,Disk+(UINTN)Lba*512,N); return EFI_SUCCESS;
}
STATIC VOID DescriptorChecksum(VOID) {
  UINT8 Group[4]={0};
  UINT8 *Desc=Disk+4096;
  UINT16 C=LogCrc16(0xffff,mBacking.Ext4.Super+104,16);
  C=LogCrc16(C,Group,4); C=LogCrc16(C,Desc,30); Put16(Desc+30,C);
}
STATIC VOID Fixture(VOID) {
  EXT4_VOLUME *V=&mBacking.Ext4;
  UINT8 *Raw, *Dir, *Desc;
  UINT32 I;
  memset(Disk,0,sizeof(Disk)); memset(&mBacking,0,sizeof(mBacking));
  memset(&Folder,0,sizeof(Folder)); memset(&Media,0,sizeof(Media));
  Io.Media=&Media; Io.ReadBlocks=Read;
  Media.LastBlock=sizeof(Disk)/512-1; Media.BlockSize=512;
  V->Disk=&Io; V->SectorSize=512; V->Scratch=Scratch; V->BlockSize=4096;
  V->Signature=EXT4_SIGNATURE; V->Incompat=0x42;
  V->InodeSize=256; V->InodesPerGroup=128; V->BlocksPerGroup=1024;
  V->DescriptorSize=32; V->Groups=1; V->BlockCount=1024; V->InodeCount=128; V->TableOffset=4096;
  Put16(V->Super+58,1); Put32(V->Super+96,0x42); Put32(V->Super+100,0x13);
  Put32(V->Super+92,0x1c);
  mBacking.PhysicalStart=4096; mBacking.Media.LastBlock=Media.LastBlock;
  Desc=Disk+4096; Put32(Desc,2); Put32(Desc+4,3); Put32(Desc+8,4);
  memset(Disk+2*4096,0xff,4096); memset(Disk+3*4096,0xff,4096);
  DescriptorChecksum();
  /* inode 12 is a fully allocated 1MiB file at blocks 100..355. */
  Raw=Disk+4*4096+11*256;
  Put16(Raw,0x81a4); Put32(Raw+4,LOG_BYTES); Put16(Raw+26,1);
  Put32(Raw+28,LOG_BYTES/512); Put32(Raw+32,0x80000);
  Put16(Raw+40,0xf30a); Put16(Raw+42,1); Put16(Raw+44,4);
  Put16(Raw+40+12+4,256); Put32(Raw+40+12+8,100);
  Folder.Mode=0x4000; Folder.Size=4096; Put32(Folder.BlockMap,20);
  Dir=Disk+20*4096; Put32(Dir,12); Put16(Dir+4,4096); Dir[6]=8; Dir[7]=1;
  memcpy(Dir+8,"uefi.log",8);
  for(I=0;I<LOG_SECTORS;++I) {
    UINT8 *Sector=Disk+100*4096+I*512;
    memset(Sector,' ',512); LogHeader(Sector,I,FALSE);
  }
  mLogMapped=FALSE; Writes=0;
}
STATIC EFI_STATUS Stage(VOID) { return LogStage(&mBacking.Ext4,&Folder); }
int main(void) {
  UINT8 Record[512];
  Fixture();
  { EFI_STATUS S=Stage(); if(EFI_ERROR(S)) fprintf(stderr,"initial stage status: 0x%llx\n",(unsigned long long)S); CHECK(S==EFI_SUCCESS); }
  CHECK(mLogNext==0); CHECK(Writes==0);
  memset(Record,' ',512); LogHeader(Record,0,TRUE); memcpy(Record+25,"000003",6); memcpy(Record+32,"hi\n",3);
  j250f_log_seal(Record,0,3);
  CHECK(J250fLogWriteRecord(0,Record)==EFI_ACCESS_DENIED);
  mBacking.Concealed=TRUE;
  CHECK(J250fLogWriteRecord(0,Record)==EFI_SUCCESS);
  CHECK(Writes==1 && LastLba==4096+100*8);
  CHECK(mLogNext==1);
  CHECK(J250fLogWriteRecord(LOG_SECTORS,Record)==EFI_ACCESS_DENIED);
  CHECK(J250fLogWriteRecord(1,Record)==EFI_INVALID_PARAMETER);
  LogHeader(Record,2,TRUE); memcpy(Record+25,"000003",6);
  j250f_log_seal(Record,2,3);
  CHECK(J250fLogWriteRecord(2,Record)==EFI_ACCESS_DENIED);
  LogHeader(Record,0,TRUE); memcpy(Record+25,"000003",6);
  j250f_log_seal(Record,0,3);
  Fixture(); Put32(mBacking.Ext4.Super+96,0x46); CHECK(EFI_ERROR(Stage())); CHECK(!mLogMapped);
  Fixture(); Put16(mBacking.Ext4.Super+58,2); CHECK(EFI_ERROR(Stage()));
  Fixture(); Put32(mBacking.Ext4.Super+100,0x400); CHECK(EFI_ERROR(Stage()));
  Fixture(); Disk[4096+30]^=1; CHECK(Stage()==EFI_CRC_ERROR);
  Fixture(); Disk[2*4096+100/8]&=(UINT8)~(1u<<(100%8)); CHECK(EFI_ERROR(Stage()));
  Fixture(); Put16(Disk+4*4096+11*256+26,2); CHECK(EFI_ERROR(Stage()));
  Fixture(); Put16(Disk+4*4096+11*256+40+12+4,0x8100); CHECK(EFI_ERROR(Stage()));
  Fixture(); Put32(Disk+4*4096+11*256+40+12+8,4); CHECK(EFI_ERROR(Stage()));
  Fixture(); Disk[100*4096+32]='X'; CHECK(EFI_ERROR(Stage()));
  Fixture(); memcpy(Disk+100*4096,Record,512); CHECK(Stage()==EFI_SUCCESS); CHECK(mLogNext==1);
  CHECK(J250fLogWriteRecord(0,Record)==EFI_ACCESS_DENIED);
  Fixture(); memcpy(Disk+100*4096+512,Record,512); CHECK(EFI_ERROR(Stage()));
  Fixture(); memcpy(Disk+100*4096,Record,512); Disk[100*4096+32]^=1; CHECK(EFI_ERROR(Stage()));
  puts("log storage guards: PASS"); return 0;
}
