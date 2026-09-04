#!/usr/bin/env bash
# Builds the poppler leg of the PDF backend differential against the host's
# libpoppler-cpp. The binary lands next to this script as poppler_floor.
set -euo pipefail
cd "$(dirname "$0")"
g++ -O2 -std=c++20 -Wall -Wextra poppler_floor.cpp \
  $(pkg-config --cflags --libs poppler-cpp) \
  -o poppler_floor
echo "built poppler_floor against poppler-cpp $(pkg-config --modversion poppler-cpp)"
