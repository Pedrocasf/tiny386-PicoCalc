#!/bin/sh
# Benchmark tiny386's CPU build options on the Luckfox Lyra (or any ARMv7 musl Linux).
#
# On the build machine, from the top of the tree:
#   scripts/lyra-bench.sh build [make variables...]
#     e.g. scripts/lyra-bench.sh build CROSS_COMPILE=arm-buildroot-linux-musleabihf-
#     builds tiny386_headless once per variant (make PLAT=lyra LYRA_STATIC=y ...) into
#     build/lyra-bench/, together with this script and a bench.ini that needs no BIOS
#
# Copy build/lyra-bench/ to the board, then there:
#   sh lyra-bench.sh run [dir]
#     runs every variant's -bench and prints one table, Mips per row (higher is better)
#
# Only POSIX sh, awk and the binaries are needed on the board (busybox will do).

set -e

# name:make variables, one per line
VARIANTS='base:USE_FAST=n USE_FASTMEM=n
fast:USE_FAST=y USE_FASTMEM=n
fastpd:USE_FAST=pd USE_FASTMEM=n
fastmem:USE_FAST=n USE_FASTMEM=y
fast+fastmem:USE_FAST=y USE_FASTMEM=y'

usage() {
	sed -n '2,15s/^# \{0,1\}//p' "$0" >&2
	exit 1
}

build() {
	out=build/lyra-bench
	mkdir -p "$out"
	[ -e fmopl.inc ] || make PLAT=lyra "$@" prepare
	echo "$VARIANTS" | while IFS=: read -r name vars; do
		echo "=== $name: $vars"
		make -s PLAT=lyra "$@" clean >/dev/null
		# shellcheck disable=SC2086
		make -j"$(nproc 2>/dev/null || echo 4)" PLAT=lyra LYRA_STATIC=y $vars "$@" tiny386_headless
		cp tiny386_headless "$out/tiny386_headless.$name"
	done
	make -s PLAT=lyra "$@" clean >/dev/null
	cp "$0" "$out/lyra-bench.sh"
	cat > "$out/bench.ini" <<'EOF'
; -bench plants its own code at the reset vector, so no BIOS is loaded
[pc]
bios =
vga_bios =
mem_size = 16M
vga_mem_size = 1M
[display]
width = 640
height = 480
[cpu]
gen = 4
fpu = 1
EOF
	echo "done: copy $out/ to the board and run: sh lyra-bench.sh run"
}

run() {
	dir=${1:-$(dirname "$0")}
	tmp=$(mktemp -d)
	trap 'rm -rf "$tmp"' EXIT
	names=
	for bin in "$dir"/tiny386_headless.*; do
		[ -x "$bin" ] || continue
		name=${bin##*/tiny386_headless.}
		names="$names $name"
		echo "running $name..." >&2
		"$bin" -bench "$dir/bench.ini" 2>/dev/null | grep '^bench:' > "$tmp/$name" || true
	done
	[ -n "$names" ] || { echo "no tiny386_headless.* in $dir" >&2; exit 1; }
	uname -a
	grep -m1 -i 'model name\|^Hardware' /proc/cpuinfo 2>/dev/null || true
	# rows: the benchmark, columns: Mips per variant; BAD marks a failed self-check
	for name in $names; do
		awk -v v="$name" '{
			sub(/^bench: +/, "")
			bad = index($0, "BAD") ? "!" : ""
			n = split($0, f, / +/)
			for (i = 1; i <= n; i++) if (f[i] == "Mips") break
			row = f[1]; for (j = 2; j < i - 1; j++) row = row " " f[j]
			print v "\t" row "\t" f[i - 1] bad
		}' "$tmp/$name"
	done | awk -F'\t' -v names="$names" '
		BEGIN { nv = split(names, vs, " ") }
		{ if (!($2 in seen)) { seen[$2] = 1; rows[++nr] = $2 } m[$1 SUBSEP $2] = $3 }
		END {
			printf "%-18s", "Mips"; for (i = 1; i <= nv; i++) printf " %13s", vs[i]; printf "\n"
			for (r = 1; r <= nr; r++) {
				printf "%-18s", rows[r]
				for (i = 1; i <= nv; i++) {
					k = vs[i] SUBSEP rows[r]
					printf " %13s", (k in m) ? m[k] : "-"
				}
				printf "\n"
			}
		}'
}

case "$1" in
build) shift; build "$@" ;;
run) shift; run "$@" ;;
*) usage ;;
esac
