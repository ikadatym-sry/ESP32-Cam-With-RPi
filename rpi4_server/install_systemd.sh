#!/bin/bash
# Install as a lightweight systemd service

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root (sudo bash install_systemd.sh)"
  exit 1
fi

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
USER_NAME="aerkd"

SERVICE_FILE="/etc/systemd/system/esp32cam-hub.service"

cat <<EOF > $SERVICE_FILE
[Unit]
Description=ESP32-CAM Hub & Cloudflare R2 Relay Service
After=network.target

[Service]
Type=simple
User=$USER_NAME
WorkingDirectory=$DIR
ExecStart=$DIR/venv/bin/python $DIR/app.py
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable esp32cam-hub.service
systemctl restart esp32cam-hub.service

echo "=========================================================="
echo " Service installed and started!"
echo " Status: sudo systemctl status esp32cam-hub.service"
echo " Logs:   journalctl -u esp32cam-hub.service -f"
echo "=========================================================="

