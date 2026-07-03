#pragma once
#include <windows.h>

class App {
public:
    int Run(HINSTANCE instance, int showCmd);

private:
    void EnableDpiAwareness();
    void InitCommonControls();

    // Returns true if this process just launched an elevated relaunch of itself and should
    // exit immediately without creating the main window.
    bool HandleStartupElevation(HINSTANCE instance);
};
