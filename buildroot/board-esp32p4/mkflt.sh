#!/bin/sh
# mkflt -- assemble and link a freestanding rv32 program, on the board.
#
#   mkflt SOURCE.S [-o OUTPUT] [-s STACK]
#
# Two steps, no compiler and no ld:
#
#   as -mno-relax SOURCE.S -o SOURCE.o
#   lua /usr/lib/mkflt.lua SOURCE.o OUTPUT
#
# mkflt.lua lays the sections out, applies the relocations and writes the bFLT
# header, calling objcopy for each section's bytes. Its header comment has the
# full account; the short version is that ld is 5.3 MB, binfmt_flat needs that
# in one contiguous block, and this guest never has one:
#
#   nommu: Allocation of length 5283840 from process 36 (ld) failed
#   Normal: ... 3*1024kB 2*2048kB 3*4096kB 0*8192kB = 25400kB
#
# -mno-relax is not optional. With relaxation on, `as` leaves R_RISCV_ALIGN and
# R_RISCV_RELAX markers that exist so a LINKER can delete instructions and
# re-lay-out the file. mkflt.lua cannot do that -- deleting an instruction
# moves every symbol after it -- so it refuses those objects by name rather
# than producing something subtly wrong.
set -e

usage() {
    echo "usage: mkflt SOURCE.S [-o OUTPUT] [-s STACK]" >&2
    exit 2
}

src=""
out=""
stack=""
# `while :` with the test inside, rather than `while [ $# -gt 0 ]`.
#
# Under `set -e` this busybox ash ends the SCRIPT when the while's controlling
# command finally returns false -- verified with sh -x on the board, where the
# trace stops dead at `+ '[' 0 -gt 0 ']'` and nothing after the loop runs. The
# test in a `||` list is exempt from errexit, so this form survives.
while :; do
    [ $# -gt 0 ] || break
    case "$1" in
        -o) shift; [ $# -gt 0 ] || usage; out="$1" ;;
        -s) shift; [ $# -gt 0 ] || usage; stack="$1" ;;
        -*) usage ;;
        *)  if [ -n "$src" ]; then usage; fi; src="$1" ;;
    esac
    shift
done

[ -n "$src" ] || usage
[ -r "$src" ] || { echo "mkflt: cannot read $src" >&2; exit 1; }

base=${src%.S}
base=${base%.s}
[ -n "$out" ] || out=$base

for tool in as objcopy lua; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "mkflt: $tool is not in PATH" >&2
        exit 1
    fi
done

WRAPPER=/usr/lib/mkflt.lua
[ -r "$WRAPPER" ] || { echo "mkflt: missing $WRAPPER" >&2; exit 1; }

as -mno-relax "$src" -o "$base.o"

if [ -n "$stack" ]; then
    lua "$WRAPPER" "$base.o" "$out" -s "$stack"
else
    lua "$WRAPPER" "$base.o" "$out"
fi

chmod +x "$out"
