#!/bin/sh
# Fail the build if any bFLT binary is too large for the guest to exec.
#
# binfmt_flat copies text+data+bss+stack into ONE contiguous power-of-two block
# per exec (mm/nommu.c do_mmap_private -> alloc_pages), with no demand paging.
# So the largest runnable program is bounded by the page allocator's
# MAX_PAGE_ORDER, and a binary over it fails at exec:
#
#   nommu: Allocation of length 7344128 from process 63 (file) failed
#   binfmt_flat: Unable to allocate RAM for process text/data, errno -12
#
# Without this check the ceiling is invisible at build time and shows up on the
# board as execs failing, which is a much worse place to find it.
#
# ---------------------------------------------------------------------------
# The limit used to be 524288 here, on the theory that the buddy free lists
# never held anything bigger. That was WRONG, and the measurement that settled
# it is worth keeping. /proc/buddyinfo on the board:
#
#   order    0    1    2    3    4    5    6    7    8    9   10
#   boot     1    2    8    8    7    7    7    2    1    2    4
#   after    0    2    0    4    4    7    9    0    2    1    4
#
# ("after" = ls -R /, dmesg, grep, tar, a 50k-element Lua table, nano.)
#
# Four free order-10 blocks at boot and still four after real work. Ordinary
# use churns the LOW orders and leaves the top of the allocator alone. So the
# ceiling was never fragmentation -- it was MAX_PAGE_ORDER itself, and the old
# 512 kB figure needlessly excluded GNU gzip, bzip2recover and others.
#
# LIMIT must therefore track CONFIG_ARCH_FORCE_MAX_ORDER in linux.fragment,
# which this board raises to 11 (8 MB) via patches/linux/0002-*. If you change
# one, change the other; they are two halves of the same decision.
# ---------------------------------------------------------------------------
set -e
TARGET_DIR="$1"
FLTHDR="$2"
PAGE=4096

# 4096 << 11 -- order 11, matching CONFIG_ARCH_FORCE_MAX_ORDER=11.
LIMIT=8388608

# Not a limit, just a note: anything over this is worth knowing about, because
# it is resident in full for the life of the process on a 29 MB machine.
NOTE=1048576

test -n "$TARGET_DIR"
test -x "$FLTHDR"

fail=0
checked=0
big=0

for f in "$TARGET_DIR"/bin/* "$TARGET_DIR"/sbin/* \
         "$TARGET_DIR"/usr/bin/* "$TARGET_DIR"/usr/sbin/*; do
    [ -f "$f" ] || continue
    if [ -L "$f" ]; then continue; fi
    head -c 4 "$f" 2>/dev/null | grep -q bFLT || continue

    out=$("$FLTHDR" "$f")
    ds=$(echo "$out" | sed -n 's/.*Data Start:  *0x\([0-9a-f]*\).*/\1/p')
    de=$(echo "$out" | sed -n 's/.*Data End:  *0x\([0-9a-f]*\).*/\1/p')
    be=$(echo "$out" | sed -n 's/.*BSS End:  *0x\([0-9a-f]*\).*/\1/p')
    ss=$(echo "$out" | sed -n 's/.*Stack Size:  *0x\([0-9a-f]*\).*/\1/p')
    rc=$(echo "$out" | sed -n 's/.*Reloc Count:  *0x\([0-9a-f]*\).*/\1/p')
    [ -n "$ds" ] && [ -n "$be" ] || continue

    text=$((0x$ds))
    data=$((0x$de - 0x$ds))
    bss=$((0x$be - 0x$de))
    stack=$((0x$ss))
    relocs=$((0x$rc * 4))

    tail=$((bss + stack))
    if [ "$relocs" -gt "$tail" ]; then tail=$relocs; fi
    need=$((text + data + tail))
    need=$(( (need + PAGE - 1) / PAGE * PAGE ))

    checked=$((checked + 1))
    if [ "$need" -gt "$LIMIT" ]; then
        echo "checkflat: FAIL ${f#$TARGET_DIR} needs $need bytes (> $LIMIT)" >&2
        fail=$((fail + 1))
    elif [ "$need" -gt "$NOTE" ]; then
        printf 'checkflat: BIG  %-28s %8d bytes -- resident in full while it runs\n' \
               "${f#$TARGET_DIR}" "$need"
        big=$((big + 1))
    fi
done

echo "checkflat: $checked bFLT binaries checked, $big over ${NOTE}, $fail over the ${LIMIT} ceiling"
if [ "$fail" -gt 0 ]; then
    echo "checkflat: shrink them, or raise CONFIG_ARCH_FORCE_MAX_ORDER and this" >&2
    echo "checkflat: LIMIT together -- see buildroot/README.md" >&2
    exit 1
fi
