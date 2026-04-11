#!/bin/bash
# SolSpektrum ESP32 Serial Monitor
# Attaches to serial monitor without building or flashing.
# Usage: ./monitor.sh

pio device monitor -e esp32dev
