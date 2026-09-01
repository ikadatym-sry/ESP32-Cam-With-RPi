#!/bin/bash
# ==========================================================
# ESP32-CAM Hub & Cloudflare R2 - RPi 4 Safe Setup Script
# ==========================================================
# This script sets up an isolated Python virtual environment
# to ensure NO global packages or system services are touched.

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
cd "$DIR"

echo "=== 1. Checking Python 3 & venv ==="
if ! command -v python3 &> /dev/null; then
    echo "Python 3 is required. Please install python3."
    exit 1
fi

echo "=== 2. Creating isolated virtual environment in ./venv ==="
if [ ! -d "venv" ]; then
    python3 -m venv venv
fi

echo "=== 3. Installing dependencies into venv ==="
./venv/bin/pip install --upgrade pip
./venv/bin/pip install -r requirements.txt

echo "=== 4. Ensuring RAM buffer directory (/dev/shm) exists ==="
mkdir -p /dev/shm/esp32cam_clips
chmod 777 /dev/shm/esp32cam_clips

echo ""
echo "=========================================================="
echo " Setup Complete! "
echo " To run manually:"
echo "   cd $DIR && ./venv/bin/python app.py"
echo ""
echo " To run as background systemd service (auto-start on boot):"
echo "   sudo bash $DIR/install_systemd.sh"
echo "=========================================================="

