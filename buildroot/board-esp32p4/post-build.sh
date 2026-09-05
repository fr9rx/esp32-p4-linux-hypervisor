#!/bin/sh
# Post-build fixups for the ESP32-P4 hypervisor guest.
set -e
TARGET_DIR="$1"
test -n "$TARGET_DIR"

# 1. Drop init scripts this board does not want.
#
# Under binfmt_flat every process gets a private copy of the whole binary, so
# syslogd + klogd + crond cost ~1.6 MB of a 29 MB machine and fragment memory.
# S02sysctl goes too: /etc/sysctl.conf is empty, so all it did was fork an echo
# that was the last boot-time exec failure.
rm -f "$TARGET_DIR/etc/init.d/S01syslogd"
rm -f "$TARGET_DIR/etc/init.d/S02klogd"
rm -f "$TARGET_DIR/etc/init.d/S02sysctl"
rm -f "$TARGET_DIR/etc/init.d/S50crond"

# 2. Prune applet symlinks busybox no longer provides.
#
# busybox is installed with "make install-noclobber", which never removes an
# existing file, so every applet disabled in board/esp32p4/busybox.fixups leaves
# its symlink behind. The result looks worse than a missing command: /bin/vi
# exists, and running it says "vi: applet not found".
#
# busybox.links is the list of what this busybox actually installs -- 104
# entries here, including /bin/sh, /bin/hush, /sbin/init, /sbin/getty and
# /bin/login -- so anything pointing at busybox and absent from it is dead.
LINKS=""
for candidate in "$BUILD_DIR"/busybox-*/busybox.links; do
    if [ -f "$candidate" ]; then
        LINKS="$candidate"
        break
    fi
done

