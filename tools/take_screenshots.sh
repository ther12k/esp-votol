#!/bin/bash
set -e

REPO_DIR="/home/ther12k/Workspace/Learning/esp-votol"
SHOT_DIR="$REPO_DIR/output/screenshots"
V3_DIR="$SHOT_DIR/app4"
mkdir -p "$SHOT_DIR" "$V3_DIR"

BASE_URL="http://127.0.0.1:8096"
CHROME="google-chrome"
FLAGS="--headless --disable-gpu --hide-scrollbars --virtual-time-budget=2500"

echo "Capturing v4.0 cyber-industrial screenshots — both themes (412x915)..."

# Control tab: 3 states x 2 themes
for theme in dark light; do
  for state in armed disarmed offline; do
    $CHROME $FLAGS --window-size=412,915 \
      --screenshot="$V3_DIR/${theme}_${state}.png" \
      "$BASE_URL/index.html?frame=raw&tab=control&state=${state}&theme=${theme}"
  done

  # Config + Pairing tabs (theme shows in all chrome + appearance card)
  $CHROME $FLAGS --window-size=412,915 \
    --screenshot="$V3_DIR/${theme}_config.png" \
    "$BASE_URL/index.html?frame=raw&tab=config&state=armed&theme=${theme}"

  $CHROME $FLAGS --window-size=412,915 \
    --screenshot="$V3_DIR/${theme}_pairing.png" \
    "$BASE_URL/index.html?frame=raw&tab=pairing&state=armed&theme=${theme}"
done

# ARM confirmation modal (dark)
$CHROME $FLAGS --window-size=412,915 \
  --screenshot="$V3_DIR/dark_arm_modal.png" \
  "$BASE_URL/index.html?frame=raw&tab=control&state=disarmed&modal=arm&theme=dark"

# Framed presentation mockups (dark + light)
$CHROME $FLAGS --window-size=500,980 \
  --screenshot="$V3_DIR/mockup_dark_disarmed.png" \
  "$BASE_URL/index.html?hideToolbar=1&tab=control&state=disarmed&theme=dark"

$CHROME $FLAGS --window-size=500,980 \
  --screenshot="$V3_DIR/mockup_light_disarmed.png" \
  "$BASE_URL/index.html?hideToolbar=1&tab=control&state=disarmed&theme=light"

# Full interactive inspector overview
$CHROME $FLAGS --window-size=880,1150 \
  --screenshot="$V3_DIR/inspector_overview.png" \
  "$BASE_URL/index.html?tab=control&state=armed&theme=dark"

echo "Screenshots successfully captured into $V3_DIR:"
ls -lh "$V3_DIR"
