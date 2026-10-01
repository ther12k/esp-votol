#!/bin/bash
set -e

REPO_DIR="/home/ther12k/Workspace/Learning/esp-votol"
SHOT_DIR="$REPO_DIR/output/screenshots"
mkdir -p "$SHOT_DIR"

BASE_URL="http://127.0.0.1:8096"
CHROME="google-chrome"
FLAGS="--headless --disable-gpu --hide-scrollbars --virtual-time-budget=2500"

echo "Capturing raw mobile viewport screenshots (412x915)..."

# 1. Control tab - ARMED state
$CHROME $FLAGS --window-size=412,915 \
  --screenshot="$SHOT_DIR/01_control_armed_mobile.png" \
  "$BASE_URL/?frame=raw&tab=control&state=armed"

# 2. Control tab - DISARMED state
$CHROME $FLAGS --window-size=412,915 \
  --screenshot="$SHOT_DIR/02_control_disarmed_mobile.png" \
  "$BASE_URL/?frame=raw&tab=control&state=disarmed"

# 3. Control tab - OFFLINE state
$CHROME $FLAGS --window-size=412,915 \
  --screenshot="$SHOT_DIR/03_control_offline_mobile.png" \
  "$BASE_URL/?frame=raw&tab=control&state=offline"

# 4. ARM Confirmation Modal Dialog
$CHROME $FLAGS --window-size=412,915 \
  --screenshot="$SHOT_DIR/04_arm_confirm_modal_mobile.png" \
  "$BASE_URL/?frame=raw&tab=control&state=disarmed&modal=arm"

# 5. Config tab
$CHROME $FLAGS --window-size=412,915 \
  --screenshot="$SHOT_DIR/05_config_tab_mobile.png" \
  "$BASE_URL/?frame=raw&tab=config&state=armed"

# 6. Pairing tab
$CHROME $FLAGS --window-size=412,915 \
  --screenshot="$SHOT_DIR/06_pairing_tab_mobile.png" \
  "$BASE_URL/?frame=raw&tab=pairing&state=armed"

echo "Capturing presentation phone mockup screenshots (500x980)..."

# Framed mockup - Control ARMED
$CHROME $FLAGS --window-size=500,980 \
  --screenshot="$SHOT_DIR/07_mockup_armed.png" \
  "$BASE_URL/?hideToolbar=1&tab=control&state=armed"

# Framed mockup - Control DISARMED
$CHROME $FLAGS --window-size=500,980 \
  --screenshot="$SHOT_DIR/08_mockup_disarmed.png" \
  "$BASE_URL/?hideToolbar=1&tab=control&state=disarmed"

# Framed mockup - Full interactive inspector overview
$CHROME $FLAGS --window-size=840,1100 \
  --screenshot="$SHOT_DIR/09_interactive_inspector_overview.png" \
  "$BASE_URL/?tab=control&state=armed"

echo "Screenshots successfully captured into $SHOT_DIR:"
ls -lh "$SHOT_DIR"
