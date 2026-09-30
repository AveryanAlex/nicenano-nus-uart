#!/usr/bin/env bash
set -euo pipefail

ncs_version="v3.4.1"
sdk_version="1.0.1"
ncs_workspace="${NCS_WORKSPACE:-${XDG_DATA_HOME:-$HOME/.local/share}/ncs/${ncs_version#v}}"
sdk_parent="${XDG_DATA_HOME:-$HOME/.local/share}"
sdk_dir="${ZEPHYR_SDK_INSTALL_DIR:-$sdk_parent/zephyr-sdk-$sdk_version}"
download_dir="${XDG_CACHE_HOME:-$HOME/.cache}/zephyr-sdk-downloads"

minimal_archive="$download_dir/zephyr-sdk-${sdk_version}_linux-x86_64_minimal.tar.xz"
toolchain_archive="$download_dir/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz"
minimal_url="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${sdk_version}/$(basename "$minimal_archive")"
toolchain_url="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${sdk_version}/$(basename "$toolchain_archive")"

mkdir -p "$download_dir" "$sdk_parent" "$(dirname "$ncs_workspace")"

if [[ ! -f "$minimal_archive" ]]; then
	curl -fL "$minimal_url" -o "$minimal_archive"
fi
if [[ ! -f "$toolchain_archive" ]]; then
	curl -fL "$toolchain_url" -o "$toolchain_archive"
fi

echo 'ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb  '"$minimal_archive" | sha256sum -c -
echo '21b85981cb5a1818d9bc53d82af80f208946ec038b982ff1907287572ed3a634  '"$toolchain_archive" | sha256sum -c -

if [[ ! -f "$sdk_dir/sdk_version" ]]; then
	tar -xf "$minimal_archive" -C "$sdk_parent"
fi
if [[ ! -x "$sdk_dir/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]]; then
	mkdir -p "$sdk_dir/gnu"
	tar -xf "$toolchain_archive" -C "$sdk_dir/gnu"
fi

if [[ ! -x "$ncs_workspace/.venv/bin/python" ]]; then
	mkdir -p "$ncs_workspace"
	uv venv --python 3.12 "$ncs_workspace/.venv"
fi

uv pip install --python "$ncs_workspace/.venv/bin/python" 'west==1.5.0'

if [[ ! -f "$ncs_workspace/.west/config" ]]; then
	"$ncs_workspace/.venv/bin/west" init \
		--manifest-url https://github.com/nrfconnect/sdk-nrf \
		--manifest-rev "$ncs_version" \
		"$ncs_workspace"
fi

cd "$ncs_workspace"
"$ncs_workspace/.venv/bin/west" update --narrow
uv pip install --python "$ncs_workspace/.venv/bin/python" \
	-r "$ncs_workspace/zephyr/scripts/requirements.txt" \
	-r "$ncs_workspace/nrf/scripts/requirements.txt"

echo "NCS workspace: $ncs_workspace"
echo "Zephyr SDK: $sdk_dir"

