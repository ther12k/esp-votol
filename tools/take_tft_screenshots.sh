#!/bin/bash
set -e

REPO_DIR="/home/ther12k/Workspace/Learning/esp-votol"
SHOT_DIR="$REPO_DIR/output/screenshots/tft"
mkdir -p "$SHOT_DIR"

BASE_URL="http://127.0.0.1:8096"
CHROME="google-chrome"
FLAGS="--headless --disable-gpu --hide-scrollbars --virtual-time-budget=2500"

echo "Capturing TFT pod screens (240x320 @3x -> 720x960)..."

for s in ride elec motor nodata lock lockarmed nofob stat cfg set standby pin armask animarm animdisarm bootdark wifi; do
  $CHROME $FLAGS --window-size=720,960 \
    --screenshot="$SHOT_DIR/tft_$s.png" \
    "$BASE_URL/tft.html?frame=raw&screen=$s&zoom=3" > /dev/null 2>&1
  echo "  tft_$s.png"
done

# theme variations (orange value / pink accent, L text, BT off icon)
$CHROME $FLAGS --window-size=720,960 \
  --screenshot="$SHOT_DIR/tft_ride_orange.png" \
  "$BASE_URL/tft.html?frame=raw&screen=ride&zoom=3&val=3&acc=5&bt=0" > /dev/null 2>&1
echo "  tft_ride_orange.png (alt theme + BT-off badge)"

echo "Done -> $SHOT_DIR"
