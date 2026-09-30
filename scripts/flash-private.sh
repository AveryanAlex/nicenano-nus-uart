#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pin_file="${1:-}"
mountpoint="${NICENANO_MOUNTPOINT:-/run/media/$USER/NICENANO}"
device_name="${NUS_UART_DEVICE_NAME:-niceUART}"
wait_seconds="${NICENANO_WAIT_SECONDS:-120}"

if [[ -z "$pin_file" || ! -r "$pin_file" ]]; then
	echo "Usage: $0 PIN_FILE" >&2
	exit 2
fi

pin_text="$(tr -d '\r\n' < "$pin_file")"
case "$pin_text" in
	*[!0-9]*|'')
		echo "PIN_FILE must contain exactly six digits." >&2
		exit 2
		;;
esac
if [[ "${#pin_text}" -ne 6 ]]; then
	echo "PIN_FILE must contain exactly six digits." >&2
	exit 2
fi

if [[ -z "$device_name" || "${#device_name}" -gt 26 || ! "$device_name" =~ ^[[:print:]]+$ ]]; then
	echo "NUS_UART_DEVICE_NAME must contain 1-26 printable ASCII characters." >&2
	exit 2
fi
case "$device_name" in
	*['"\']*)
		echo "NUS_UART_DEVICE_NAME cannot contain a quote or backslash." >&2
		exit 2
		;;
esac

# Normalize leading zeroes. BLE displays the numeric passkey zero-padded to six digits.
pin_number=$((10#$pin_text))
temp_root="$(mktemp -d /dev/shm/nicenano-private-build.XXXXXX)"
cleanup() {
	rm -rf -- "$temp_root"
}
trap cleanup EXIT

pin_conf="$temp_root/private-pin.conf"
build_dir="$temp_root/build"
umask 077
printf 'CONFIG_NUS_UART_PASSKEY=%u\nCONFIG_BT_DEVICE_NAME="%s"\n' \
	"$pin_number" "$device_name" > "$pin_conf"

cd "$project_root"
nix develop -c env \
	NUS_UART_EXTRA_CONF="$pin_conf" \
	BUILD_DIR="$build_dir" \
	./scripts/build.sh paired

config_file="$build_dir/nicenano-nus-uart/zephyr/.config"
actual_pin="$(sed -n 's/^CONFIG_NUS_UART_PASSKEY=//p' "$config_file")"
if [[ -z "$actual_pin" || "$actual_pin" -ne "$pin_number" ]]; then
	echo "Private PIN did not reach final Kconfig; refusing to flash." >&2
	exit 3
fi
actual_name="$(sed -n 's/^CONFIG_BT_DEVICE_NAME="\(.*\)"$/\1/p' "$config_file")"
if [[ "$actual_name" != "$device_name" ]]; then
	echo "Device name did not reach final Kconfig; refusing to flash." >&2
	exit 3
fi

uf2="$build_dir/nicenano-nus-uart/zephyr/zephyr.uf2"
if [[ ! -s "$uf2" ]]; then
	echo "UF2 output is missing." >&2
	exit 1
fi

deadline=$((SECONDS + wait_seconds))
while [[ ! -f "$mountpoint/INFO_UF2.TXT" ]]; do
	device="$(lsblk -rno PATH,LABEL | awk '$2 == "NICENANO" { print $1; exit }')"
	if [[ -n "$device" ]]; then
		udisksctl mount -b "$device" >/dev/null 2>&1 || true
	fi
	if [[ -f "$mountpoint/INFO_UF2.TXT" ]]; then
		break
	fi
	if (( SECONDS >= deadline )); then
		echo "nice!nano bootloader did not appear; double-tap RESET and retry." >&2
		exit 1
	fi
	sleep 2
done

cp "$uf2" "$mountpoint/nicenano-nus-uart.uf2"
echo "Private-PIN firmware flashed; RAM build removed."
