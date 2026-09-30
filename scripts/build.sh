#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ncs_workspace="${NCS_WORKSPACE:-${XDG_DATA_HOME:-$HOME/.local/share}/ncs/3.4.1}"
zephyr_sdk="${ZEPHYR_SDK_INSTALL_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/zephyr-sdk-1.0.1}"
profile="${1:-paired}"
build_dir="${BUILD_DIR:-$project_root/build/$profile}"

case "$profile" in
	paired)
		extra_conf="$project_root/paired.conf"
		if [[ -z "${NUS_UART_EXTRA_CONF:-}" ]]; then
			echo "Paired builds require NUS_UART_EXTRA_CONF with CONFIG_NUS_UART_PASSKEY." >&2
			exit 2
		fi
		;;
	open)
		extra_conf=""
		;;
	*)
		echo "Usage: $0 [paired|open]" >&2
		exit 2
		;;
esac

if [[ -n "${NUS_UART_EXTRA_CONF:-}" ]]; then
	extra_conf="$extra_conf;$NUS_UART_EXTRA_CONF"
fi

if [[ ! -x "$ncs_workspace/.venv/bin/west" ]]; then
	echo "Missing NCS workspace. Run scripts/bootstrap.sh first." >&2
	exit 1
fi

export ZEPHYR_SDK_INSTALL_DIR="$zephyr_sdk"
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

cd "$ncs_workspace"
build_args=(
	--pristine=always
	--board nice_nano@2.0.0
	--source-dir "$project_root"
	--build-dir "$build_dir"
	--
	-DBOARD_ROOT="$project_root"
)

if [[ -n "$extra_conf" ]]; then
	build_args+=("-DEXTRA_CONF_FILE=$extra_conf")
fi

"$ncs_workspace/.venv/bin/west" build "${build_args[@]}"

echo "UF2: $build_dir/nicenano-nus-uart/zephyr/zephyr.uf2"
