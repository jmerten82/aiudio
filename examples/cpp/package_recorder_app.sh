#!/usr/bin/env bash
# Package the mic+tap recorder binary into a signed macOS .app bundle.
#
#   package_recorder_app.sh <binary> <Info.plist> <output_dir>
#
# A .app with a stable CFBundleIdentifier gives TCC a stable identity, so the
# microphone + audio-recording grants survive rebuilds (unlike a bare ad-hoc CLI,
# whose signature — and grant — resets each build). Ad-hoc signs by default; set
# AIUDIO_CODESIGN_ID to a Developer ID / Apple Development identity to real-sign.
#
# Run the tool from the bundle (args + stdout work):
#   ./aiudio-recorder.app/Contents/MacOS/aiudio-recorder --seconds 10 out.wav
set -euo pipefail

BIN="${1:?binary path required}"
PLIST="${2:?Info.plist path required}"
OUTDIR="${3:?output dir required}"
IDENTITY="${AIUDIO_CODESIGN_ID:--}"   # "-" = ad-hoc

APP="$OUTDIR/aiudio-recorder.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN" "$APP/Contents/MacOS/aiudio-recorder"
cp "$PLIST" "$APP/Contents/Info.plist"

codesign --force --sign "$IDENTITY" --identifier com.aiudio.recorder \
         --timestamp=none "$APP"

echo "packaged + signed: $APP"
echo "  run: $APP/Contents/MacOS/aiudio-recorder --seconds 10 out.wav"
codesign -dvv "$APP" 2>&1 | sed 's/^/  /' || true
