# VoiceToText project instructions

## Application build destination

Whenever building the application, including when the user asks to "build" or
"bygga", build the `VoiceToText` target in `Release` using
`artifacts/pianissimo-tooltip-build` as the CMake build directory.

The required executable on Josef's Windows machine is:

```text
C:\Users\Josef\Documents\Github\VoiceToTextApp\artifacts\pianissimo-tooltip-build\bin\Release\VoiceToText.exe
```

The user's Windows taskbar shortcut points to this exact file. Keep this path
stable and update this executable for every application build, unless the user
explicitly requests a different destination. A build in another directory does
not satisfy a request to build the app.

Run from the repository root:

```powershell
cmake --build artifacts/pianissimo-tooltip-build --config Release --target VoiceToText
```

If `cmake` is not on PATH, use the installed Visual Studio CMake executable:

```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build artifacts/pianissimo-tooltip-build --config Release --target VoiceToText
```

Verify that the build succeeds and the executable exists at the required path
before reporting completion. If the executable is locked or the build fails,
report the actual problem; do not silently switch output directories.
