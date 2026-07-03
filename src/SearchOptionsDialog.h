#pragma once
#include "IndexTypes.h"
#include <windows.h>

namespace SearchOptionsDialog {

// Shows the modal Search Options dialog, editing the persistent filter defaults (case
// sensitivity, hidden/system inclusion, size range, modified date range) that get merged into
// every search alongside the query-box text. Returns true if the user clicked OK.
bool Show(HWND owner, HINSTANCE instance, SearchOptions& options);

} // namespace SearchOptionsDialog
