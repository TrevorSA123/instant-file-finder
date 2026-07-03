#include "Clipboard.h"

namespace Clipboard {

bool CopyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;

    bool ok = false;
    if (EmptyClipboard()) {
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem != nullptr) {
            void* dest = GlobalLock(mem);
            if (dest != nullptr) {
                memcpy(dest, text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) {
                GlobalFree(mem); // only free on failure; a successful SetClipboardData takes ownership
            }
        }
    }

    CloseClipboard();
    return ok;
}

} // namespace Clipboard
