#!/bin/bash
# Launch hologram, wait for render, screenshot the window, kill it.
# Usage: ./scripts/screenshot.sh [output_path]
# Default output: /tmp/hologram_screenshot.png
set -e

OUTPUT="${1:-/tmp/hologram_screenshot.png}"
APP="./build/hologram"

# Kill any existing instance
pkill -f "build/hologram" 2>/dev/null || true
sleep 0.5

# Launch in background
"$APP" 2>/dev/null &
APP_PID=$!

# Wait for window to appear and render a few frames
sleep 3

# Get window ID via swift (CoreGraphics)
cat > /tmp/get_window_id.swift << 'SWIFT'
import CoreGraphics
import Foundation
let windowList = CGWindowListCopyWindowInfo(.optionOnScreenOnly, kCGNullWindowID) as? [[String: Any]] ?? []
for window in windowList {
    if let ownerName = window[kCGWindowOwnerName as String] as? String,
       ownerName == "hologram",
       let windowNumber = window[kCGWindowNumber as String] as? Int {
        print(windowNumber)
        exit(0)
    }
}
exit(1)
SWIFT

WINDOW_ID=$(swift /tmp/get_window_id.swift 2>/dev/null)

if [ -n "$WINDOW_ID" ]; then
    screencapture -l "$WINDOW_ID" -x "$OUTPUT"
    echo "$OUTPUT"
else
    echo "ERROR: Could not find hologram window" >&2
    kill $APP_PID 2>/dev/null || true
    exit 1
fi

# Kill the app
kill $APP_PID 2>/dev/null || true
wait $APP_PID 2>/dev/null || true
