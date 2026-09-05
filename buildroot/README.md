ESP32-P4 hypervisor guest: buildroot side
=========================================

The buildroot tree itself is not in this repo -- it lives in WSL at
`~/buildroot-2026.05.1`. These are the pieces of it that matter, so the work is
not stranded there.

Keep that mirror complete. Two things needed to build a working guest were
missing from it and were only found by doing a clean build: the kernel patch
below (which existed only as a hand-edit in `output/build/linux-6.8-rc1/`) and
`busybox.config`, which `BR2_PACKAGE_BUSYBOX_CONFIG` points at. Before
trusting this directory, diff it against the live tree:

    diff -rq buildroot/board-esp32p4 ~/buildroot-2026.05.1/board/esp32p4

Layout in buildroot
-------------------
    board/esp32p4/mkromfs.py         <- board-esp32p4/mkromfs.py
    board/esp32p4/checkromfs.py      <- board-esp32p4/checkromfs.py
    board/esp32p4/post-build.sh      <- board-esp32p4/post-build.sh
    board/esp32p4/busybox-fixup.sh   <- board-esp32p4/busybox-fixup.sh
    board/esp32p4/busybox.config     <- board-esp32p4/busybox.config
    board/esp32p4/busybox.fragment   <- board-esp32p4/busybox.fragment
    board/esp32p4/busybox.fixups     <- board-esp32p4/busybox.fixups
    board/esp32p4/busybox-extra.fixups <- board-esp32p4/busybox-extra.fixups
    board/esp32p4/checkflat.sh       <- board-esp32p4/checkflat.sh
    board/esp32p4/patches/           <- board-esp32p4/patches/  (BR2_GLOBAL_PATCH_DIR)
    board/esp32p4/linux.fragment     <- board-esp32p4/linux.fragment
    board/esp32p4/uclibc.fragment    <- board-esp32p4/uclibc.fragment
    .config                          <- buildroot.config
    package/busybox/busybox.mk       <- busybox-fixups-hook.patch

Building
--------
Buildroot rejects the PATH it inherits here ("Your PATH contains spaces"), so
build with a clean one:

    cd ~/buildroot-2026.05.1
    export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
    make -j"$(nproc)"
    python3 board/esp32p4/mkromfs.py output/target output/images/rootfs.romfs rootfs
    python3 board/esp32p4/checkromfs.py output/images/rootfs.romfs output/target

post-build.sh already ran checkflat.sh as part of `make`, so if that succeeded
no binary is over the order-7 ceiling.

Changing a toolchain option -- BR2_TOOLCHAIN_BUILDROOT_WCHAR, anything in
uclibc.fragment -- means `rm -rf output` first. Buildroot will not rebuild the
toolchain on its own and the stale libc silently stays.

Then flash, from the project root:

    parttool.py -p COM17 -b 921600 write_partition -n kernel --input guest/Image-romfs
    parttool.py -p COM17 -b 921600 write_partition -n rootfs --input guest/rootfs.romfs
    parttool.py -p COM17 -b 921600 write_partition -n dtb    --input dts/p4-phase6.dtb

The kernel patch, and why a clean build used to produce a dead board
--------------------------------------------------------------------
`board/esp32p4/patches/linux/0001-*.patch` changes one line:

    -	default 0x80000000 if !MMU
    +	default 0x48000000 if !MMU

in arch/riscv/Kconfig. Read it before touching anything else here.

It has to be a source patch. CONFIG_PAGE_OFFSET is a promptless `hex` symbol
with three hardcoded defaults, so a defconfig, a fragment, or a hand-edited
.config all lose to `olddefconfig`. Changing the default is the only way.

And it is load-bearing. For !MMU, arch/riscv/include/asm/pgtable.h:25 makes
KERNEL_LINK_ADDR equal PAGE_OFFSET and vmlinux.lds.S:15,33 links the image
there. Mainline's 0x80000000 is not where this board's RAM is -- the P4's
PSRAM window starts at 0x48000000 -- so an unpatched kernel starts at
0x48000000 where the monitor put it, runs the PC-relative early setup, then
takes its first absolute jump into 0x80000000 and dies:

    W: guest has taken no trap for 5 s -- wedged?
    15: cause=1 epc=0x80006310 tval=0x80006310 priv=3 insn=0x00000000/0

**This patch previously existed only as a hand-edit inside
output/build/linux-6.8-rc1/.** `rm -rf output` therefore produced a kernel
that built, flashed and booted far enough to look plausible, and the repo could
not reproduce a working guest. It is a patch file now so that it cannot happen
again -- and note what it means for the project's headline claim: the guest
kernel is mainline 6.8-rc1 plus this one line. It is not unmodified.

How to tell which kernel you are holding, without booting it -- count absolute
words in each range:

    python3 - <<'EOF'
    import struct
    d = open('guest/Image-romfs','rb').read()
    a = sum(1 for i in range(0,len(d)-4,4)
            if 0x48000000 <= struct.unpack_from('<I',d,i)[0] < 0x4A000000)
    b = sum(1 for i in range(0,len(d)-4,4)
            if 0x80000000 <= struct.unpack_from('<I',d,i)[0] < 0x80800000)
    print(a, b, 'patched' if a > b else 'UNPATCHED')
    EOF

