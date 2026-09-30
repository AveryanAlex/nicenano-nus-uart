#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
profile="${1:-paired}"
uf2="$project_root/build/$profile/nicenano-nus-uart/zephyr/zephyr.uf2"
mountpoint="${NICENANO_MOUNTPOINT:-/run/media/$USER/NICENANO}"

if [[ ! -f "$uf2" ]]; then
	echo "Missing $uf2. Run scripts/build.sh $profile first." >&2
	exit 1
fi

if [[ ! -f "$mountpoint/INFO_UF2.TXT" ]]; then
	echo "nice!nano bootloader is not mounted at $mountpoint." >&2
	echo "Double-tap RESET and try again." >&2
	exit 1
fi

cp "$uf2" "$mountpoint/nicenano-nus-uart.uf2"
echo "Flashed $uf2"
