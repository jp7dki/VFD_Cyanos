#!/usr/bin/env bash
set -euo pipefail

# This script stages the refactor files and creates a single commit.
# Run from the project root: ./apply_and_commit.sh

FILES=(
  src/display.h
  src/display.cpp
  src/wifi_server.h
  src/wifi_server.cpp
  src/main.cpp
)

echo "Staging files..."
git add "${FILES[@]}"

echo "Creating commit..."
git commit -m "refactor: split display and wifi server into separate modules; clean includes and formatting"

echo "Done. To push: git push"
