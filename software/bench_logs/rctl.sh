#!/bin/bash
exec /root/sync/Victor9000-Development-Private/victor9k_remote_control/host/.venv/bin/python /root/sync/Victor9000-Development-Private/victor9k_remote_control/host/remotectl.py --dev /dev/serial/by-id/usb-IGM_Victor_9000_Remote_Control_2600A48D424FB2F8-if00 "$@"
