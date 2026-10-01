#!/bin/bash
# Matches a freshly-flashed shed node to the gateway's mesh: channel PSK,
# region, and modem preset. Also disables WiFi/MQTT/stock Detection Sensor,
# since only the gateway needs those, and a fresh flash could have any of
# them in an unknown state.
#
# Owner name and remote_hardware.available_pins (this shed's specific GPIOs)
# are NOT set here - set those via the Meshtastic phone app over Bluetooth
# once the node is physically installed and you can see the actual wiring.
#
# Usage:
#   ./configure_shed_node.sh <port>
#
# Example:
#   ./configure_shed_node.sh /dev/ttyUSB0

set -e

PORT="$1"

if [ -z "$PORT" ]; then
    echo "Usage: $0 <port>"
    echo "Example: $0 /dev/ttyUSB0"
    exit 1
fi

# ---- Fixed to match your gateway - update here if the gateway's channel
# PSK, region, or modem preset ever change, and every future shed picks it up.
# The "base64:" prefix is required by the CLI's psk parser (meshtastic.util.fromStr) -
# without it, a raw base64 string is treated as plain text rather than decoded to
# bytes, which crashes with "expected bytes, str found" when writing it to the device.
PRIMARY_PSK="base64:dlthum8CeC0sQPeHSEJe+cACwT/AAf0kNcBkB4N+ZmA=" #PSK for the channel 0
PUBLIC_KEY="base64:PwbVChNynIihxYn8u3fnPc2oX/wc2OoSoiYDORbNklA="  #Public key for the gateway node
REGION="ANZ"
MODEM_PRESET="LONG_FAST"
TIME_ZONE="NZST-12NZDT,M9.5.0,M4.1.0/3"

echo "Configuring $PORT to match the gateway's mesh..."

# Channel edits and --set config flags don't reliably commit together in one
# invocation on this library version (the channel PSK can silently fail to
# apply while config fields succeed) - so the channel PSK gets its own
# isolated command, verified before anything else happens.
echo "Setting channel PSK..."
meshtastic --port "$PORT" --ch-set psk "$PRIMARY_PSK" --ch-index 0
meshtastic --port "$PORT" --reboot
sleep 5

echo "Setting Admin Public Key..."
meshtastic --port "$PORT" --set security.admin_key "$PUBLIC_KEY"
meshtastic --port "$PORT" --reboot
sleep 5


ACTUAL_PSK=$(meshtastic --port "$PORT" --info 2>/dev/null | grep -o '"psk": "[^"]*"' | head -n 1)
PRIMARY_PSK_RAW="${PRIMARY_PSK#base64:}"  # --info reports the raw value, without the CLI-only "base64:" prefix
if [[ "$ACTUAL_PSK" != *"$PRIMARY_PSK_RAW"* ]]; then
    echo "WARNING: channel PSK does not appear to have applied correctly."
    echo "Expected to contain: $PRIMARY_PSK_RAW"
    echo "Got: $ACTUAL_PSK"
    echo "Stopping here rather than continuing with a mismatched channel - check manually."
    exit 1
fi
echo "Channel PSK confirmed applied."

# Known issue: meshtastic-python crashes with "expected bytes, str found" when
# committing a settings transaction on any device with PKI security keys set
# (which is every current-firmware device) - see meshtastic/python#678. The
# individual field writes below still succeed before that crash happens; only
# the final auto-reboot step fails. So: don't let set -e kill the script here,
# and always force a reboot afterward regardless of whether this step "failed".
set +e
meshtastic --port "$PORT" \
    --set lora.region "$REGION" \
    --set lora.modem_preset "$MODEM_PRESET" \
    --set network.wifi_enabled false \
    --set mqtt.enabled false \
    --set detection_sensor.enabled false \
    --set device.tzdef "$TIME_ZONE" \
    --set bluetooth.enabled true 
set -e

echo "Rebooting $PORT to apply (forced separately, in case the transaction commit above hit the known PKI-key bug)..."
meshtastic --port "$PORT" --reboot

echo "Verifying applied settings..."
meshtastic --port "$PORT" --info

echo "Done. Set the owner name for this"
echo "shed via the Meshtastic phone app over Bluetooth once it's installed."
