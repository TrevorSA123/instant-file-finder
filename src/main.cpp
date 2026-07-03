#include "App.h"

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE /*prevInstance*/, LPWSTR /*cmdLine*/, int showCmd) {
    App app;
    return app.Run(instance, showCmd);
}