A correct image is ~15668 vs 608. An unpatched one is ~5941 vs 10325.

Why the rootfs is romfs on flash
--------------------------------
It used to be an initramfs built into the kernel image, which cost twice:
~2.4 MB of ramfs pinned for the whole uptime (`unevictable` in Mem-Info) plus a
second copy of the same cpio inside the 2.8 MB Image. With romfs on flash,
`unevictable` is 16 kB and the Image is 2.2 MB.

The per-exec allocation ceiling
-------------------------------
This is the important one, and it is about a power of two. It is also the
constraint that does not care how much RAM the guest has -- read this before
concluding that a bigger machine would fix anything.

`binfmt_flat` copies text+data+bss+stack into ONE contiguous allocation per
exec, and the page allocator serves it from a power-of-two block. The size is

    text + data + max(bss + stack, relocs * 4), rounded up to a page

read straight out of `flthdr`:

    output/host/bin/riscv32-linux-flthdr output/target/bin/busybox

Stock busybox came to 1,142,784 bytes -> order 9, so every process occupied a
2 MB block for 1.1 MB of need, and on a 29 MB NOMMU machine there is no second
2 MB block once anything is running. Every exec then failed with

    nommu: Allocation of length 1142784 from process 65 (cat) failed
    binfmt_flat: Unable to allocate RAM for process text/data, errno -12

with 20 MB free.

The ceiling was called 524,288 (order 7) here for a long time, on the theory
that the free lists never held anything bigger. **That was wrong**, and the
measurement that settled it is worth keeping. `/proc/buddyinfo` on the board:

    order    0    1    2    3    4    5    6    7    8    9   10
    boot     1    2    8    8    7    7    7    2    1    2    4
    after    0    2    0    4    4    7    9    0    2    1    4

("after" = `ls -R /`, `dmesg`, `grep`, `tar`, a 50,000-element Lua table and
an editor.) Four free order-10 blocks at boot, and still four after real work:
ordinary use churns the LOW orders and leaves the top of the allocator alone.

So the binding constraint was never fragmentation. It was `MAX_PAGE_ORDER`
itself, which defaults to 10 -- a 4 MB cap on any single allocation, and
therefore on any single program. That is what `file(1)` hit:

    nommu: Allocation of length 7344128 from process 63 (file) failed

The old 512 kB figure needlessly excluded GNU gzip, bzip2recover and others.

`patches/linux/0002-riscv-allow-ARCH_FORCE_MAX_ORDER-to-be-configured.patch`
adds the Kconfig symbol riscv is missing, and `linux.fragment` sets
`CONFIG_ARCH_FORCE_MAX_ORDER=11` for an 8 MB cap -- enough for GNU ld at
5.3 MB. Verified on the board: `/proc/buddyinfo` grew from 11 columns to 12,
with a free order-11 block. `checkflat.sh` enforces the same number at build
time and the two must be changed together.

Raising it further is possible but not free: `pageblock_order` follows
`MAX_PAGE_ORDER` when huge pages are not in use, so on a 29 MB machine an
8 MB pageblock is already a quarter of memory and the allocator's
anti-fragmentation grouping stops meaning much.

What is still true is that there is no demand paging. A binary is resident in
full for the life of the process whether it touches all of itself or not, and
NOMMU cannot compact -- compaction needs MMU page migration -- so a long-lived
system with heavy exec churn can still degrade one way. `checkflat.sh` prints
a BIG note for anything over 1 MB for that reason.

Measured, same board, same kernel:

    1,142,784  order 9  ->  1 process, then every exec fails
      606,208  order 8  -> 12 processes, then execs and pipelines fail
      520,192  order 7  -> 55 processes, zero allocation failures

Two things that look like they should help and do not:

1. **`vm.nr_trim_pages = 0`.** `do_mmap_private` in mm/nommu.c allocates the
   power-of-two block then hands the tail back (`split_page` plus a
   `__free_page` per excess page) when the waste is >= `sysctl_nr_trim_pages`,
   default 1. That returned tail is what fragments the buddy allocator, so
   switching it off looks like the fix. It is not: measured with it off, free
   memory *dropped* from 15.2 MB to 9.7 MB, `0*1024kB` was unchanged, and the
   same execs still failed. The initial request needs a contiguous
   power-of-two block whether or not the tail comes back.

2. **`-fno-asynchronous-unwind-tables` to drop `__EH_FRAME_BEGIN__`.** That
   symbol covers ~25 kB of `.eh_frame`, dead weight in a static bFLT binary
   that never throws. Setting the flag in BR2_TARGET_OPTIMIZATION and
   rebuilding busybox moved it 576 bytes; rebuilding uClibc too moved it by
   nothing, because the section comes from prebuilt `libgcc.a`/`crtbegin.o`,
   which needs the toolchain itself rebuilt. `-ffunction-sections
   -fdata-sections -Wl,--gc-sections` produced a byte-identical binary --
   BR2_ENABLE_LTO is already on and subsumes it.

There is also ~25 kB of soft quad-precision float (`__addtf3`, `__subtf3`,
`__multf3`, `__divtf3`, `__getf2`, `__lttf2`, `__fixunstfsi`, `__floatunsitf`).
busybox_unstripped.map gives the chain: uClibc `_vfprintf_internal` ->
`_fpmaxtostr` -> `__fpmax_t`, which is `long double` = 128-bit on rv32.
Turning off UCLIBC_HAS_FLOATS removes all of it and also breaks awk, so it
stays.