if [ -n "$LINKS" ]; then
    pruned=0
    for dir in bin sbin usr/bin usr/sbin; do
        [ -d "$TARGET_DIR/$dir" ] || continue
        for link in "$TARGET_DIR/$dir"/*; do
            [ -L "$link" ] || continue
            case "$(readlink "$link")" in
                *busybox) ;;
                *) continue ;;
            esac
            relative="${link#$TARGET_DIR}"
            # -F matters: two applets are named "[" and "[[", which as
            # regexes are "Unmatched [" errors, so grep failed, the match
            # looked negative and both symlinks got deleted every build.
            if ! grep -qxF "$relative" "$LINKS"; then
                rm -f "$link"
                pruned=$((pruned + 1))
            fi
        done
    done
    echo "post-build: pruned $pruned stale busybox applet symlinks"
else
    echo "post-build: no busybox.links found, leaving applet symlinks alone" >&2
fi

# Note on XIP, which is what you would reach for to avoid the per-exec copy
# entirely. It very nearly works and then corrupts the process:
#
#   fs/binfmt_flat.c:526 takes the XIP path when FLAT_FLAG_RAM is clear (clear
#   it with flthdr -R), mmaps text straight out of the file and allocates only
#   data+bss+stack. On romfs over mtd-rom that mmap really does resolve into the
#   PSRAM window (romfs_get_unmapped_area -> mtd_get_unmapped_area ->
#   maprom_point): init was observed running with epc=0x49d23df8, inside the
#   romfs, text never copied.
#
#   But text and data then live at unrelated addresses and nothing tells the
#   program where its data went. riscv has no arch/riscv/include/asm/flat.h, so
#   there is no FLAT_PLAT_INIT to pass a data base (only m68k and sh have one),
#   and uClibc crt1.S:73 computes gp with `lla gp, __global_pointer$` -- PC
#   relative, so under XIP gp lands in the read-only ROM copy of the GOT. Every
#   global then reads an unrelocated address: init died on a store to 0x00111a4c
#   with gp = start_data + 0x920, pointing into the romfs.
#
#   Doing it properly needs every data reference routed through a data-base
#   register, which is what -msep-data does on m68k/bfin. RISC-V gcc has no
#   equivalent, so this is not reachable from here. Shrinking the binary under
#   the next power of two was the productive path instead.

# A writable /home.
#
# / is romfs and mounted ro, which is what got the rootfs out of RAM, so the
# mount point has to exist in the image -- you cannot mkdir it at runtime --
# and fstab needs somewhere writable to put over it. tmpfs is RAM backed, so
# /home does not survive a reboot; persistence would need the monitor to write
# back to the flash partition, which it currently only ever reads.
mkdir -p "$TARGET_DIR/home"
if ! grep -q "[[:space:]]/home[[:space:]]" "$TARGET_DIR/etc/fstab"; then
    printf "tmpfs		/home		tmpfs	mode=0755	0	0\n" >> "$TARGET_DIR/etc/fstab"
fi

# Three long-standing mount complaints on this board, all inherited from the
# buildroot skeleton rather than caused by anything here:
#
#   romfs: Unknown parameter 'relatime'
#   mount: mounting mtd0 on / failed: Invalid argument
#   devpts: called with bogus options
#   sysfs: Unknown parameter 'defaults'        <- and so no /sys at all
#
# The common cause is that THIS busybox mount does not translate the option
# string; it hands the whole thing to the kernel, so the field may contain
# real filesystem options and nothing else. Measured on the board:
#
#   mount -t sysfs sysfs /sys                       -> ok
#   mount -t sysfs -o rw sysfs /sys                 -> ok
#   mount -t sysfs -o nosuid,nodev,noexec sysfs /sys-> sysfs: Unknown parameter
#   mount -t devpts -o gid=5,mode=620,ptmxmode=0666 -> ok
#
# So "defaults" becomes "rw" -- not the VFS flags, which fail the same way --
# and devpts loses the "defaults," prefix while keeping its real options.
#
# The root line is separately wrong: / is romfs here, not ext2.
#
# Backreferences, so the skeleton's column layout survives untouched; its tab
# counts are not worth reproducing by hand.
sed -i \
    -e 's|^/dev/root\(.*\)ext2\(.*\)rw,noauto|/dev/root\1romfs\2ro,noauto|' \
    -e 's|\tdefaults\t|\trw\t|' \
    -e 's|\tdefaults,gid=5|\tgid=5|' \
    "$TARGET_DIR/etc/fstab"

# ...and the remaining one is not in fstab at all. The skeleton inittab has
#
#   ::sysinit:/bin/mount -o remount,rw /
#
# which cannot succeed here: / is a read-only romfs out of an mtd-rom window,
# and there is nothing to remount it read-write onto. busybox passes the
# current option string back to the kernel, romfs rejects "relatime", and the
# first two lines the guest ever prints are a failure that does not matter.
sed -i '\|^::sysinit:/bin/mount -o remount,rw /$|d' "$TARGET_DIR/etc/inittab"

# Buildroot drops this marker when it builds a filesystem image (fs/common.mk);
# mkromfs.py is not wired into that path, so it would otherwise ship and show
# up in ls / on the guest. Buildroot recreates it on the next build.
rm -f "$TARGET_DIR/THIS_IS_NOT_YOUR_ROOT_FILESYSTEM"

# The ncurses demo.
#
# Built here rather than as a buildroot package because it is one file. The
# flags are the ones buildroot passes to every package on this target and all
# of them matter: -fPIC with -Wl,-elf2flt=-r produces a bFLT carrying PIC-GOT
# relocations, and -static is not optional -- BR2_BINFMT_FLAT_SHARED is off, so
# there is no runtime loader on the guest to resolve a dynamic libncurses.
# Without these gcc silently emits a plain ELF that the kernel cannot exec.
#
# flthdr -s raises the stack from the 4 kB default. Under NOMMU the stack is
# part of the single contiguous exec allocation, so it is fixed at build time
# and cannot grow at runtime; ncurses overruns 4 kB. 32 kB keeps the total
# inside an order-6 block.
NCTEST_SRC="$(dirname "$0")/nctest.c"
if [ -f "$NCTEST_SRC" ] && [ -f "$STAGING_DIR/usr/lib/libncurses.a" ]; then
    "$HOST_DIR/bin/riscv32-linux-gcc" -Os -g0 -fPIC -s "$NCTEST_SRC" -o "$TARGET_DIR/usr/bin/nctest" -lncurses -Wl,-elf2flt=-r -static
    "$HOST_DIR/bin/riscv32-linux-flthdr" -s 32768 "$TARGET_DIR/usr/bin/nctest"
    # elf2flt leaves the unstripped ELF beside the flat binary. Nothing else
    # removes it because this compiles straight into TARGET_DIR, and at 1.4 MB
    # it was larger than the rest of the rootfs put together -- it pushed the
    # image past the 2 MB window main.c copies into.
    rm -f "$TARGET_DIR/usr/bin/nctest.gdb"
    echo "post-build: built /usr/bin/nctest ($(stat -c %s "$TARGET_DIR/usr/bin/nctest") bytes)" >&2
else
    echo "post-build: skipping nctest (no source or libncurses.a)" >&2
fi


# ---------------------------------------------------------------------------
# The second busybox: /bin/busybox-extra
#
# The main busybox is at 495,616 bytes against a hard 524,288 ceiling (see
# buildroot/README.md), so it cannot take many more applets at any RAM budget
# -- the limit is allocation contiguity, not capacity. A second binary
# sidesteps it entirely: separate exec, separate order-7 block, and the two
# never have to fit together.
#
# What lives here is everything whose GNU equivalent is "depends on
# BR2_USE_MMU" in buildroot and therefore cannot be built for this target at
# all: tar, top, patch, and the coreutils family (du, stat, expr, uptime,
# mktemp, install, truncate...). Plus vi, which is the editor that always
# works because it has no library dependency, and the gzip compressor, whose
# GNU version measured at 1,003,520 bytes in one allocation.
#
# Built in a COPY of buildroot's busybox build directory rather than in it:
# reusing it would leave a foreign .config behind that buildroot's stamps do
# not notice, so the next build would silently ship the wrong applet set.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXTRA_FIXUPS="$SCRIPT_DIR/busybox-extra.fixups"
BB_SRC=""
for candidate in "$BUILD_DIR"/busybox-[0-9]*/; do
    if [ -d "$candidate" ]; then BB_SRC="$candidate"; break; fi
