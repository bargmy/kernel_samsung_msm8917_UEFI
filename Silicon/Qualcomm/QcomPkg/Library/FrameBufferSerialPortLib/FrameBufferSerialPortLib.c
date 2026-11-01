#include <PiDxe.h>

#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/SerialPortLib.h>

#include <Resources/FbColor.h>
#include <Resources/font5x12.h>

#include <Library/FrameBufferSerialPortLib.h>

UINTN delay = FixedPcdGet32(PcdMipiFrameBufferDelay);

ARM_MEMORY_REGION_DESCRIPTOR_EX DisplayMemoryRegion;
STATIC UINT8 *mFbBase = NULL;

FBCON_POSITION* p_Position = NULL;
FBCON_POSITION m_MaxPosition;
FBCON_COLOR    m_Color;
BOOLEAN        m_Initialized = FALSE;

UINTN gWidth = FixedPcdGet32(PcdMipiFrameBufferWidth);
// Reserve half screen for output
UINTN gHeight = FixedPcdGet32(PcdMipiFrameBufferHeight);
UINTN gBpp    = FixedPcdGet32(PcdMipiFrameBufferPixelBpp);
UINTN gStride = FixedPcdGet32(PcdMipiFrameBufferPixelsPerScanLine);

// Module-used internal routine
void FbConPutCharWithFactor(char c, int type, unsigned scale_factor);

void FbConDrawglyph(
    char *pixels, unsigned stride, unsigned bpp, unsigned *glyph,
    unsigned scale_factor);

void FbConReset(void);
void FbConScrollUp(void);
void FbConFlush(void);

RETURN_STATUS
EFIAPI
SerialPortInitialize(VOID)
{
  // Prevent dup initialization
  if (m_Initialized)
    return RETURN_SUCCESS;

  if (gStride == 0) {
    gStride = gWidth;
  }

  UINT64 FbAddr = FixedPcdGet64(PcdMipiFrameBufferAddress);
  if (FbAddr != 0) {
    mFbBase = (UINT8 *)(UINTN)FbAddr;
  } else {
    LocateMemoryMapAreaByName("Display Reserved", &DisplayMemoryRegion);
    mFbBase = (UINT8 *)DisplayMemoryRegion.Address;
  }

  UINTN BytesPerPixel = gBpp / 8;
  if (BytesPerPixel == 0) {
    BytesPerPixel = 3;
  }

  p_Position = (FBCON_POSITION*)(((UINTN)mFbBase + (gStride * gHeight * BytesPerPixel) + 7) & ~7);

  // Reset console
  FbConReset();

  // Set flag
  m_Initialized = TRUE;

  return RETURN_SUCCESS;
}

void ResetFb(void)
{
  if (mFbBase == NULL) {
    UINT64 FbAddr = FixedPcdGet64(PcdMipiFrameBufferAddress);
    if (FbAddr != 0) {
      mFbBase = (UINT8 *)(UINTN)FbAddr;
    } else {
      LocateMemoryMapAreaByName("Display Reserved", &DisplayMemoryRegion);
      mFbBase = (UINT8 *)DisplayMemoryRegion.Address;
    }
  }

  if (gStride == 0) {
    gStride = gWidth;
  }

  UINTN BytesPerPixel = gBpp / 8;
  if (BytesPerPixel == 0) {
    BytesPerPixel = 3;
  }

  UINT8 *Line = mFbBase;
  UINTN LinePitch = gStride * BytesPerPixel;
  UINTN RowBytes = gWidth * BytesPerPixel;

  for (UINTN y = 0; y < gHeight; y++) {
    ZeroMem (Line, RowBytes);
    Line += LinePitch;
  }
}

// Start an explicit status page without inheriting another module's cursor.
void FbConStartScreen(void)
{
  SerialPortInitialize();
  ResetFb();
  p_Position->x = 0;
  p_Position->y = 0;
  FbConReset();
  FbConFlush();
}