What the cuts cost, and how it was paid back
--------------------------------------------
The 104-applet build this replaced had awk, tar, gzip, vi, less, diff, dd and
the checksum tools *disabled*. Those eight families are ~85 kB and are exactly
the difference between order 7 and order 8. Keeping all of them in one binary
was tried and measured at 606,208 (order 8, 12 processes, pipelines failing
under load), so some had to go.

**They are back now, in other binaries.** See "Two busyboxes" below. The thing
worth understanding is why that works when growing busybox does not: the
ceiling is on a *single* allocation, not on the total. Two 500 kB binaries are
two order-7 blocks and both always succeed; one 1 MB binary is an order-8 block
and essentially never does. So the fix for "I want more tools" is never a
bigger busybox -- it is more binaries.

Kept, because they are what you type: awk, sed, grep, find, xargs, gunzip,
zcat, dd, md5sum, sha256sum, xxd, sort, uniq, cut, tr, head, tail, wc, cmp,
paste, nl, fold, seq, ps, free, df, mount, dmesg, kill, killall, pidof, stty,
resize, printf, test, id, which.

Still out of the main busybox, and where each one went instead:

    vi                -> busybox-extra (and nano, as a real package)
    tar, top, patch   -> busybox-extra; the GNU versions need an MMU
    du expr stat      -> busybox-extra; coreutils needs an MMU
    uptime mktemp
    less, more        -> less, as a standalone package
    diff              -> diffutils, as a standalone package
    gzip compressor   -> gzip, as a standalone package (also bzip2, xz)
    user management   still out. addgroup/adduser/passwd/su/sulogin/vlock:
                      single-user board, root logs in on the serial console.
    net, modules,     still out. No NIC configured, no loadable module
    i2c, VT, mkfs     support, no i2c, nothing to mkfs.

Headroom in the main busybox is 4,096 bytes -- one page. Do not spend it; add
to busybox-extra or as a package instead. checkflat.sh enforces this now.

Two busyboxes, and the standalone packages
------------------------------------------
The guest went from 95 commands to **197**. The main busybox is at 495,616
bytes with 28,672 free under the ceiling -- it got *smaller*, not larger,
because enabling wchar changed the uClibc build (data went 70,696 -> 46,752).
It still cannot absorb 100 more applets, so the commands come from two other
places.

**/bin/busybox-extra**, built by post-build.sh from a copy of buildroot's
busybox build directory with board/esp32p4/busybox-extra.fixups applied on top
of `make allnoconfig`. It is a separate exec and therefore a separate order-7
block. It carries everything whose GNU equivalent buildroot marks
`depends on BR2_USE_MMU` -- those packages use fork(), which does not exist
here, so they cannot be built for this target at all:

    tar                              package/tar         NEEDS-MMU
    top, pmap, iostat, mpstat        package/procps-ng   NEEDS-MMU
    patch                            package/patch       NEEDS-MMU
    du stat expr uptime mktemp
    install truncate realpath ...    package/coreutils   NEEDS-MMU

plus **vi**, which is the editor that always works because it has no library
dependency.

It is built in a *copy* of the build directory on purpose. Reusing buildroot's
own would leave a foreign .config behind that buildroot's stamps do not notice,
and the next build would silently ship the wrong applet set.

**Standalone packages.** These build normally and each gets its own
allocation, so they are strictly better than busybox applets when available:
nano, less, diffutils, bzip2, xz, sed, grep, tree, bc, which, memtester, lua,
dhrystone, whetstone.

Measured allocations, all against the 524,288 ceiling:

    /bin/busybox         495,616   (28,672 free)
    /usr/bin/nano        421,888   (102,400 free, with a 64 kB stack)
    /usr/bin/less        413,696   (110,592 free, with a 32 kB stack)
    /bin/busybox-extra   ~330,000
    /usr/bin/lua         307,200
    /usr/bin/diff        299,008
    /bin/grep            290,816
    /usr/bin/xz          266,240
    ... 28 bFLT binaries total, none over the ceiling

GNU **gzip** is not among them: it measured 1,003,520 bytes in one allocation,
nearly all .bss, because it keeps its window and I/O buffers as static arrays
and no configure switch shrinks them. busybox-extra provides gzip instead.
**bzip2recover** (1,662,976) and **luac** (187 kB of a bytecode compiler that
belongs on the build host) are deleted in post-build.sh.

Two gates had to come off before most of them were even offered:

    BR2_PACKAGE_BUSYBOX_SHOW_OTHERS   hides every package that duplicates a
                                      busybox applet. Without it less, gzip,
                                      sed, grep, diffutils and nano do not
                                      appear in menuconfig at all.
    BR2_TOOLCHAIN_BUILDROOT_WCHAR     nano depends on BR2_USE_WCHAR. Turning
                                      this on changes the uClibc config, so it
                                      costs a full toolchain rebuild.