done
EXTRA_DIR="$BUILD_DIR/busybox-extra"
BB_MAKE="ARCH=riscv CROSS_COMPILE=$HOST_DIR/bin/riscv32-linux- SKIP_STRIP=y"
# These are what make the output bFLT rather than a plain ELF, and nothing
# warns if they are missing -- the build succeeds and the kernel then cannot
# exec the result. Source of truth is package/Makefile.in:216, which appends
# -fPIC and -Wl,-elf2flt=-r to TARGET_CFLAGS/TARGET_LDFLAGS for riscv +
# BR2_BINFMT_FLAT; the rest mirrors BR2_TARGET_OPTIMIZATION/BR2_TARGET_LDFLAGS.
BB_CFLAGS="-Os -fPIC -fno-asynchronous-unwind-tables -fno-unwind-tables"
BB_CFLAGS="$BB_CFLAGS -ffunction-sections -fdata-sections -Wl,-elf2flt=-r"
BB_LDFLAGS="-s -Wl,--gc-sections -Wl,-elf2flt=-r"

if [ -n "$BB_SRC" ] && [ -f "$EXTRA_FIXUPS" ]; then
    if [ ! -f "$EXTRA_DIR/.stamp_extra" ] || [ "$EXTRA_FIXUPS" -nt "$EXTRA_DIR/.stamp_extra" ]; then
        echo "post-build: building busybox-extra from $BB_SRC" >&2
        rm -rf "$EXTRA_DIR"
        cp -a "$BB_SRC" "$EXTRA_DIR"
        # allnoconfig, then turn on exactly what the fixups name. Starting
        # from defconfig and subtracting does not work -- this kconfig keeps
        # the FIRST assignment of a symbol, which is why busybox-fixup.sh
        # deletes each line before appending it.
        make -C "$EXTRA_DIR" $BB_MAKE allnoconfig >/dev/null
        sh "$SCRIPT_DIR/busybox-fixup.sh" "$EXTRA_DIR/.config" "$EXTRA_FIXUPS"
        # oldconfig, not olddefconfig: busybox 1.38 has no such target. It
        # prompts for anything unresolved, so feed it empty lines.
        yes "" | make -C "$EXTRA_DIR" $BB_MAKE oldconfig >/dev/null
        CFLAGS="$BB_CFLAGS" make -C "$EXTRA_DIR" $BB_MAKE \
            EXTRA_LDFLAGS="$BB_LDFLAGS" -j"$(nproc)" busybox busybox.links >/dev/null
        touch "$EXTRA_DIR/.stamp_extra"
    fi

    if ! head -c 4 "$EXTRA_DIR/busybox" | grep -q bFLT; then
        echo "post-build: busybox-extra is not bFLT -- the elf2flt flags did not take" >&2
        exit 1
    fi
    install -D -m 0755 "$EXTRA_DIR/busybox" "$TARGET_DIR/bin/busybox-extra"
    # 4 kB is the elf2flt default and vi and top both overrun it. Under NOMMU
    # the stack is part of the single exec allocation and cannot grow at
    # runtime, so it is fixed here or not at all.
    "$HOST_DIR/bin/riscv32-linux-flthdr" -s 32768 "$TARGET_DIR/bin/busybox-extra"

    # Symlink every applet it provides, except where something already owns
    # the path -- the main busybox and the standalone GNU packages win, since
    # their versions have the features compiled in.
    added=0
    skipped=0
    while IFS= read -r applet; do
        [ -n "$applet" ] || continue
        mkdir -p "$TARGET_DIR$(dirname "$applet")"
        if [ -e "$TARGET_DIR$applet" ] || [ -L "$TARGET_DIR$applet" ]; then
            skipped=$((skipped + 1))
        else
            ln -s /bin/busybox-extra "$TARGET_DIR$applet"
            added=$((added + 1))
        fi
    done < "$EXTRA_DIR/busybox.links"
    echo "post-build: busybox-extra linked $added applets, $skipped already provided" >&2
