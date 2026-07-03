#include "DpiUtil.h"

namespace DpiUtil {

int GetDpiForWindowSafe(HWND hwnd) {
    if (hwnd != nullptr) {
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi != 0) return static_cast<int>(dpi);
    }

    // Fallback for a null/invalid handle: the desktop DC's DPI is a reasonable default.
    HDC screenDc = GetDC(nullptr);
    int dpi = kDefaultDpi;
    if (screenDc != nullptr) {
        int queried = GetDeviceCaps(screenDc, LOGPIXELSX);
        if (queried > 0) dpi = queried;
        ReleaseDC(nullptr, screenDc);
    }
    return dpi;
}

UniqueGdiObject<HFONT> CreateMessageFont(int dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);

    // SystemParametersInfoForDpi (Windows 10 1607+) returns metrics pre-scaled for 'dpi'; if
    // it's unavailable for some reason, fall back to the unscaled metrics and scale by hand.
    bool haveMetrics = SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                                   static_cast<UINT>(dpi)) != FALSE;
    if (!haveMetrics && SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) != FALSE) {
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, dpi, kDefaultDpi);
        haveMetrics = true;
    }

    LOGFONTW lf{};
    if (haveMetrics) {
        lf = ncm.lfMessageFont;
    } else {
        // Extremely unlikely fallback (both SPI calls failing): hand-build a reasonable dialog
        // font rather than give up.
        lf.lfHeight = -MulDiv(9, dpi, 72);
        lf.lfWeight = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = DEFAULT_QUALITY;
        lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
    }

    return UniqueGdiObject<HFONT>(CreateFontIndirectW(&lf));
}

} // namespace DpiUtil
