// SPDX-License-Identifier: GPL-2.0-only
/* Adapted from Samsung kernel d8e6e0d79bf4e26a11d5f69c843a6ad096b2b181:
 * drivers/soc/qcom/{smem,smd,smd_private,rpm-smd}.c and smem/smd_private.h.
 * Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
 * GPL version 2 only; distributed WITHOUT ANY WARRANTY. See Original/COPYING-GPL2.
 *
 * Existing uncached SMEM entries only. Never allocate or overwrite SMEM tables.
 * Only the APPS/RPM edge is opened; unsupported layouts fail closed.
 * No Linux IRQ/tasklet/allocator dependency; bounded polling replaces waitqueues.
 */
#include "RpmSmd.h"
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>

STATIC UINTN mSmemBase = 0x86300000U;
STATIC UINT32 mSmemSize = 0x100000U;
#define SMEM mSmemBase
#define SMEM_SIZE mSmemSize
#define AUX 0x00060000U
#define AUX_SIZE 0x8000U
#define TOC (SMEM + SMEM_SIZE - 4096U)
#define DOORBELL 0x0b011008U
#define HALF 44U
#define TAIL 36U
#define HEAD 40U
STATIC UINTN mPartition, mPartitionSize, mSend, mRecv, mTx, mRx;
STATIC UINT32 mFifoSize, mId = 2;
STATIC BOOLEAN mPacket, mOpen, mFaulted, mBusy;
STATIC CONST CHAR8 *mStage = "not opened";
CONST CHAR8 *J250fRpmStage (VOID) { return mStage; }
STATIC UINT32 R (UINTN A) { return MmioRead32 (A); }
STATIC VOID W (UINTN A, UINT32 V) { MmioWrite32 (A, V); }
STATIC VOID Notify (VOID) { MemoryFence (); W (DOORBELL, 1); MemoryFence (); }
STATIC BOOLEAN Bounds (UINT32 Offset, UINT32 Size, UINT32 Limit)
{ return Offset <= Limit && Size <= Limit - Offset; }
STATIC BOOLEAN Hosts (UINTN A)
{
  UINT16 A0 = MmioRead16 (A), A1 = MmioRead16 (A + 2);
  return (A0 == 0 && A1 == 6) || (A0 == 6 && A1 == 0);
}
EFI_STATUS J250fRpmConfigureSmem (UINTN Base, UINT32 Size)
{
  if (mOpen) return EFI_ALREADY_STARTED;
  if (Base < 0x80000000U || Base >= 0xe0000000U || Size < 0x4000 ||
      Size > 0x1000000 || (Base & 4095) || (Size & 4095) ||
      Size > 0xe0000000U - Base) return EFI_INVALID_PARAMETER;
  mSmemBase = Base; mSmemSize = Size;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FindPartition (VOID)
{
  UINT32 N, I, Off, Size;
  mPartition = mPartitionSize = 0;
  if (R (TOC) != 0x434f5424U) return EFI_SUCCESS; // legacy global heap
  if (R (TOC + 4) != 1) return EFI_UNSUPPORTED;
  N = R (TOC + 8);
  if (N > (4096U - 32U) / 48U) return EFI_COMPROMISED_DATA;
  for (I = 0; I < N; ++I) {
    UINTN E = TOC + 32 + I * 48;
    if (!Hosts (E + 12)) continue;
    Off = R (E); Size = R (E + 4);
    if (mPartition || Off < 0x2000 || !Bounds (Off, Size, SMEM_SIZE - 4096) || Size < 32)
      return EFI_COMPROMISED_DATA;
    mPartition = SMEM + Off; mPartitionSize = Size;
    if (R (mPartition) != 0x54525024U || !Hosts (mPartition + 4) ||
        R (mPartition + 8) != Size) return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}
STATIC UINTN Item (UINT32 Id, UINT32 *Size)
{
  UINT32 Off, Len, Pad, Free, Base;
  UINTN E;
  *Size = 0;
  if (mPartition) {
    Free = R (mPartition + 12);
    if (Free < 32 || Free > mPartitionSize) return 0;
    for (Off = 32; Off < Free;) {
      if (!Bounds (Off, 16, Free)) return 0;
      E = mPartition + Off;
      if (MmioRead16 (E) != 0xa5a5) return 0;
      Len = R (E + 4); Pad = MmioRead16 (E + 10);
      if (!Bounds (Off + 16, Pad, Free) || !Bounds (Off + 16 + Pad, Len, Free) ||
          MmioRead16 (E + 8) > Len) return 0;
      if (MmioRead16 (E + 2) == Id) {
        *Size = Len - MmioRead16 (E + 8);
        return E + 16 + Pad;
      }
      Off += 16 + Pad + Len;
    }
    return 0;
  }
  if (Id >= 512 || R (SMEM + 0xc0) != 1) return 0;
  E = SMEM + 0xd0 + Id * 16;
  if (R (E) != 1) return 0;
  Off = R (E + 4); Len = R (E + 8); Base = R (E + 12) & 0xfffffffcU;
  if (Base == 0 || Base == SMEM) Base = SMEM;
  if ((Base != SMEM && Base != AUX) ||
      !Bounds (Off, Len, Base == SMEM ? SMEM_SIZE : AUX_SIZE)) return 0;
  // Metadata must be stable: no allocation or remote heap mutation is assumed.
  MemoryFence ();
  if (R (E) != 1 || R (E + 4) != Off || R (E + 8) != Len) return 0;
  *Size = Len;
  return (UINTN)Base + Off;
}
STATIC BOOLEAN NameMatch (UINTN A)
{
  STATIC CONST CHAR8 Name[] = "rpm_requests";
  UINTN I;
  for (I = 0; I < sizeof (Name); ++I)
    if (MmioRead8 (A + I) != (UINT8)Name[I]) return FALSE;
  return TRUE;
}
STATIC EFI_STATUS FindChannel (VOID)
{
  STATIC CONST UINT32 Tables[] = {13, 266};
  STATIC CONST UINT32 Headers[] = {14, 138};
  STATIC CONST UINT32 Fifos[] = {338, 202};
  UINT32 Size, I, T, Cid, Type, HSize, FSize;
  UINTN Table, Entry, H, F;
  for (T = 0; T < 2; ++T) {
    Table = Item (Tables[T], &Size);
    if (!Table) continue;
    if (Size % 32 != 0 || Size > 64 * 32) return EFI_COMPROMISED_DATA;
    for (I = 0; I < Size / 32; ++I) {
      Entry = Table + I * 32;
      if (!NameMatch (Entry) || R (Entry + 28) == 0) continue;
      Cid = R (Entry + 20); Type = R (Entry + 24);
      if ((Type & 0xff) != 15 || Cid >= 64) continue;
      if (((Type >> 8) & 15) != 1 && ((Type >> 8) & 15) != 2) return EFI_UNSUPPORTED;
      H = Item (Headers[T] + Cid, &HSize);
      F = Item (Fifos[T] + Cid, &FSize);
      if (!H || !F) return EFI_NOT_FOUND;
      if (HSize != HALF * 2 || FSize < 256 || FSize > 0x20000 || FSize % 32)
        return EFI_COMPROMISED_DATA;
      mSend = H; mRecv = H + HALF; mTx = F; mRx = F + FSize / 2;
      mFifoSize = FSize / 2; mPacket = ((Type >> 8) & 15) == 2;
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}
STATIC BOOLEAN ChannelState (VOID)
{
  UINT32 State = R (mRecv);
  return R (mSend) == 2 && (State == 2 || State == 3);
}
STATIC EFI_STATUS Indices (UINTN Half, UINT32 *Head, UINT32 *Tail)
{
  *Head = R (Half + HEAD); *Tail = R (Half + TAIL); MemoryFence ();
  return (*Head < mFifoSize && *Tail < mFifoSize && !((*Head | *Tail) & 3))
    ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}
STATIC VOID SetChannelState (UINT32 State)
{
  // Samsung smd.c::ch_set_state: modem-control flags track OPENED.
  UINT32 Up = State == 2 ? 1 : 0;
  W (mSend + 4, Up); W (mSend + 8, Up); W (mSend + 12, Up);
  W (mSend, State); W (mSend + 28, 1); Notify ();
}
EFI_STATUS J250fRpmOpen (VOID)
{
  EFI_STATUS S;
  UINT32 SH, ST, RH, RT, I, Local, Remote;
  mOpen = FALSE; mFaulted = FALSE; mBusy = FALSE;
  mSend = mRecv = mTx = mRx = mFifoSize = 0;
  mStage = "SMEM APPS/RPM partition";
  S = FindPartition (); if (EFI_ERROR (S)) return S;
  mStage = "SMEM rpm_requests channel";
  S = FindChannel (); if (EFI_ERROR (S)) return S;
  mStage = "SMD inherited state validation";
  S = Indices (mSend, &SH, &ST); if (EFI_ERROR (S)) return S;
  S = Indices (mRecv, &RH, &RT); if (EFI_ERROR (S)) return S;
  Local = R (mSend); Remote = R (mRecv);
  // A reset in progress is not permission to overwrite an RPM session.
  for (I = 0; (Local == 5 || Local == 6 || Remote == 5 || Remote == 6) && I < 10000; ++I) {
    MicroSecondDelay (100); Local = R (mSend); Remote = R (mRecv);
  }
  if (Local > 4 || Remote > 4) return EFI_NOT_READY;
  // Firmware is the sole APPS owner after aboot. Reclaim its previous session
  // through SMD close/open, not by assuming both FIFO indices are empty.
  // Never reset counters while the peer still reports OPENED/FLUSHING.
  if (Local != 0 || Remote == 2 || Remote == 3) {
    mStage = "SMD closing bootloader session";
    SetChannelState (0);
    for (I = 0; I < 10000; ++I) {
      Remote = R (mRecv);
      if (Remote == 0 || Remote == 1 || Remote == 4) break;
      MicroSecondDelay (100);
    }
    if (I == 10000) return EFI_TIMEOUT;
  }
  mStage = "SMD opening firmware session";
  // Same local producer/consumer reset as smd_state_change(OPENING).
  // Peer acknowledgment above permits reclaiming stale bootloader entries.
  W (mSend + HEAD, 0); W (mRecv + TAIL, 0);
  W (mSend + 32, 0); // fBLOCKREADINTR
  SetChannelState (1);
  for (I = 0; I < 10000; ++I) {
    if (R (mRecv) == 2) break;
    MicroSecondDelay (100);
  }
  if (I == 10000) return EFI_TIMEOUT;
  SetChannelState (2);
  // The remote owns send-tail and receive-head. Do not zero those fields.
  S = Indices (mSend, &SH, &ST); if (EFI_ERROR (S)) return S;
  S = Indices (mRecv, &RH, &RT); if (EFI_ERROR (S)) return S;
  if (SH != ST || RH != RT) { mStage = "SMD peer did not reset its FIFO"; return EFI_NOT_READY; }
  mOpen = TRUE; mStage = "SMD opened";
  return EFI_SUCCESS;
}
EFI_STATUS J250fRpmSnapshot (UINT32 Values[6])
{
  if (!Values) return EFI_INVALID_PARAMETER;
  if (!mSend || !mRecv) return EFI_NOT_READY;
  Values[0] = R (mSend); Values[1] = R (mRecv);
  Values[2] = R (mSend + HEAD); Values[3] = R (mSend + TAIL);
  Values[4] = R (mRecv + HEAD); Values[5] = R (mRecv + TAIL);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Send (CONST UINT32 *Data, UINT32 Bytes)
{
  UINT32 Head, Tail, Free, I;
  EFI_STATUS S = Indices (mSend, &Head, &Tail);
  if (EFI_ERROR (S)) return S;
  Free = (Tail + mFifoSize - Head - 4) % mFifoSize;
  if (Free < Bytes + (mPacket ? 20U : 0U)) return EFI_NOT_READY;
  if (mPacket) {
    for (I = 0; I < 5; ++I) {
      W (mTx + Head, I == 0 ? Bytes : 0); Head = (Head + 4) % mFifoSize;
    }
  }
  for (I = 0; I < Bytes / 4; ++I) {
    W (mTx + Head, Data[I]); Head = (Head + 4) % mFifoSize;
  }
  MemoryFence (); W (mSend + HEAD, Head); MemoryFence ();
  W (mSend + 20, 1); Notify (); return EFI_SUCCESS;
}
STATIC UINT32 Peek (UINT32 Tail, UINT32 Offset)
{ return R (mRx + ((Tail + Offset) % mFifoSize)); }
STATIC EFI_STATUS ReceiveAck (UINT32 Id)
{
  UINT32 I, Head, Tail, Available, Bytes, Prefix, Next, ReqLen, GotId;
  EFI_STATUS S;
  for (I = 0; I < 10000; ++I) {
    if (!ChannelState ()) return EFI_NOT_READY;
    S = Indices (mRecv, &Head, &Tail); if (EFI_ERROR (S)) return S;
    Available = (Head + mFifoSize - Tail) % mFifoSize;
    Prefix = mPacket ? 20 : 0;
    if (Available >= Prefix + 8) {
      Bytes = mPacket ? Peek (Tail, 0) : Peek (Tail, 4) + 8;
      if (Bytes < 20 || Bytes > 256 || (Bytes & 3) || Bytes + Prefix > mFifoSize - 4)
        return EFI_COMPROMISED_DATA;
      if (Available >= Bytes + Prefix) {
        if (Peek (Tail, Prefix) != 0x00716572U ||
            Peek (Tail, Prefix + 8) != 0x2367736dU ||
            Peek (Tail, Prefix + 12) != 4) return EFI_COMPROMISED_DATA;
        ReqLen = Peek (Tail, Prefix + 4); GotId = Peek (Tail, Prefix + 16);
        if (ReqLen != Bytes - 8 || GotId != Id) return EFI_COMPROMISED_DATA;
        Next = (Tail + Bytes + Prefix) % mFifoSize;
        MemoryFence (); W (mRecv + TAIL, Next); MemoryFence ();
        W (mRecv + 24, 1); W (mRecv + 20, 0); Notify ();
        return ReqLen == 12 ? EFI_SUCCESS : EFI_DEVICE_ERROR;
      }
    }
    MicroSecondDelay (100);
  }
  return EFI_TIMEOUT;
}
EFI_STATUS J250fRpmVote (UINT32 Type, UINT32 Id, CONST UINT32 *Keys,
                         CONST UINT32 *Values, UINTN Count)
{
  UINT32 Msg[7 + 3 * 8], ReqId;
  UINTN I;
  EFI_STATUS S;
  if (!mOpen || mFaulted) { mStage = "RPM session unavailable"; return EFI_NOT_READY; }
  if (!ChannelState ()) { mStage = "RPM peer not open"; return EFI_NOT_READY; }
  if (!Keys || !Values || Count == 0 || Count > 8) return EFI_INVALID_PARAMETER;
  if (mBusy) { mStage = "RPM request already active"; return EFI_NOT_READY; }
  mBusy = TRUE;
  ReqId = mId++;
  if (ReqId < 2) { mFaulted = TRUE; mBusy = FALSE; return EFI_OUT_OF_RESOURCES; }
  Msg[0] = 0x00716572U; Msg[1] = 20 + (UINT32)Count * 12;
  Msg[2] = ReqId; Msg[3] = 0; // ACTIVE_SET
  Msg[4] = Type; Msg[5] = Id; Msg[6] = (UINT32)Count * 12;
  for (I = 0; I < Count; ++I) { Msg[7 + 3*I] = Keys[I]; Msg[8 + 3*I] = 4; Msg[9 + 3*I] = Values[I]; }
  mStage = "RPM request send";
  S = Send (Msg, 28 + (UINT32)Count * 12);
  if (!EFI_ERROR (S)) { mStage = "RPM acknowledgment"; S = ReceiveAck (ReqId); }
  if (EFI_ERROR (S)) mFaulted = TRUE;
  mBusy = FALSE;
  return S;
}