else
    echo "post-build: skipping busybox-extra (no busybox build dir or fixups)" >&2
fi

# Two binaries that go no further, each for a measured reason.
#
# bzip2recover wants 1,662,976 bytes in one allocation -- order 8, which this
# machine cannot serve. It reconstructs data from damaged .bz2 files;
# bzip2/bunzip2/bzcat are 135,168 and stay.
#
# luac compiles Lua to bytecode, which is a build-host job, and it is 187 kB
# of a rootfs partition with better uses.
rm -f "$TARGET_DIR/usr/bin/bzip2recover" "$TARGET_DIR/bin/bzip2recover"
rm -f "$TARGET_DIR/usr/bin/luac" "$TARGET_DIR/bin/luac"

# Raise the bFLT stack on the ncurses-linked binaries.
#
# Same reason as nctest: 4 kB is the elf2flt default, the stack is part of the
# single contiguous exec allocation, and ncurses overruns it. nano gets 64 kB
# because in --tiny mode it also keeps working buffers on the stack.
for prog in usr/bin/nano usr/bin/less bin/nano bin/less; do
    if [ -f "$TARGET_DIR/$prog" ] && head -c 4 "$TARGET_DIR/$prog" | grep -q bFLT; then
        case "$prog" in
            *nano) size=65536 ;;
            *)     size=32768 ;;
        esac
        "$HOST_DIR/bin/riscv32-linux-flthdr" -s "$size" "$TARGET_DIR/$prog"
        echo "post-build: $prog stack -> $size" >&2
    fi
