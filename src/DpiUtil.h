#pragma once
// Small helpers for manual per-monitor DPI scaling. The app declares Per-Monitor-V2 DPI
// awareness (see App.cpp / resources/app.manifest), which means Windows never stretches our
// window content for us - every hand-built dialog is responsible for scaling its own control
// positions/sizes and creating its own DPI-appropriate font. Without this, layouts that look
// right at 100% scaling get visibly cramped (oversized text overflowing fixed-size boxes) on
// any higher-DPI display, which is exactly the class of bug this file exists to prevent.

#include "Handle.h"

#include <windows.h>

namespace DpiUtil {

constexpr int kDefaultDpi = 96;

// DPI of the monitor a window is currently on. Falls back to 96 if the window handle is null or
// the API is unavailable for any reason - never fails outright.
int GetDpiForWindowSafe(HWND hwnd);

// Scales a design-time pixel value (authored at 96 DPI) to the given DPI.
inline int Scale(int value, int dpi) { return MulDiv(value, dpi, kDefaultDpi); }

// Builds the system's current dialog/message font (what GetStockObject(DEFAULT_GUI_FONT) is
// meant to approximate) properly scaled to 'dpi', via SystemParametersInfoForDpi where
// available. Unlike the stock font, this one must be deleted by the caller (UniqueGdiObject<HFONT>
// handles that); it is not a shared system object.
UniqueGdiObject<HFONT> CreateMessageFont(int dpi);

} // namespace DpiUtil
