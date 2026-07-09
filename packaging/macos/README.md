# macOS packaging

`build-dmg.sh` creates a user-friendly DMG from a built `VoiceToText.app`:

```bash
./packaging/macos/build-dmg.sh build/VoiceToText.app dist/VoiceToText-macOS.dmg
```

The current application uses Win32 APIs, Windows global hotkeys, Windows tray
integration, and Windows-specific linking. It therefore cannot produce a
working macOS `.app` yet. This packaging script is intentionally strict and
will stop instead of shipping a misleading macOS installer. A macOS port must
first replace those platform integrations with macOS equivalents.
