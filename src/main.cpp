#include "app.h"
#include <windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // Single-instance guard: a named mutex scoped to the current logon session
    // (Local\) blocks a second VoiceToText from running simultaneously. If the
    // mutex already exists, another process owns it — terminate silently as
    // requested. The handle is intentionally leaked: the OS releases the mutex
    // when this process exits, so we never need to CloseHandle.
    HANDLE single_instance_mutex = CreateMutexW(
        nullptr, TRUE, L"Local\\VoiceToText.SingleInstance.Mutex");
    if (!single_instance_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (single_instance_mutex) CloseHandle(single_instance_mutex);
        return 0;
    }

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
