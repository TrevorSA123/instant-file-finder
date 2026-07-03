#pragma once
#include "IndexManager.h"
#include <windows.h>

namespace IndexDrivesDialog {

// Shows the modal Index Drives dialog, letting the user check/uncheck drives and cycle each
// drive's scan method. On OK, the choices are written back into 'indexManager' and the
// function returns true (the caller is then expected to call IndexManager::StartIndexing for
// the drives the user selected). Returns false if the user cancelled.
bool Show(HWND owner, HINSTANCE instance, IndexManager& indexManager);

} // namespace IndexDrivesDialog
