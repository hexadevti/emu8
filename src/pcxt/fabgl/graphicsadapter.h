// graphicsadapter.h - minimal CGA adapter STATE for the emu8 PC-XT port.
//
// FabGL's GraphicsAdapter rasterized straight to a VGA scanout buffer. We don't
// have VGA; the LCD render task (pcxtRenderFrame) reads this state and rasterizes
// CGA text / graphics into the panel itself. So this class is just a state holder:
// the current emulation mode, the video buffer pointer (into s_videoMemory), the
// cursor, blink, and the CGA graphics palette selection. No rendering here.
//
// Interface kept compatible with the call sites in machine.cpp (setCGAMode /
// setCGA6845Register) and bios.cpp (videoHandlerEntry).

#pragma once

#include "fabgl.h"

namespace fabgl {

class GraphicsAdapter {

public:

  enum class Emulation {
    None,
    PC_Text_40x25_16Colors,      // CGA text 40x25 (BIOS 00h/01h)
    PC_Text_80x25_16Colors,      // CGA text 80x25 (BIOS 02h/03h)
    PC_Graphics_320x200_4Colors, // CGA 320x200 4-color (BIOS 04h/05h)
    PC_Graphics_640x200_2Colors, // CGA 640x200 mono (BIOS 06h)
    PC_Graphics_HGC_720x348,     // Hercules (unused, kept for enum compat)
  };

  void setEmulation(Emulation e)                  { m_emulation = e; }
  Emulation emulation()                           { return m_emulation; }

  bool enableVideo(bool value)                    { bool p = m_videoEnabled; m_videoEnabled = value; return p; }
  bool videoEnabled()                             { return m_videoEnabled; }

  void setVideoBuffer(void const * b)             { m_videoBuffer = (uint8_t const *)b; }
  uint8_t const * videoBuffer()                   { return m_videoBuffer; }

  void setCursorShape(int start, int end)         { m_cursorStart = start; m_cursorEnd = end; }
  void setCursorPos(int row, int column)          { m_cursorRow = row; m_cursorCol = column; }
  void setCursorVisible(bool value)               { m_cursorVisible = value; }
  int  cursorRow()                                { return m_cursorRow; }
  int  cursorCol()                                { return m_cursorCol; }
  bool cursorVisible()                            { return m_cursorVisible; }

  void setBit7Blink(bool value)                   { m_bit7blink = value; }
  bool bit7blink()                                { return m_bit7blink; }

  int getTextColumns()                            { return m_emulation == Emulation::PC_Text_80x25_16Colors ? 80 : 40; }
  int getTextRows()                               { return 25; }

  // CGA 320x200 graphics palette selection (read by the graphics rasterizer)
  void setPCGraphicsBackgroundColorIndex(int i)   { m_graphBgIndex = i; }
  void setPCGraphicsForegroundColorIndex(int i)   { m_graphFgIndex = i; }
  void setPCGraphicsPaletteInUse(int i)           { m_graphPalette = i; }
  int  graphBackgroundIndex()                     { return m_graphBgIndex; }
  int  graphForegroundIndex()                     { return m_graphFgIndex; }
  int  graphPalette()                             { return m_graphPalette; }

private:

  Emulation        m_emulation     = Emulation::None;
  bool             m_videoEnabled  = false;
  uint8_t const *  m_videoBuffer   = nullptr;
  int              m_cursorStart   = 0;
  int              m_cursorEnd     = 0;
  int              m_cursorRow     = 0;
  int              m_cursorCol     = 0;
  bool             m_cursorVisible = true;
  bool             m_bit7blink     = false;
  int              m_graphBgIndex  = 0;
  int              m_graphFgIndex  = 0;
  int              m_graphPalette  = 0;

};

} // namespace fabgl