done

# file(1) is not installed, and the reason is not its database size.
#
# It was tried three ways and all three failed, so this is here to stop the
# fourth attempt.
#
# 1. Stock. magic.mgc is 10,353,312 bytes -- larger than the whole 8 MB rootfs
#    partition on its own, because it covers every format anyone contributed.
#
# 2. A trimmed *compiled* database. Does not load at any size: libmagic mmaps
#    magic.mgc and then mprotects it, and NOMMU has no mprotect --
#      file: cannot mprotect `/usr/share/misc/magic.mgc' (Function not implemented)
#    (magic_load tries "$path.mgc" before "$path", so a .mgc must be absent,
#    not merely unused.)
#
# 3. A trimmed *text* database, which libmagic parses with fopen/fgets and so
#    sidesteps mprotect. This is the one that looked right and is not:
#
#      330 kB of magic  ->  "Bad file format 1", Aborted
#       29 kB of magic  ->  nommu: Allocation of length 7344128 from
#                           process 36 (file) failed
#                           /bin/busybox: ERROR: (null)
#
#    Note the second line. A 29 kB database still asks for 7.3 MB in ONE
#    allocation, so the request is not proportional to the database and
#    shrinking it further changes nothing. Meanwhile the largest block this
#    machine can produce at all is 4 MB (buddyinfo tops out at 4*4096kB), so
#    a 7.3 MB contiguous request can never be served, at any load, on any
#    boot.
#
# Making file(1) work here would mean patching file itself to bound that
# allocation, not curating Magdir. Until someone does that, BR2_PACKAGE_FILE
# stays off and this removes any magic file a future config change drags in.
rm -f "$TARGET_DIR/usr/share/misc/magic" "$TARGET_DIR/usr/share/misc/magic.mgc"
rm -f "$TARGET_DIR/usr/bin/file" "$TARGET_DIR/bin/file"


# ---------------------------------------------------------------------------
# A native toolchain in the guest: as, objcopy and mkflt.lua.
#
# The point is to be able to write assembly on the board and run it. That
# needs three things, and the third is the one people forget:
#
#   as          assembles to an ELF object
#   <a linker>  lays the sections out and applies the relocations
#   <a packer>  converts the result to bFLT -- WITHOUT THIS IT WILL NOT RUN.
#
# The guest kernel executes bFLT. Its other loader, binfmt_elf_fdpic, refuses
# a plain ELF on NOMMU:
#
#   /* fs/binfmt_elf_fdpic.c */
#   /* nommu can only load ET_DYN (PIE) ELF */
#   if (exec_params.hdr.e_type != ET_DYN) ... -ENOEXEC
#
# ...and riscv has no FDPIC ABI in the toolchain, so nothing can produce a
# binary that path accepts. ELF in, bFLT out, or it does not execute.
#
# The obvious answers to the last two are ld and elf2flt, and BOTH WERE SHIPPED
# AND THEN REMOVED. ld does not run here: binfmt_flat makes one contiguous
# allocation per exec, ld is 5,283,840 bytes, and the guest's allocator has no
# block that big once it has booted --
#
#   nommu: Allocation of length 5283840 from process 36 (ld) failed
#   Normal: ... 3*1024kB 2*2048kB 3*4096kB 0*8192kB = 25400kB
#
# -- 25 MB free, largest block 4 MB, on every attempt. That is also why
# patches/linux/0002-* raises MAX_PAGE_ORDER to 11: it was an attempt at this,
# and it is kept because an 8 MB order is still the difference between a 4 MB
# per-exec ceiling and an 8 MB one for everything else. elf2flt consumes ld's
# output, so it went with ld.
#
# What replaced them is /usr/lib/mkflt.lua: 12 kB of Lua that lays the
# sections out, applies the rv32 relocations and writes the bFLT header,
# calling objcopy for each section's bytes. For one freestanding object with
# no libraries -- which is what hand-written assembly is -- that is the whole
# job. See buildroot/README.md.
# ---------------------------------------------------------------------------

