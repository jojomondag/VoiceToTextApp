#include "app.h"
#include <windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // Enable high-DPI awareness
    SetProcessDPIAware();

    vtt::App app;
    if (!app.initialize(hInstance)) {
        MessageBoxW(nullptr,
                    L"Failed to initialize VoiceToText.\n\n"
                    L"Please ensure:\n"
                    L"  - A Whisper GGML model is in the 'models' folder\n"
                    L"  - Your GPU drivers are up to date\n"
                    L"  - A microphone is connected",
                    L"VoiceToText - Error",
                    MB_ICONERROR | MB_OK);
        return 1;
    }

    int result = app.run();
    app.shutdown();
    return result;
}