Three that cannot be had, with the reason, so nobody re-tries them:

    vim       depends on BR2_USE_MMU # uses fork(). Its runtime is also 11 MB.
    strace    depends on !BR2_RISCV_32 -- excluded upstream on this arch.
    jq        depends on BR2_TOOLCHAIN_HAS_THREADS; uClibc here is
              BR2_PTHREADS_NONE, because NOMMU.

And one that cannot be had at all, for a reason worth writing down because
it looks like a size problem and is not: **file**. Three attempts:

1. Stock. magic.mgc is 10,353,312 bytes, larger than the whole 8 MB rootfs
   partition, because it compiles all ~400 files in `magic/Magdir`.

2. A trimmed *compiled* database. Does not load at any size -- libmagic mmaps
   magic.mgc and then mprotects it, and NOMMU has no mprotect:

       file: cannot mprotect `/usr/share/misc/magic.mgc' (Function not implemented)

   (`magic_load()` tries "$path.mgc" before "$path", so a .mgc has to be
   absent, not merely unused.)

3. A trimmed *text* database, which libmagic parses with fopen/fgets and so
   sidesteps mprotect. This is the one that looked right:

       330 kB of magic  ->  "Bad file format 1", Aborted
        29 kB of magic  ->  nommu: Allocation of length 7344128 from
                            process 36 (file) failed
                            /bin/busybox: ERROR: (null)

Look at the second line. A **29 kB** database still asks for **7.3 MB in one
allocation**, so the request is not proportional to the database and shrinking
it further changes nothing -- which is the opposite of what the first two
attempts implied. Meanwhile the largest block this machine can produce at all
is 4 MB (`buddyinfo` tops out at `4*4096kB`), so a 7.3 MB contiguous request
can never be served on any boot at any load. Making file(1) work here means
patching file to bound that allocation, not curating Magdir.

Two general lessons from that detour. A package list does not tell you which
package ships a 10 MB data blob -- `du -a output/target | sort -rn | head`
before believing a size. And on this machine "it is too big" and "it asks for
one too-large block" are different diagnoses with different fixes; check
`dmesg` for the actual requested length before optimising the wrong thing.

nano is the `--tiny` build: buildroot does `select BR2_PACKAGE_NANO_TINY if
!BR2_USE_MMU`, which compiles out the fork() paths. Its bFLT stack is raised
to 64 kB in post-build.sh for the same reason nctest's is -- under NOMMU the
stack is part of the single exec allocation and cannot grow at runtime.


checkflat.sh: the ceiling is a build error now
----------------------------------------------
board/esp32p4/checkflat.sh walks every bFLT binary in the target tree,
recomputes what binfmt_flat will ask the page allocator for --

    text + data + max(bss + stack, relocs * 4), rounded up to a page

-- and fails the build if any of them exceeds 524,288. post-build.sh runs it
last.

This exists because the ceiling was previously invisible until the board was
running: an order-8 binary builds, installs and boots fine, and then execs
start failing under load with ENOMEM and 20 MB free. That is a bad place to
find out. It also makes `flthdr -s` safe to use -- raising a stack raises the
allocation, and the check notices.


THIS_IS_NOT_YOUR_ROOT_FILESYSTEM
--------------------------------
Buildroot drops that marker when it builds a filesystem image (fs/common.mk),
but mkromfs.py is not wired into that path, so it shipped and showed up in
`ls /`. post-build.sh removes it; buildroot recreates it next build.

The ncurses demo
----------------
board/esp32p4/nctest.c, built by post-build.sh into /usr/bin/nctest. An
"already built" binary only runs here if it was built by *this* toolchain:
rv32 ilp32, uClibc, static, converted to bFLT. The flags all matter -- -fPIC
with -Wl,-elf2flt=-r for a bFLT carrying PIC-GOT relocations, and -static
because BR2_BINFMT_FLAT_SHARED is off, so there is no runtime loader on the
guest. Without them gcc silently emits a plain ELF the kernel cannot exec.

`flthdr -s 32768` raises the stack from the 4 kB default: under NOMMU the
stack is part of the single exec allocation and cannot grow, and ncurses
overruns 4 kB. elf2flt also leaves a 1.4 MB unstripped `nctest.gdb` beside the
binary, which post-build.sh deletes -- it was larger than the rest of the
rootfs and pushed the image past the 2 MB window main.c copies into.

getty passes TERM=vt100 (BR2_TARGET_GENERIC_GETTY_TERM) and terminfo for vt100
ships on the target, so ncurses gets 24x80 from terminfo even though the
serial line reports no size via TIOCGWINSZ. `resize` is kept for when it does
not.
Why not XIP
-----------
XIP is the obvious way to avoid the per-exec copy entirely, and it very nearly
works. Clear the bFLT Load-to-Ram flag (`flthdr -R`) and fs/binfmt_flat.c:526
takes the XIP path: mmap text straight out of the file, allocate only
data+bss+stack. Because / is romfs on an mtd-rom device that mmap really does
resolve into the PSRAM window (romfs_get_unmapped_area -> mtd_get_unmapped_area
-> maprom_point). init was observed running with epc=0x49d23df8, inside the
romfs, text never copied.

Then it corrupts the process. Text and data now live at unrelated addresses and
nothing tells the program where its data went: riscv has no
arch/riscv/include/asm/flat.h, so there is no FLAT_PLAT_INIT to pass a data base
(only m68k and sh define one), and uClibc crt1.S:73 computes gp with
`lla gp, __global_pointer$` -- PC relative. Under XIP that puts gp in the
read-only ROM copy of the GOT, so every global read is unrelocated. init died on
a store to 0x00111a4c with gp = start_data + 0x920, pointing into the romfs.

Doing it properly requires every data reference to go through a data-base
register, which is what -msep-data does on m68k and bfin. RISC-V gcc has no
equivalent, so it is not reachable from here.

Three traps in the build system
-------------------------------
1. **`make linux-reconfigure` gives you a 64-bit kernel.**
   BR2_LINUX_KERNEL_DEFCONFIG is "nommu_virt", and
   arch/riscv/configs/nommu_virt_defconfig does not set the XLEN, so kconfig
   falls back to CONFIG_ARCH_RV64I; buildroot does not force it either, despite
   BR2_RISCV_32=y. The 64-bit .config still builds with the rv32 toolchain and
   the only visible damage is that `li` assembles as lui+addiw -- opcode 0x1b,
   RV64-only -- so the kernel dies on its third instruction with an illegal
   instruction at 0x480000d2. linux.fragment pins CONFIG_ARCH_RV32I=y.

2. **romfs support was removed from buildroot** (Config.in.legacy:1314) and
   there is no genromfs here. mkromfs.py writes the image directly from the
   format in the kernel we boot (include/uapi/linux/romfs_fs.h,
   fs/romfs/super.c); checkromfs.py re-implements the kernel's reader and diffs
   the result against output/target, so a layout bug is caught before it reaches
   the board.

3. **busybox kconfig ignores both of the documented knobs.**
   BUSYBOX_KCONFIG_FIXUP_CMDS runs after BR2_PACKAGE_BUSYBOX_CONFIG and after
   BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES, and this kconfig keeps the FIRST
   assignment of a symbol, so appending to .config does nothing either. The only
   thing that works is a fixup command applied last that deletes each symbol's
   existing line before writing the new one -- busybox-fixup.sh, hooked in by
   busybox-fixups-hook.patch. Everything in busybox.fixups depends on it.

   ROMFS_FS also sits inside "if MISC_FILESYSTEMS" in fs/Kconfig, so
   CONFIG_MISC_FILESYSTEMS=y has to come first or the romfs options are never
   offered and vanish from .config without a word.

The SD card: what it can and cannot buy
---------------------------------------
Putting the rootfs on the microSD card is attractive -- the image becomes a
file you copy on a PC instead of a partition you reflash, and it stops being
bounded by the 8 MB partition. The hardware side is already written and
correct, behind ROOTFS_FROM_SD in main/main.c (currently 0):

  slot 0        the P4 has two slots and SDMMC_HOST_DEFAULT() selects slot 1
  the EV board  CLK 43, CMD 44, D0 39, D1 40, D2 41, D3 42 (also the
  pins          defaults in examples/storage/sd_card/sdmmc)
  LDO chan 4    SOC_SDMMC_IO_POWER_EXTERNAL is 1 on the P4, so SD VDD comes
                from an on-chip LDO and the card is unpowered without it

**The blocker described here previously is gone.** It used to be that
app_main() ran with no scheduler, so the SDMMC and FAT drivers -- which block
on semaphores and wait on ISR dispatch -- never returned, and the board looked
dead right after "SD_HOST: src_freq_hz". Core 0 runs FreeRTOS now and app_main
is a real task, so mounting a card from it works; the flash access that must
stay pre-scheduler is a separate constraint, handled by doing it in
__wrap_esp_startup_start_app() before __real_.

What remains is not a bug but an arithmetic fact, and it is the reason root is
still on flash: **flash is free because it is memory-mapped, and an SD card is
not.** romfs on flash is executed in place -- physmap gives the kernel a
window it can point at, bFLT text is never copied, and `unevictable` is 16 kB.
An SD card is a block device with no address in the memory map, so the image
has to be read into RAM, and NOMMU has no demand paging to soften it. The cost
is the image size, permanently, out of the guest's 31 MB. There is no spare
PSRAM to take it from either: the guest has 31 MB of the 32 MB window and the
last 1 MB holds the DTB.

So, three options, priced:

1. **SD -> PSRAM window at boot** (what ROOTFS_FROM_SD does). Costs the guest
   the image size in RAM -- 4.4 MB today, so memory{} drops from 31 MB to
   about 26. Buys: no reflash, no partition ceiling. This is the option that
   matches "put the romfs on the SD card" literally, and it is the one that
   cannot be free.

2. **SD as delivery, flash as runtime.** The monitor reads rootfs.romfs from
   the card and writes it into the flash partition only when it differs
   (compare size and hash), and the guest still XIPs from flash. Zero RAM
   cost; the card becomes the thing you edit on a PC. Costs a slow boot when
   the file changes, flash wear, and care over *when* the write happens --
   flash ops IPC the other core, which does not run FreeRTOS, so this has to
   happen before core 1 is diverted.

3. **Emulate a block device the guest drives itself.** This is the real prize
   and it is cheaper than it looks: `nommu_virt_defconfig` already sets
   CONFIG_VIRTIO_MMIO=y and CONFIG_VIRTIO_BLK=y, so a virtio-mmio device
   emulated in the monitor's trap handler needs **no additional kernel
   config or patch**. Root stays romfs-on-flash (XIP, free) and the card
   mounts as a writable filesystem, so /home survives a reboot and an editor
   has somewhere to save. RAM cost is page cache, which is reclaimable.
   Work is entirely on the monitor side: virtqueue descriptor rings backed by
   esp_vfs_fat calls.

Note that none of these changes the allocation ceiling above. That is RAM
contiguity, not storage.


The terminal the guest thinks it has
------------------------------------
`BR2_TARGET_GENERIC_GETTY_TERM` is `xterm-256color`, changed from `vt100`,
because that is what is actually on the other end of the wire once you drive
the board with `tools/hypmon.py` from Windows Terminal. It is a truthfulness
fix, not a measured win, and the measurements are worth recording so nobody
spends the afternoon I did:

* **nano cannot show colour here at all.** buildroot does
  `select BR2_PACKAGE_NANO_TINY if !BR2_USE_MMU`, and `--enable-tiny` implies
  `--disable-color`. nano's output is byte-identical under vt100,
  screen-256color and linux -- 676, 686 and 676 bytes, same attribute set,
  reverse video (`ESC[0;7m`) and nothing else. No TERM and no terminfo
  changes that.
* **Both editors parse escape sequences themselves**, so TERM buys little for
  key handling either: nano has its own `parse_escape_sequence` and busybox vi
  does its own decoding, rather than going through terminfo.
* **It does have one real consequence, and it is on the host side.** Under
  `xterm-256color` nano uses the **alternate screen buffer**, and then sets
  its own DECSTBM region -- which throws away the scrolling region
  `tools/hypmon.py` installs to fence guest output out of its status row.
  Verified harmless: nano's deepest absolute cursor row is 24 against a
  24-row pane, because hypmon answers the window-size probe with `rows-1`
  and nano believes 24 *is* the screen. The size answer is what protects the
  status line; hypmon's region is belt-and-braces. Same result for `nctest`,
  which self-reports `xterm-256color` and `24 rows x 100 cols`.

`BR2_PACKAGE_NCURSES_ADDITIONAL_TERMINFO` stays **empty**. buildroot's default
`NCURSES_TERMINFO_FILES` already installs `x/xterm`, `x/xterm-256color`,
`x/xterm+256color`, `x/xterm-color`, `x/xterm-xfree86`, `s/screen`,
`s/screen-256color`, `l/linux`, `v/vt100`, `v/vt220` and more -- verified on
the board. Adding entries for those is not just redundant: the list is
space-separated `dir/name` pairs, and a comma-separated string fails the
ncurses install with

    install: cannot stat '.../terminfo/xterm,xterm-256color,screen,linux,vt220'

which is how this was found out.

A writable /home is still the gap
---------------------------------
/ is romfs mounted ro, so `mkdir /home` at runtime returns EROFS: the mount
point has to exist in the image and fstab needs a writable filesystem over it.
post-build.sh creates the directory and appends

    tmpfs           /home           tmpfs   mode=0755       0       0

which is RAM backed, so nano and vi work but nothing you save survives a
reboot. Option 3 above is what fixes it properly. Until
then, `tar cf - /home | ...` over the serial line is the escape hatch, or keep
the authoritative copy on the build host and rebuild the image.

The rootfs partition size is not free: it must be a power of two
----------------------------------------------------------------
This cost a full build-and-flash cycle to find, and it is invisible until the
guest panics.

The rootfs reaches the guest as an `mtd-rom` device through `physmap`, and
`drivers/mtd/maps/physmap-core.c:521` rounds the window **down** to a power
of two:

```c
info->win_order = get_bitmask_order(resource_size(res)) - 1;
info->maps[i].size = BIT(info->win_order + (info->gpios ? ... : 0));
```

So a 0xC60000 (12.375 MB) partition becomes a `1 << 23` = 8 MB device. The
monitor and the DTB are both innocent -- they report the real span:

```
I: rootfs XIP from flash at 0x40030000 (12672 KB), no PSRAM copy
[    0.150733] physmap-flash 40030000.rom: physmap platform flash device: [mem 0x40030000-0x40c8ffff]
```

...and then:

```
[    0.177802] 1f00            8192 mtdblock0
[    0.178555] Kernel panic - not syncing: VFS: Unable to mount root fs on "mtd0"
```

`romfs_fill_super` refuses an image larger than its device, so a 12.2 MB
image on an 8 MB window does not mount. Confirmed from a booting guest:

```
# cat /proc/mtd
dev:    size   erasesize  name
mtd0: 00800000 00800000 "rootfs"
```

(The 8 MB `erasesize` is unrelated -- `map_rom.c` fabricates one.)

**Consequences.** The rootfs can be 4 MB or 8 MB and nothing in between. 16 MB
is the next step and does not fit beside the app, kernel and dtb on a 16 MB
chip (`esptool flash-id`: manufacturer c8, device 4018, 16MB). Sizing the
partition to "whatever is left over" silently wastes everything above the
previous power of two.

If you need more than 8 MB of guest filesystem, the way to get it is **more
windows** rather than a bigger one: partitions of 4 + 8 + 1 MB give 13 MB of
usable filesystem where one 13 MB partition gives 8.

That was built and booted here -- three `rom@` nodes in the DTS, a
`guest_windows[]` table in main.c mapping each by label and patching its own
placeholder into the DTB, and fstab entries mounting them at `/usr/local` and
`/opt`:

```
physmap-flash 40030000.rom: physmap platform flash device: [mem 0x40030000-0x4042ffff]
physmap-flash 40430000.rom: physmap platform flash device: [mem 0x40430000-0x40c2ffff]
physmap-flash 40c30000.rom: physmap platform flash device: [mem 0x40c30000-0x40d2ffff]
# cat /proc/mtd
mtd0: 00400000 00400000 "rootfs"
mtd1: 00800000 00800000 "tools"
mtd2: 00100000 00100000 "extra"
```

It then came out again. The only thing that needed 13 MB was a native `ld`, and
`ld` turned out not to run on this board at all (next section). What replaced
it fits in the rootfs with 2 MB to spare, so the layout is one 8 MB window
again. `guest_windows[]` is still a table, and the DTS comments say what a
second node needs, because the ceiling has not gone anywhere.

Two things to know if you add one back:

* **Node order in the DTS is the mtd device order.** `of_platform_populate`
  walks the tree in source order, so the first `rom@` node is `mtdblock0`, and
  the kernel command line says `root=mtd0`.
* **Each node needs its own placeholder `reg` pair**, distinct from every
  other, because `patch_dtb_window()` finds each node by searching those eight
  bytes at boot. A duplicate would patch the first node twice and leave the
  second pointing into guest RAM.

A native toolchain in the guest: as, objcopy and 500 lines of Lua
-----------------------------------------------------------------
You can write assembly on the board, assemble it, link it and run it:

```
# cp /usr/share/hello-onboard.S /home && cd /home
# mkflt hello-onboard.S
mkflt.lua: hello-onboard -- 290 bytes (226 code+data, 0 bss zeroed in place), 2 relocs applied, entry 0x0, stack 8192
# ./hello-onboard
Hello from rv32 assembly, assembled and linked
on the ESP32-P4 itself -- as, objcopy and a Lua
linker ran as U-mode processes in a NOMMU Linux
guest, under a hand-written M-mode hypervisor.
```

Getting there took two attempts down blind alleys, and both are worth keeping
because the obvious approach is the one that fails.

### ld is on the board and cannot run

The obvious toolchain is `as` + `ld` + `elf2flt`, and all three were built,
shipped and verified present:

| | bytes |
|---|---|
| `as` | 1,286,816 |
| `ld` | 5,280,148 |
| `objcopy` | 955,868 |
| `elf2flt` | 948,248 |

`ld` does not run. `binfmt_flat` makes ONE physically contiguous allocation per
exec, covering the whole binary, and this guest's allocator has no block big
enough once it has booted:

```
# ld --version
nommu: Allocation of length 5283840 from process 36 (ld) failed
Normal: 19*4kB 8*8kB 12*16kB 11*32kB 11*64kB 11*128kB 8*256kB 3*512kB
        3*1024kB 2*2048kB 3*4096kB 0*8192kB = 25836kB
binfmt_flat: Unable to allocate RAM for process text/data, errno -12
Segmentation fault
```

25 MB free and the largest block is 4 MB, so a 5.04 MB allocation fails --
every time, not intermittently. `echo 3 > /proc/sys/vm/drop_caches` first does
not help; the page cache is 1.7 MB and lives in low orders.

`CONFIG_ARCH_FORCE_MAX_ORDER=11` (see the ceiling section above) was added for
exactly this and is necessary but not sufficient: it gives the allocator an
8 MB order, and an 8 MB block on a 29 MB machine only reforms while nothing
else is churning. `/proc/buddyinfo` shows one free order-11 block moments after
`ld` fails and none at the moment it tries. The patch is kept because an 8 MB
per-exec ceiling instead of 4 MB is worth having for everything else.

`elf2flt` converts `ld`'s output, so it went with `ld`. Both were removed from
the image, which is also why the three-window partition layout described above
was built and then taken out again -- it existed to carry `ld`.

### What replaced them

`/usr/lib/mkflt.lua`, about 500 lines including its own documentation. For a
single freestanding object file, a linker does two things:

1. lay the allocatable sections out end to end and give the symbols addresses,
2. apply the relocations the assembler left behind.

Both are straightforward, and `lua` (300 kB) was already in the image. It calls
`objcopy -O binary --only-section=NAME` for each section's bytes -- `objcopy`
is the right tool for that and it is 956 kB rather than 5.3 MB. `mkflt(1)` runs
`as -mno-relax` and then `mkflt.lua`.

Handled: `PCREL_HI20`/`PCREL_LO12_I`/`PCREL_LO12_S` (so `lla` works),
`HI20`/`LO12_I`/`LO12_S`, `BRANCH`, `JAL`, `CALL`/`CALL_PLT`, `ADD32`/`SUB32`/
`SET32`, and `RELAX`/`NONE` ignored. Anything else is refused **by name** --
the one thing a hand-rolled linker must never do is emit a zero and let the
program jump to it. `/usr/share/stress.S` exercises the list and is the only
regression test the on-board toolchain has.

### Relocation-free assembly is not a thing on rv32

The first design tried to avoid needing a linker at all: `objcopy -O binary` on
an unlinked object, with the source computing addresses itself:

```
1:  auipc a1, 0
    addi  a1, a1, msg - 1b      /* a label difference is a constant */
```

rv32 gas rejects that, and rejects every spelling of it -- inline, hoisted into
`.equ`, wrapped in `%lo()`, with `.balign` removed so no alignment intervenes,
and with relaxation disabled at both assembly and link time:

```
Error: illegal operands `addi a1,a1,msg-1b'
```

Only literal constants are accepted as an immediate. So every address-of in
hand-written assembly leaves a relocation behind, and something has to apply
it. That is what forced the linker.

### A flat binary's data segment is not next to its text

The second blind alley. `mkflt.lua` put code, rodata and data in one blob --
correct, because PC-relative code needs them contiguous -- and declared a
zero-length data segment to carry the relocation table. On the board:

```
binfmt_flat: reloc outside program 0x84000000 (0 - 0xd0/0x8e), killing stress!
```

`0x84000000` is `ntohl()` of the little-endian word `0x00000084`: the program's
own data pointer. The loader had read the relocation table from inside the
text. From `fs/binfmt_flat.c`:

```c
realdatastart = textpos + ntohl(hdr->data_start);
datapos = ALIGN(realdatastart + DATA_START_OFFSET_WORDS * sizeof(u32),
                FLAT_DATA_ALIGN);              /* FLAT_DATA_ALIGN is 0x20 */
reloc = (__be32 __user *)(datapos + (ntohl(hdr->reloc_start) - text_len));
```

The data segment starts at a 32-byte boundary *past* the end of text, bss
follows data, and the relocation table is addressed relative to `datapos`.
None of that is where a single blob lives.

This is the reason elf2flt-built binaries reach their data through a global
offset table and set `FLAT_FLAG_GOTPIC` -- and a GOT is precisely what
hand-written assembly does not have.

The resolution is to use one region and no load-time fixups: code, rodata,
data **and bss** all in the text region, bss materialised as zeroes in the file
rather than declared, `data_start = data_end = bss_end = end of image`, and
`reloc_count = 0`. Everything is contiguous, exactly as the relocations assume.

What it costs: `.word some_symbol` cannot work. A stored absolute address needs
a load-time fixup, and load-time fixups are what was just given up. `mkflt.lua`
refuses those by name and says to use `lla` at run time instead -- which is one
instruction pair, and position-independent, which the stored pointer never was.

### busybox ash exits a `set -e` script when a while condition fails

Found while debugging why `mkflt` produced nothing and said nothing. `sh -x`:

```
+ set -e
+ '[' 1 -gt 0 ']'
+ src=hello-onboard.S
+ shift
+ '[' 0 -gt 0 ']'
<script ends, silently, status 1>
```

Nothing after the loop ran. Under `set -e` this busybox ash treats the failing
controlling command of a `while` as the failure that ends the script. dash does
not. The fix is `while :; do [ $# -gt 0 ] || break; ...`, which puts the test in
a `||` list, where errexit does not apply.

Small flat binaries need -Wl,--no-relax
---------------------------------------
A hand-written assembly hello-world built the obvious way installs cleanly and
then dies:

    hello[35]: unhandled signal 11 code 0x2 at 0x48dfa2dc
    status: 00000080 badaddr: 000007a4 cause: 00000007

cause 7 is a store access fault and badaddr 0x7a4 is a *link-time* offset --
an address never relocated to where the program loaded. The disassembly of
uClibc's `__uClibc_main` in the output says why:

    2f8:  76f02823   sw  a5,1904(zero)   # 770 <__uclibc_progname>

A store to absolute address 0x770 through x0. A flat binary is linked at
address 0, so in a *small* one every address falls inside the +/-2048 range an
x0-relative instruction can reach, and the linker's relaxation pass rewrites
GOT-relative accesses into absolute ones. bFLT has no relocation type that can
fix those up at load time, so they keep pointing at offset-from-zero.

This is why nothing else on the board trips over it: lua is 300 kB and busybox
474 kB, so their data sits far past 2048 and the relaxation is never possible.
It only bites binaries small enough to fit entirely in the low 2 kB.

`-mno-relax` on the compiler is NOT the fix -- it changes which relocations
survive but not this. The link-time pass is what does the damage, so it must be
`-Wl,--no-relax`. post-build.sh passes it when building /usr/bin/hello.

Confirmed on the board afterwards:

    # hello
    Hello from rv32 assembly.
    U-mode process, in a NOMMU Linux guest, in U-mode,
    under a hand-written M-mode hypervisor, on an ESP32-P4.

The other half of the same story is that `-nostdlib` does not work here at all.
ld-elf2flt links with `-q` (--emit-relocs), so every relocation survives into
the executable and elf2flt has to classify each one; its RISC-V case list
(elf2flt.c ~line 828) covers R_RISCV_32/64 and the ADD/SUB/SET family and
nothing else, so PCREL_HI20, PCREL_LO12_I, GPREL_I and JAL all fail with
"unsupported in this context". A normal link does not leave those behind.
Verified by writing the same program in C: with -nostdlib it fails identically,
so it was never an assembly-versus-C question.
