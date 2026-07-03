#pragma once
#include "SettingsService.h"
#include <windows.h>

namespace PreferencesDialog {

// Shows the modal Preferences dialog. Returns true if the user clicked OK, in which case
// 'settings' has been updated in place with the values chosen in the dialog.
bool Show(HWND owner, HINSTANCE instance, AppSettings& settings);

} // namespace PreferencesDialog