void FbConReset(void)
{
  // Calc max position.
  m_MaxPosition.x = gWidth / (FONT_WIDTH + 1);
  m_MaxPosition.y = (gHeight - 1) / FONT_HEIGHT;

  // Reset color.
  m_Color.Foreground = 0x00FFFFFF;
  m_Color.Background = 0x00000000;
}

void FbConPutCharWithFactor(char c, int type, unsigned scale_factor)
{
  char *Pixels;

paint:

  if ((unsigned char)c > 127)
    return;

  if ((unsigned char)c < 32) {
    if (c == '\n') {
      goto newline;
    }
    else if (c == '\r') {
      p_Position->x = 0;
      return;
    }
    else {
      return;
    }
  }

  // Save some space
  if (p_Position->x == 0 && (unsigned char)c == ' ' &&
      type != FBCON_SUBTITLE_MSG && type != FBCON_TITLE_MSG)
    return;

  BOOLEAN intstate = ArmGetInterruptState();
  if (intstate)
    ArmDisableInterrupts();

  if (gStride == 0) {
    gStride = gWidth;
  }

  UINTN BytesPerPixel = gBpp / 8;
  if (BytesPerPixel == 0) {
    BytesPerPixel = 3;
  }

  Pixels = (void *)mFbBase;
  Pixels += p_Position->y * (BytesPerPixel * FONT_HEIGHT * gStride);
  Pixels += p_Position->x * scale_factor * (BytesPerPixel * (FONT_WIDTH + 1));

  FbConDrawglyph(
      Pixels, gStride, BytesPerPixel, font5x12 + (c - 32) * 2, scale_factor);

  p_Position->x++;

  if (p_Position->x >= (int)(m_MaxPosition.x / scale_factor))
    goto newline2;

  if (intstate)
    ArmEnableInterrupts();
  return;

newline:
  MicroSecondDelay( delay );

newline2:
  p_Position->y += scale_factor;
  p_Position->x = 0;
  if (p_Position->y >= m_MaxPosition.y - scale_factor) {
    ResetFb();
    FbConFlush();
    p_Position->y = 0;

    if (intstate)
      ArmEnableInterrupts();
    goto paint;
  }
  else {
    FbConFlush();
    if (intstate)
      ArmEnableInterrupts();
  }
}

void FbConDrawglyph(
    char *pixels, unsigned stride, unsigned bpp, unsigned *glyph,
    unsigned scale_factor)
{
  unsigned line_pitch = stride * bpp;
  unsigned char_box_width = (FONT_WIDTH + 1) * scale_factor;
  unsigned total_rows = FONT_HEIGHT * scale_factor;
  unsigned r, font_y, bit_row, data, font_x, j, k, pixel_x;
  unsigned int fg_color = m_Color.Foreground;

  for (r = 0; r < total_rows; ++r) {
    UINT8 *row_ptr = (UINT8 *)pixels + (r * line_pitch);

    // Clear background for this character row to black
    ZeroMem (row_ptr, char_box_width * bpp);

    // Determine font line
    font_y = r / scale_factor;
    if (font_y < 6) {
      bit_row = font_y;
      data = glyph[0] >> (bit_row * 5);
    } else {
      bit_row = font_y - 6;
      data = glyph[1] >> (bit_row * 5);
    }

    // Render foreground pixels
    for (font_x = 0; font_x < FONT_WIDTH; ++font_x) {
      if ((data >> font_x) & 1) {
        for (j = 0; j < scale_factor; ++j) {
          pixel_x = font_x * scale_factor + j;
          UINT8 *p = row_ptr + (pixel_x * bpp);
          if (bpp == 3) {
            p[0] = (UINT8)((fg_color >> 16) & 0xFF); // Red
            p[1] = (UINT8)((fg_color >> 8) & 0xFF);  // Green
            p[2] = (UINT8)(fg_color & 0xFF);         // Blue
          } else {
            unsigned int color = fg_color;
            for (k = 0; k < bpp; ++k) {
              p[k] = (UINT8)(color & 0xFF);
              color >>= 8;
            }
          }
        }
      }
    }
  }
}

