#!/usr/bin/env bash
set -euo pipefail

APP_PATH="${1:-build/VoiceToText.app}"
OUTPUT_PATH="${2:-dist/VoiceToText-macOS.dmg}"

if [[ ! -d "$APP_PATH" ]]; then
  echo "VoiceToText.app was not found at: $APP_PATH" >&2
  echo "The current application source is Windows-only; a macOS port must provide the .app first." >&2
  exit 2
fi

mkdir -p "$(dirname "$OUTPUT_PATH")"
rm -f "$OUTPUT_PATH"
hdiutil create -volname "VoiceToText" -srcfolder "$APP_PATH" -ov -format UDZO "$OUTPUT_PATH"
echo "Created $OUTPUT_PATH"
