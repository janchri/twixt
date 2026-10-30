#!/usr/bin/env bash
# Copyright (c) 2026 Christoph Jans <jans.christoph@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Builds the MCUs of a twixt sample for native_sim (west build -DMCU=<mcu>),
# runs each as its own process on the sample's virtual I2C bus, and prints their
# logs as they come, every line after the name of its MCU. Ctrl-C stops them.
#
#   scripts/twixt_run.sh <sample> <mcu>... [-- <native_sim options>]
#
#   scripts/twixt_run.sh samples/ping a b
#   scripts/twixt_run.sh samples/ping a b -- -stop_at=3
#
#   BUILD_DIR=dir       where the builds, logs and flash files go
#                       (default: ./build-twixt-<sample>)
#   SKIP_BUILD=1        run what was built before
#   CMAKE_ARGS="..."    more arguments for CMake, as -DCONFIG_TWIXT_LOG_LEVEL_DBG=y
#                       (another BUILD_DIR for another variant: CMake keeps
#                       its cache)
#   GDB="<mcu>..."      build these MCUs for debugging (<BUILD_DIR>/<mcu>-gdb)
#                       and run them under gdbserver :2345, :2346 ..., each
#                       linked as /tmp/twixt-gdb-<port>.exe
#
# Every MCU has its own flash file, <BUILD_DIR>/<mcu>.flash.bin, if it has
# flash: settings and files are still there on the next run.
#
# Needs `west` and an initialised Zephyr environment (ZEPHYR_BASE).

set -u

[[ $# -ge 2 ]] || { sed -n 's/^#   //p' "$0" | head -3; exit 2; }

sample=$(realpath "$1")
shift
mcus=()
while [[ $# -gt 0 && $1 != -- ]]; do
	mcus+=("$1")
	shift
done
[[ ${1:-} == -- ]] && shift
out=$(realpath -m "${BUILD_DIR:-build-twixt-$(basename "$sample")}")
mkdir -p "$out"

gdb_port() {
	local port=2345 g
	for g in ${GDB:-}; do
		[[ $g == "$1" ]] && { echo "$port"; return; }
		port=$((port + 1))
	done
	return 1
}
dir() { gdb_port "$1" > /dev/null && echo "$out/$1-gdb" || echo "$out/$1"; }

# The images are independent: build them at the same time
if [[ -z ${SKIP_BUILD:-} ]]; then
	echo "building ${mcus[*]} ..."
	builds=()
	for m in "${mcus[@]}"; do
		opt=()
		gdb_port "$m" > /dev/null && opt=(-DCONFIG_DEBUG_OPTIMIZATIONS=y)
		# shellcheck disable=SC2086 # CMAKE_ARGS: one argument per word
		west build -b native_sim -d "$(dir "$m")" "$sample" -- -DMCU="$m" "${opt[@]}" \
			${CMAKE_ARGS:-} \
			> "$out/$m.build.log" 2>&1 &
		builds+=("$!")
	done
	for i in "${!mcus[@]}"; do
		wait "${builds[i]}" || {
			echo "build of ${mcus[i]} failed, see $out/${mcus[i]}.build.log"
			exit 1
		}
	done
fi

pids=()
cleanup() {
	kill "${pids[@]}" 2>/dev/null
	for g in ${GDB:-}; do
		pkill -f "^$(dir "$g")/zephyr/zephyr.exe"
	done
	wait 2>/dev/null
}
trap cleanup EXIT
trap exit INT TERM

for m in "${mcus[@]}"; do
	exe=$(dir "$m")/zephyr/zephyr.exe
	flash=()
	"$exe" --help 2>/dev/null | grep -q -- '-flash=' && flash=(--flash="$out/$m.flash.bin")
	gdb=()
	if port=$(gdb_port "$m"); then
		gdb=(gdbserver ":$port")
		ln -sf "$exe" "/tmp/twixt-gdb-$port.exe"
	fi
	"${gdb[@]}" "$exe" "${flash[@]}" "$@" > >(sed -u "s/^/[$m] /") 2>&1 &
	pids+=("$!")
done
wait