void FbConScrollUp(void)
{
  if (mFbBase == NULL) {
    return;
  }

  if (gStride == 0) {
    gStride = gWidth;
  }

  UINTN BytesPerPixel = gBpp / 8;
  if (BytesPerPixel == 0) {
    BytesPerPixel = 3;
  }

  UINTN LinePitch = gStride * BytesPerPixel;
  UINTN RowBytes = gWidth * BytesPerPixel;

  UINT8 *dst = mFbBase;
  UINT8 *src = dst + (LinePitch * FONT_HEIGHT);

  for (UINTN y = 0; y < (gHeight - FONT_HEIGHT); y++) {
    CopyMem (dst, src, RowBytes);
    dst += LinePitch;
    src += LinePitch;
  }

  for (UINTN y = 0; y < FONT_HEIGHT; y++) {
    ZeroMem (dst, RowBytes);
    dst += LinePitch;
  }

  FbConFlush();
}

void FbConFlush(void)
{
  if (mFbBase == NULL) {
    return;
  }

  if (gStride == 0) {
    gStride = gWidth;
  }

  UINTN BytesPerPixel = gBpp / 8;
  if (BytesPerPixel == 0) {
    BytesPerPixel = 3;
  }

  WriteBackInvalidateDataCacheRange(
      (void *)mFbBase,
      (gStride * gHeight * BytesPerPixel));
}

UINTN
EFIAPI
SerialPortWrite(IN UINT8 *Buffer, IN UINTN NumberOfBytes)
{
  UINT8 *CONST Final          = &Buffer[NumberOfBytes];
  UINTN        InterruptState = ArmGetInterruptState();

  if (InterruptState)
    ArmDisableInterrupts();

  while (Buffer < Final) {
    FbConPutCharWithFactor(*Buffer++, FBCON_COMMON_MSG, SCALE_FACTOR);
  }

  if (InterruptState)
    ArmEnableInterrupts();
  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortWriteCritical(IN UINT8 *Buffer, IN UINTN NumberOfBytes)
{
  UINT8 *CONST Final             = &Buffer[NumberOfBytes];
  UINTN        CurrentForeground = m_Color.Foreground;
  UINTN        InterruptState    = ArmGetInterruptState();

  if (InterruptState)
    ArmDisableInterrupts();

  m_Color.Foreground = FB_BGRA8888_YELLOW;

  while (Buffer < Final) {
    FbConPutCharWithFactor(*Buffer++, FBCON_COMMON_MSG, SCALE_FACTOR);
  }

  m_Color.Foreground = CurrentForeground;

  if (InterruptState)
    ArmEnableInterrupts();
  return NumberOfBytes;
}

UINTN
EFIAPI
SerialPortRead(OUT UINT8 *Buffer, IN UINTN NumberOfBytes) { return 0; }

BOOLEAN
EFIAPI
SerialPortPoll(VOID) { return FALSE; }

RETURN_STATUS
EFIAPI
SerialPortSetControl(IN UINT32 Control) { return RETURN_UNSUPPORTED; }

RETURN_STATUS
EFIAPI
SerialPortGetControl(OUT UINT32 *Control) { return RETURN_UNSUPPORTED; }

RETURN_STATUS
EFIAPI
SerialPortSetAttributes(
    IN OUT UINT64 *BaudRate, IN OUT UINT32 *ReceiveFifoDepth,
    IN OUT UINT32 *Timeout, IN OUT EFI_PARITY_TYPE *Parity,
    IN OUT UINT8 *DataBits, IN OUT EFI_STOP_BITS_TYPE *StopBits)
{
  return RETURN_UNSUPPORTED;
}

UINTN SerialPortFlush(VOID) { return 0; }

VOID EnableSynchronousSerialPortIO(VOID)
{
  // Already synchronous
}