# Everything BR2_PACKAGE_BINUTILS_TARGET installs that is NOT in this list gets
# removed. Measured: the full set is 21.5 MB against a 12.4 MB rootfs
# partition, so this is a space decision, not a taste one.
#
#   as       1,286,816   assembles
#   objcopy    955,868   -O binary; mkflt.lua reads sections through it
#   ld       5,280,148   NOT kept, and not for space reasons -- it does not
#                        RUN. binfmt_flat makes one contiguous allocation per
#                        exec and this guest has no 5 MB block once it has
#                        booted:
#                          nommu: Allocation of length 5283840 from ld failed
#                          Normal: ... 3*1024kB 2*2048kB 3*4096kB 0*8192kB
#                        25 MB free, largest block 4 MB, every time. See
#                        buildroot/README.md.
#   elf2flt    948,248   NOT kept -- it converts ld's output, so it goes with
#                        ld. /usr/lib/mkflt.lua replaces both.
#   objdump  1,390,568   NOT kept. Add it here if you want on-board
#                        disassembly and are willing to re-check the size.
#
# The rest -- gprof, nm, addr2line, strip, ar, ranlib, size, readelf, strings,
# c++filt, elfedit -- is 8.5 MB of tools this board has no use for.
BINUTILS_KEEP="as objcopy"

