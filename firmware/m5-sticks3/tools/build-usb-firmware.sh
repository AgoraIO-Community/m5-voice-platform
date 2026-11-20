#!/bin/bash
# Build the StickS3 firmware using the project-local ESP-IDF installation.
set -e
umask 077
cd "$(dirname "$0")/.."
export IDF_TOOLS_PATH="$PWD/.cache/espressif"
export IDF_TARGET=esp32s3
export PATH="/opt/homebrew/opt/python@3.12/libexec/bin:$PATH"
. ./esp-idf/export.sh >/dev/null
out="$PWD/build/usb-fallback"
mkdir -p "$out"
python3 "$IDF_PATH/tools/idf.py" -B "$out" -DSDKCONFIG="$out/sdkconfig" build
for setting in CONFIG_TINYUSB_NET_MODE_NCM CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF; do
    grep -qx "$setting=y" "$out/sdkconfig" || { echo "Missing required configuration: $setting" >&2; exit 1; }
done
shasum -a 256 "$out/m5stack_agora.bin"
