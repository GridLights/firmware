#!/bin/bash

# Simple script to monitor ESP32 serial output
# Usage: ./monitor.sh

cd "$(dirname "$0")"

echo "Starting serial monitor for ESP32..."
echo "Press Ctrl+C to exit"
echo ""

pio device monitor -b 115200