if [ -x "$TARGET_DIR/usr/bin/as" ]; then
    # binutils installs its whole tool set TWICE: once in /usr/bin and again
    # in /usr/<target-triple>/bin, the layout a cross toolchain uses. On the
    # target the second copy is meaningless -- there is only one architecture
    # here -- and it is expensive in a way that does not show up in du:
    #
    #   $ ls -i usr/bin/as usr/riscv32-buildroot-linux-uclibc/bin/as
    #   737628 usr/bin/as    737628 usr/riscv32-.../bin/as
    #
    # They are hardlinks, so du counts them once, but romfs has no hardlinks
    # in the image mkromfs.py writes -- every directory entry gets its own
    # copy of the data. Leaving this directory in place made the image
    # 34,544,032 bytes against a 12.4 MB partition, which reads as "the
    # toolchain is enormous" when really it was there twice.
    if [ -d "$TARGET_DIR/usr/riscv32-buildroot-linux-uclibc" ]; then
        rm -rf "$TARGET_DIR/usr/riscv32-buildroot-linux-uclibc"
        echo "post-build: removed the duplicate /usr/<triple>/bin tool set" >&2
    fi

    for f in "$TARGET_DIR"/usr/bin/*; do
        b=$(basename "$f")
        case " $BINUTILS_KEEP " in
            *" $b "*) continue ;;
        esac
        # Only consider things binutils actually owns, so this never eats an
        # unrelated package's binary.
        case "$b" in
            addr2line|ar|c++filt|elfedit|gprof|ld|ld.bfd|nm|objdump|ranlib|\
            readelf|size|strings|strip|dwp|gp-*)
                if [ -f "$f" ] && [ ! -L "$f" ]; then
                    rm -f "$f"
                fi
                ;;
        esac
    done
    # strings is a busybox-extra applet; putting the symlink back keeps the
    # command available at no cost after binutils' 1.2 MB copy is gone.
    if [ ! -e "$TARGET_DIR/usr/bin/strings" ] \
       && [ -x "$TARGET_DIR/bin/busybox-extra" ]; then
        ln -sf /bin/busybox-extra "$TARGET_DIR/usr/bin/strings"
    fi
    kept=$(cd "$TARGET_DIR/usr/bin" && ls $BINUTILS_KEEP 2>/dev/null | tr '\n' ' ')
    echo "post-build: binutils trimmed to: $kept" >&2
fi

# ---- elf2flt for the target: removed ------------------------------------
# There used to be 60 lines here that cross-built elf2flt against the target
# binutils libraries, including a fight with binutils 2.45's libbfd needing
# libsframe on the link line. It worked, and the binary is useless: elf2flt
# converts ELF executables, ld is the only thing that produces those, and ld
# cannot run on this board (see the size table above). /usr/lib/mkflt.lua does
# the same job from an object file instead.
#
# git history has the code if the allocator constraint ever changes.

# The assembly example: source in /usr/share, built binary in /usr/bin.
#
# The root filesystem is read-only romfs, so copy the source somewhere
# writable before editing:
#
#   cp /usr/share/hello.S /home/ && cd /home
#
# ...but hello.S itself cannot be rebuilt there: it links against crt1.o and
# libc.a, and neither is on the board. /usr/share/hello-onboard.S is the
# freestanding version that can:
#
#   cp /usr/share/hello-onboard.S /home/ && cd /home && mkflt hello-onboard.S
#
# -Wl,--no-relax is load-bearing and the reason is in hello.S: without it the
# linker rewrites GOT accesses into x0-relative absolute stores, which bFLT
# cannot relocate, and the program dies with a store access fault to a
# link-time offset. It only bites binaries small enough to sit in the low
# 2 kB, which is why nothing else on this board needs it.
if [ -f "$SCRIPT_DIR/hello.S" ]; then
    mkdir -p "$TARGET_DIR/usr/share"
    cp "$SCRIPT_DIR/hello.S" "$TARGET_DIR/usr/share/hello.S"
    if "$HOST_DIR/bin/riscv32-linux-gcc" -Os -fPIC -static -Wl,--no-relax            -Wl,-elf2flt=-r "$SCRIPT_DIR/hello.S"            -o "$TARGET_DIR/usr/bin/hello" 2>/dev/null; then
        rm -f "$TARGET_DIR/usr/bin/hello.gdb"
        if head -c 4 "$TARGET_DIR/usr/bin/hello" | grep -q bFLT; then
            echo "post-build: built /usr/bin/hello ($(stat -c %s                 "$TARGET_DIR/usr/bin/hello") bytes)" >&2
        else
            echo "post-build: NOTE hello is not bFLT; removing" >&2
            rm -f "$TARGET_DIR/usr/bin/hello"
        fi
    else
        echo "post-build: NOTE hello.S did not build" >&2
    fi
fi

# ---- the on-board build kit ---------------------------------------------
#
# Three files that turn `as` and `objcopy` into a complete toolchain:
#
#   /usr/bin/mkflt              as -mno-relax, then mkflt.lua
#   /usr/lib/mkflt.lua          lays sections out, applies relocations, writes
#                               the bFLT header. A linker, for the one case
#                               that matters here: a single freestanding
#                               object with no libraries.
#   /usr/share/hello-onboard.S  an example that builds and runs on the board
#
# Total 24 kB, against 6.2 MB for ld + elf2flt -- neither of which works here
# anyway. lua was already in the image.
if [ -f "$SCRIPT_DIR/mkflt.sh" ] && [ -f "$SCRIPT_DIR/mkflt.lua" ]; then
    install -D -m 0755 "$SCRIPT_DIR/mkflt.sh"  "$TARGET_DIR/usr/bin/mkflt"
    install -D -m 0644 "$SCRIPT_DIR/mkflt.lua" "$TARGET_DIR/usr/lib/mkflt.lua"
    # hello-onboard.S is the example; stress.S is the one that exercises the
    # relocation types mkflt.lua claims to handle -- .data and .bss laid out
    # contiguously with the text, a call, a loop, and a store through an
    # lla'd address. It is 1.3 kB and it is the only regression test the
    # on-board toolchain has.
    for example in hello-onboard.S stress.S; do
        if [ -f "$SCRIPT_DIR/$example" ]; then
            install -D -m 0644 "$SCRIPT_DIR/$example" \
                "$TARGET_DIR/usr/share/$example"
        fi
    done
    echo "post-build: installed mkflt, mkflt.lua and the example sources" >&2
fi

# ---- reclaim space -------------------------------------------------------
# bzip2 installs bzip2, bunzip2 and bzcat as three separate 121,876-byte files
# rather than links -- 243 kB of the same program stored twice more.
# mkromfs.py stores each one, so making them symlinks is a straight saving
# with no change in behaviour.
for dup in bunzip2 bzcat; do
    if [ -f "$TARGET_DIR/usr/bin/$dup" ] && [ ! -L "$TARGET_DIR/usr/bin/$dup" ] \
       && [ -f "$TARGET_DIR/usr/bin/bzip2" ]; then
        rm -f "$TARGET_DIR/usr/bin/$dup"
        ln -sf bzip2 "$TARGET_DIR/usr/bin/$dup"
        echo "post-build: $dup -> bzip2 (was a full copy)" >&2
    fi
done

# GNU builds of commands busybox already provides. Each costs 60-350 kB, and
# with a 9 MB toolchain moving in they no longer fit. Removing the GNU binary
# leaves busybox's applet in place -- it is a separate file at /bin/<name>, so
# the command still works, just with fewer options.
#
# The busybox applet is only assumed to exist if busybox.links says so.
# Deleting a GNU binary for an applet busybox does NOT build would silently
# remove the command, which is how you end up with a board that has no sed.
# diff3 and sdiff have no busybox equivalent at all and are kept for that
# reason -- there is no "GNU or busybox" choice to make, only "or nothing".
#
# If you would rather have GNU sed than a native assembler, this is the list
# to shorten; see the arithmetic in partitions.csv.
reclaimed=0
kept_no_applet=""
for dup in sed grep bc dc which cmp less; do
    have_applet=no
    if [ -n "$LINKS" ]; then
        if grep -qxF "/bin/$dup" "$LINKS" || grep -qxF "/usr/bin/$dup" "$LINKS"
        then
            have_applet=yes
        fi
    fi
    if [ "$have_applet" = no ]; then
        kept_no_applet="$kept_no_applet $dup"
        continue
    fi
    for d in bin usr/bin; do
        f="$TARGET_DIR/$d/$dup"
        if [ -f "$f" ] && [ ! -L "$f" ]; then
            size=$(stat -c %s "$f")
            rm -f "$f"
            reclaimed=$((reclaimed + size))
        fi
    done
done

# lesskey and lessecho are part of GNU less and are useless without it.
if [ ! -f "$TARGET_DIR/usr/bin/less" ]; then
    for orphan in lesskey lessecho; do
        f="$TARGET_DIR/usr/bin/$orphan"
        if [ -f "$f" ] && [ ! -L "$f" ]; then
            reclaimed=$((reclaimed + $(stat -c %s "$f")))
            rm -f "$f"
        fi
    done
fi

echo "post-build: reclaimed $reclaimed bytes of GNU duplicates" >&2
if [ -n "$kept_no_applet" ]; then
    echo "post-build: kept (busybox has no applet):$kept_no_applet" >&2
fi

# elf2flt leaves an unstripped ELF beside every flat binary it makes. Nothing
# else removes the ones from packages that install straight into TARGET_DIR,
# and they are far larger than the binaries themselves.
find "$TARGET_DIR" -name '*.gdb' -type f -delete

# Nothing below the order-7 ceiling, or the board fails execs under load
# instead of at build time.
sh "$SCRIPT_DIR/checkflat.sh" "$TARGET_DIR" "$HOST_DIR/bin/riscv32-linux-flthdr"
