#!/bin/bash
# SolSpektrum ESP32 Build and Flash Script
# Usage: ./build_flash_monitor.sh [--heap-monitor]

set -e  # Exit on error

# Parse arguments
BUILD_FLAGS=""
if [[ "$*" != *"--heap-monitor"* ]]; then
    echo "ℹ️  Heap monitor disabled (use --heap-monitor to enable)"
else
    BUILD_FLAGS="-D USERMOD_GL_HEAP_MONITOR"
    echo "ℹ️  Heap monitor enabled"
fi

echo "→ Building, flashing, and monitoring..."
if [ -n "$BUILD_FLAGS" ]; then
    PLATFORMIO_BUILD_FLAGS="$BUILD_FLAGS" pio run -e esp32dev -t upload -t monitor
else
    pio run -e esp32dev -t upload -t monitor
fi
