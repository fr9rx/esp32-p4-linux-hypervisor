/* Guest loading, PMP and core-1 bring-up
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_cpu.h"
#include "riscv/interrupt.h"
#include "riscv/rv_utils.h"
#include "soc/soc.h"
#include "soc/interrupts.h"
#include "soc/clic_reg.h"
#include "esp_private/interrupt_clic.h"
#include "hal/cache_ll.h"
#include "hal/systimer_ll.h"
#include "hal/uart_ll.h"

#define pmp_entery 5

uint32_t linux_base_address = 0x48000000;
uint32_t linux_size = 0x02000000;      /* 32 MB: all populated PSRAM, 0x48000000-0x4A000000 */
uint32_t dtb_address = 0x49F00000;     /* top 1 MB: PMP-granted, outside the memory{} node */

/* The guest's root filesystem -- executed in place, out of flash.
 *
 * It used to be memcpy'd out of the "rootfs" partition into a 2 MB PSRAM window
 * at 0x49D00000, which cost 2 MB of the guest's RAM forever and capped the
 * image at the size of that window. It is now left where it is: the partition
 * stays esp_partition_mmap()'d for the lifetime of the board, and the DTB's
 * mtd-rom node is patched to point at the mapping instead.
 *
 * Three things make that work, and all three are properties of this chip rather
 * than choices:
 *
 *   - the mapping lands in the flash cache window, 0x40000000-0x44000000, and
 *     PMP entry 6 covers exactly that window with R+X and the L bit
 *     (cpu_region_protect.c, the rev < v3 path). PMP permissions apply to
 *     U-mode, so the guest can already read and execute there. No new entry --
 *     which matters, because only entries 5 and 7-10/12-14 are free and 6
 *     outranks all of the latter.
 *   - physmap-core's ioremap is the identity on a NOMMU kernel, so an mtd-rom
 *     node whose reg names the mapping resolves to plain loads.
 *   - romfs_get_unmapped_area -> mtd_get_unmapped_area gives bFLT text pages
 *     straight out of that window, so program text is now XIP from flash and
 *     never occupies RAM at all.
 *
 * What it costs: flash is slower than PSRAM, so exec and demand-read of program
 * text is slower. What it buys: 2 MB of guest RAM back, no size cap below the
 * partition size, and the window at 0x49D00000 freed for the framebuffer.
 *
 * This is a table rather than a single mapping because there can be more than
 * one window, and the reason is a constraint rather than an ambition:
 * physmap-core.c:521 rounds an mtd-rom window DOWN to a power of two, so one
 * guest filesystem can be 4 MB or 8 MB and nothing between, and 16 MB does not
 * fit beside the app and kernel on a 16 MB chip. Several small windows are the
 * only way past that ceiling.
 *
 * A three-window layout (rootfs 4 + tools 8 + extra 1 = 13 MB) was built and
 * booted here, to carry a native ld. ld does not run on this board -- see
 * buildroot/README.md -- and what replaced it fits in 8 MB, so there is one
 * entry again. The table stays because the constraint has not gone anywhere.
 *
 * Each window needs its own placeholder reg pair in the DTS, and each pair must
 * appear exactly once in the blob. */
struct guest_window {
    const char *label;              /* partition label, and linux,mtd-name */
    const char *mountpoint;         /* where the guest's fstab puts it; doc only */
    uint8_t     placeholder[8];     /* the DTS reg pair, big-endian <addr size> */
    const void *mapping;            /* filled in by window_map_from_flash() */
    uint32_t    span;
};

/* ORDER MATTERS, and it must match dts/p4-phase6.dts. of_platform_populate
 * walks the tree in source order, so the guest sees these as mtdblock0/1/2 in
 * this order, and the kernel command line says root=mtd0. This array does not
 * control that -- the DTS does -- but keeping the two in the same order is the
 * only way the comments stay honest. */
static struct guest_window guest_windows[] = {
    { "rootfs", "/", { 0x49, 0xD0, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00 }, null, 0u },
};
#define GUEST_WINDOW_COUNT (sizeof(guest_windows) / sizeof(guest_windows[0]))

bool linux_running = false;

extern int hyp_vector_table;
static uint32_t hyp_stack[2048] __attribute__((aligned(16)));

/* Core 1's own stack, for the short stretch between being diverted out of
 * FreeRTOS and mret-ing into the guest.
 *
 * Separate from hyp_stack (which hyp_trap_entry swaps in from mscratch) and
 * separate from the IDF startup stack it would otherwise still be using, and
 * the second point is the one that matters: core 0's main_task calls
 * heap_caps_enable_nonos_stack_heaps(), which hands both cores' startup stacks
 * to the heap. Core 1 has to be off its startup stack before it says it is
 * done, or an allocation on core 0 can land inside core 1's live frame. */
static uint32_t hyp_boot_stack[512] __attribute__((aligned(16)));

/* Core 0 copies the kernel; core 1 must not mret into a half-copied image. */
volatile bool guest_image_ready = false;

/* ===========================================================================
 * Host bring-up (runs on core 1, where the guest lives)
 * ===========================================================================
 */

/* Rip out everything IDF armed on this core.
 *
 * Not optional hygiene. esp_cache_err_int_init() routed ETS_CACHE_INTR_SOURCE
 * to ETS_CACHEERR_INUM and enabled it, and IDF's _mtvt_table entry for it is
 * _panic_handler -- which builds its frame on the interrupted stack. With a
 * U-mode guest owning sp, that faults inside the trap prologue and nests until
 * the watchdog fires. Exactly the death hyp_vectors.S exists to avoid.
 *
 * The CLIC control block is per-core behind a current/other aliasing scheme
 * (soc/interrupt_reg.h), so this MUST run on the core it is meant to affect. */
static void hyp_clic_sanitize(uint32_t core_id)
{
    for (int src = 0; src < ETS_MAX_INTR_SOURCE; src++) {
        esp_rom_route_intr_matrix(core_id, src, ETS_INVALID_INUM);
    }
    for (int i = 0; i < 32; i++) {
        ESP_INTR_DISABLE(i);
        esprv_int_set_vectored(i, false);
    }
}

static void hyp_setup_interrupts(uint32_t core_id)
{
    hyp_clic_sanitize(core_id);

    /* Threshold, explicitly. On this silicon (rev < v3, INTTHRESH_STANDARD == 0)
     * it is the memory-mapped CLIC_INT_THRESH_REG rather than the mintthresh
     * CSR, which is what this helper picks for us. Only the FreeRTOS port ever
     * writes it, and FreeRTOS never starts, so it is sitting at its reset value
     * -- and the comparison is inclusive, so a non-zero reset value would mask
     * our lines and nothing would ever fire. */
    rv_utils_restore_intlevel_regval(RVHAL_INTR_ENABLE_THRESH_CLIC);

    /* SYSTIMER counter 0 is shared with esp_timer, which already started it.
     * Enable it anyway -- idempotent, and a stopped counter presents only as
     * "the guest's clock never advances". */
    systimer_ll_enable_clock(&SYSTIMER, true);
    systimer_ll_enable_counter(&SYSTIMER, HYP_SYSTIMER_COUNTER, true);
    systimer_ll_counter_can_stall_by_cpu(&SYSTIMER, HYP_SYSTIMER_COUNTER, 0, false);
    systimer_ll_counter_can_stall_by_cpu(&SYSTIMER, HYP_SYSTIMER_COUNTER, 1, false);
    systimer_ll_enable_alarm(&SYSTIMER, HYP_SYSTIMER_ALARM, false);
    systimer_ll_clear_alarm_int(&SYSTIMER, HYP_SYSTIMER_ALARM);

    /* Take ownership of UART0 from the ROM. The ROM's uart_rx_intr_handler is
     * already unrouted by IDF's core_intr_matrix_clear, but its enable bits,
     * its latched status, and any bytes typed during boot are all still live. */
    uart_dev_t *hw = UART_LL_GET_HW(0);
    uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
    uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
    while (uart_ll_get_rxfifo_len(hw) > 0u) {
        uint8_t discard;
        uart_ll_read_rxfifo(hw, &discard, 1);
    }
    uart_ll_set_rxfifo_full_thr(hw, 1);     /* one keystroke -> one interrupt */
    uart_ll_set_rx_tout(hw, 10);            /* ~11 us at 921600 */

    /* Priority must be set explicitly. CLIC_INT_CTL resets to 0x1f and with
     * NLBITS == 3 that is level 0; the threshold comparison is inclusive, so
     * threshold 0 masks level 0 and a line left at its reset priority never
     * fires. esp_intr_alloc always sets priority, which is why IDF never trips
     * on this and we would. */
    esprv_int_set_type(HYP_INUM_TIMER, INTR_TYPE_LEVEL);
    esprv_int_set_priority(HYP_INUM_TIMER, HYP_INT_PRIO);
    esprv_int_set_vectored(HYP_INUM_TIMER, false);   /* shv=0 -> lands in mtvec */

    esprv_int_set_type(HYP_INUM_UART, INTR_TYPE_LEVEL);
    esprv_int_set_priority(HYP_INUM_UART, HYP_INT_PRIO);
    esprv_int_set_vectored(HYP_INUM_UART, false);

    esp_rom_route_intr_matrix(core_id, ETS_SYSTIMER_TARGET1_INTR_SOURCE, HYP_INUM_TIMER);
    esp_rom_route_intr_matrix(core_id, ETS_UART0_INTR_SOURCE, HYP_INUM_UART);

    ESP_INTR_ENABLE(HYP_INUM_TIMER);
    ESP_INTR_ENABLE(HYP_INUM_UART);

    uart_ll_ena_intr_mask(hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT |
                              UART_INTR_RXFIFO_OVF);

    /* mstatus.MIE is deliberately left clear. The guest runs in U-mode, and
     * M-mode interrupts are always globally enabled when the current privilege
     * is below M -- so this costs nothing and keeps our own setup code
     * uninterruptible. */
}

static inline void linux_start(void)
{
    asm volatile ("csrc mstatus, %0" :: "r"(3u << 11));
    asm volatile ("csrw mepc, %0" :: "r"(linux_base_address));
    asm volatile ("li a0, 0");
    asm volatile ("mv a1, %0" :: "r"(dtb_address) : "a1");
    asm volatile ("mret");
}

static esp_err_t pmp_set_rwx_region(uint32_t base, uint32_t size)
{
    if (size < 8u || (size & (size - 1u)) != 0u || (base & (size - 1u)) != 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (PMP_ENTRY_CFG_READ(pmp_entery) & PMP_L) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t addr = base | ((size - 1u) >> 1);
    const uint8_t cfg = PMP_NAPOT | PMP_R | PMP_W | PMP_X;

    PMP_RESET_AND_ENTRY_SET(pmp_entery, addr, cfg);

    if (PMP_ENTRY_CFG_READ(pmp_entery) != cfg) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Copy a partition into PSRAM. `limit` caps the copy: it exists from when the
 * rootfs was copied into a 2 MB window smaller than its partition, and without
 * the cap the memcpy ran straight through the DTB sitting above it. Nothing
 * passes a non-zero limit now -- the filesystems are mapped, not copied -- but
 * the kernel and DTB still come through here. Pass 0 for no cap. */
static esp_err_t partition_cp_to_psram(const char *label, uint32_t destination,
                                       uint32_t limit)
{
    esp_err_t err_code;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
                                                        label);
    if (p == null) {
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t span = p->size;
    if (limit != 0u && span > limit) {
        span = limit;
    }

    const void* src;
    esp_partition_mmap_handle_t h;
    err_code = esp_partition_mmap(p, 0, span, ESP_PARTITION_MMAP_DATA, &src, &h);
    if (err_code != ESP_OK) {
        return err_code;
    }
    memcpy((void *)destination, src, span);
    esp_partition_munmap(h);
    return ESP_OK;
}

/* Map one filesystem partition into the flash cache window and leave it mapped.
 *
 * No munmap: the guest reads this for its whole uptime. Dropping the handle
 * would unmap the pages and the guest's next read would fault somewhere far
 * away from here.
 *
 * Three of these are mapped at boot and they are not free of each other: they
 * share the flash MMU, so 13 MB of windows consumes 13 MB of it. That is well
 * inside SOC_DROM_LOW..SOC_DROM_HIGH on this part, and if it ever stops being
 * so, the bounds check below is what says so rather than the guest. */
static esp_err_t window_map_from_flash(struct guest_window *w)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                       ESP_PARTITION_SUBTYPE_DATA_UNDEFINED,
                                                       w->label);
    if (p == null) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_partition_mmap_handle_t h;
    esp_err_t err = esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA,
                                       &w->mapping, &h);
    if (err != ESP_OK) {
        return err;
    }
    w->span = p->size;

    /* Sanity: a mapping outside the flash cache window would not be covered by
     * PMP entry 6, and the guest's first read would take an access fault that
     * looks like a filesystem bug. Say so here instead. */
    uint32_t va = (uint32_t)w->mapping;
    if (va < SOC_DROM_LOW || va + w->span > SOC_DROM_HIGH) {
        esp_rom_printf("E: %s mapped at 0x%08x, outside the PMP-granted "
                       "flash window 0x%08x-0x%08x\n",
                       w->label, (unsigned)va,
                       (unsigned)SOC_DROM_LOW, (unsigned)SOC_DROM_HIGH);
        return ESP_ERR_INVALID_STATE;
    }

    /* A partition whose size is not a power of two is a silent loss of up to
     * half of it: physmap-core.c:521 computes win_order from the size and then
     * uses BIT(win_order), so a 12 MB window becomes an 8 MB device and an
     * image that fits the partition does not fit the device. The guest reports
     * that as "Unable to mount root fs", which points nowhere near
     * partitions.csv, so it is worth a line here. */
    if ((w->span & (w->span - 1u)) != 0u) {
        esp_rom_printf("W: %s is %u KB, not a power of two -- the guest sees "
                       "only %u KB of it (physmap rounds down)\n",
                       w->label, (unsigned)(w->span / 1024u),
                       (unsigned)((1u << (31 - __builtin_clz(w->span))) / 1024u));
    }
    return ESP_OK;
}

/* Point one mtd-rom node in the DTB at wherever its mapping landed.
 *
 * esp_partition_mmap picks a free MMU slot, so the address is not knowable at
 * build time and the DTS carries a placeholder. Patched by pattern rather than
 * by walking the FDT: reg is one big-endian <address size> pair, and each
 * node's placeholder pair appears exactly once in the blob, so finding those
 * eight bytes is both simpler and self-checking -- a DTS edit that changes a
 * placeholder makes this fail loudly instead of writing to the wrong offset.
 *
 * With three nodes the "exactly once" property is doing more work than it was
 * with one, because a duplicated placeholder would now silently patch the
 * FIRST node twice and leave the second pointing at a stale address. The three
 * pairs in guest_windows[] are deliberately different from each other; the
 * count is checked after compiling the DTS, not here.
 *
 * The scan restarts from the top of the blob for each window. Three passes
 * over 2 kB at boot is not worth optimising. */
static bool patch_dtb_window(const uint8_t needle[8], uint32_t address,
                             uint32_t span)
{
    uint8_t *dtb = (uint8_t *)dtb_address;

    /* The blob's own totalsize, big-endian at offset 4. Bounded by the 1 MB
     * window in case the partition was never written. */
    uint32_t total = ((uint32_t)dtb[4] << 24) | ((uint32_t)dtb[5] << 16) |
                     ((uint32_t)dtb[6] << 8)  |  (uint32_t)dtb[7];
    if (total < 8u || total > 0x00100000u) {
        return false;
    }

    for (uint32_t i = 0u; i + 8u <= total; i++) {
        if (memcmp(dtb + i, needle, 8) != 0) {
            continue;
        }
        dtb[i + 0] = (uint8_t)(address >> 24);
        dtb[i + 1] = (uint8_t)(address >> 16);
        dtb[i + 2] = (uint8_t)(address >> 8);
        dtb[i + 3] = (uint8_t)(address);
        dtb[i + 4] = (uint8_t)(span >> 24);
        dtb[i + 5] = (uint8_t)(span >> 16);
        dtb[i + 6] = (uint8_t)(span >> 8);
        dtb[i + 7] = (uint8_t)(span);
        return true;
    }
    return false;
}

/* Rewrite the kernel's built-in command line in place.
 *
 * CONFIG_CMDLINE_FORCE=y, so the DTB's bootargs is ignored -- but the built-in
 * string is plain data inside the image we just copied into RWX PSRAM. The
 * field is 73 chars plus 3 NUL, so a replacement of up to 75 fits.
 *
 * This buys three things: root=/dev/vda goes away (the embedded initramfs makes
 * it inert anyway, and this stops the kernel from trying), and keep_bootcon
 * holds earlycon open through the ttyS0 handover -- without it, any defect in
 * the 8250 emulation is indistinguishable from a hang.
 *
 * Searched rather than hardcoded at 0x48247128: that offset is specific to this
 * kernel build. */
static bool patch_guest_cmdline(uint32_t span)
{
    static const char needle[] = "root=/dev/vda rw earlycon=";
    static const char replacement[] =
        "earlycon=uart8250,mmio,0x10000000 console=ttyS0 keep_bootcon";

    const size_t needle_len = sizeof(needle) - 1u;
    char *base = (char *)linux_base_address;

    for (uint32_t i = 0u; i + needle_len < span; i++) {
        if (memcmp(base + i, needle, needle_len) != 0) {
            continue;
        }
        size_t original = strlen(base + i);
        if (sizeof(replacement) - 1u > original) {
            return false;                     /* would overrun the field */
        }
        memset(base + i, 0, original);
        memcpy(base + i, replacement, sizeof(replacement) - 1u);
        return true;
    }
    return false;
}

/* Everything that touches flash, done before FreeRTOS starts.
 *
 * The split is forced by spi_flash's cross-core handshake, and finding that out
 * cost a boot: a flash operation on core 0 normally IPCs core 1 into
 * spi_flash_op_block_func() so that neither core is fetching from a disabled
 * cache. Core 1 does not run FreeRTOS here, so that IPC is never serviced and
 * core 0 spins forever inside
 * spi_flash_disable_interrupts_caches_and_other_cpu() -- which presents as the
 * task watchdog firing on IDLE0 about ten milliseconds into app_main(), with
 * the register dump pointing at cache_utils.c and nothing pointing at the
 * guest.
 *
 * cache_utils.c takes a different path when xTaskGetSchedulerState() is
 * taskSCHEDULER_NOT_STARTED: no IPC, on the assumption that the other core is
 * still spinning in startup code. That is exactly the state here, so all of the
 * loading happens in that window and nothing after it touches flash again.
 *
 * Returns false if the guest cannot be assembled; the caller still has to boot
 * core 0, because a board with an empty kernel partition should come up far
 * enough to say so. */
static bool hyp_load_guest(void)
{
    esp_err_t result = partition_cp_to_psram("kernel", linux_base_address, 0u);
    if (result != ESP_OK) {
        esp_rom_printf("E: kernel: %s\n", esp_err_to_name(result));
        return false;
    }
    esp_rom_printf("I: Copied Linux Image to PSRAM\n");

    result = partition_cp_to_psram("dtb", dtb_address, 0u);
    if (result != ESP_OK) {
        esp_rom_printf("E: dtb: %s\n", esp_err_to_name(result));
        return false;
    }
    esp_rom_printf("I: Copied DTB to PSRAM\n");

    /* The guest filesystems, left in flash.
     *
     * A MISSING partition is not fatal: a kernel with a built-in initramfs
     * needs no rootfs at all, the tools and extra windows are optional by
     * design, and the same firmware has to boot a board flashed with an older
     * partition table. Each absent one costs a warning and its mount.
     *
     * What IS fatal is mapping one and then failing to tell the guest where it
     * went. A guest that reads the stale placeholder address fails somewhere in
     * the filesystem layer, a long way from the cause, so that case stops the
     * boot here where the message is still readable. */
    for (unsigned i = 0u; i < GUEST_WINDOW_COUNT; i++) {
        struct guest_window *w = &guest_windows[i];

        result = window_map_from_flash(w);
        if (result != ESP_OK) {
            esp_rom_printf("W: %s: %s (no %s mount)\n",
                           w->label, esp_err_to_name(result), w->mountpoint);
            continue;
        }
        if (!patch_dtb_window(w->placeholder, (uint32_t)w->mapping, w->span)) {
            esp_rom_printf("E: %s mapped at 0x%08x but its DTB placeholder was "
                           "not found -- guest would read the wrong address\n",
                           w->label, (unsigned)(uint32_t)w->mapping);
            return false;
        }
        esp_rom_printf("I: %s XIP from flash at 0x%08x (%u KB) -> %s\n",
                       w->label, (unsigned)(uint32_t)w->mapping,
                       (unsigned)(w->span / 1024u), w->mountpoint);
    }

    /* Only the older initramfs kernel needs this -- the romfs kernel sets its
     * own CONFIG_CMDLINE, so the needle is absent and the patch is a no-op. */
    if (patch_guest_cmdline(0x00400000u)) {
        esp_rom_printf("I: Patched guest command line\n");
    } else {
        esp_rom_printf("I: Guest command line left as built\n");
    }
    return true;
}

/* Read by app_main() in main.c: a board with an empty kernel partition
 * should still boot core 0 far enough to say so. */
bool guest_loaded = false;

/* Load the guest, then hand over to IDF's own core-0 startup.
 *
 * The wrap exists only to get in before vTaskStartScheduler(); everything after
 * it is stock. __real_ is called rather than reimplemented, so main_task,
 * app_main and the task watchdog all behave exactly as they normally would. */
extern void __real_esp_startup_start_app(void);

void __wrap_esp_startup_start_app(void)
{
    guest_loaded = hyp_load_guest();
    __real_esp_startup_start_app();
}


/* Core 1. The guest's core: PMP, trap vector, host interrupts, then mret.
 *
 * PMP, mtvec and mscratch are all per-hart, so all three are set up here rather
 * than inherited from core 0. IDF's call_start_cpu1() has already run
 * bootloader_init_mem() on this core, so entries 0-4/6/11/15 arrive locked
 * exactly as they are on core 0. Free entries on rev 1.0 are 5 and 7-10/12-14;
 * 5 is the one used, and it has to be 5 rather than any of the others because
 * entry 6 covers the flash window and PMP resolves by lowest matching index.
 *
 * Entered from __wrap_xPortStartScheduler() rather than by overriding
 * esp_startup_start_app_other_cores(), and the difference cost a boot to find.
 *
 * The obvious hook is that weak symbol in startup.c. It cannot be used here:
 * freertos/app_startup.c ALSO defines it, strongly, so overriding it collides
 * at link time as soon as that object is in the link -- and it is, because core
 * 0 now boots into FreeRTOS normally. A --wrap on it does not help either,
 * because --wrap only redirects UNDEFINED references and startup.c both defines
 * the symbol weakly and calls it in the same object. The board boots, core 0
 * comes all the way up, the panel initialises -- and core 1 quietly runs
 * FreeRTOS instead of the guest, with nothing printed to say so.
 *
 * xPortStartScheduler() is wrappable: it lives in the FreeRTOS port and is
 * called from app_startup.c and tasks.c, never from its own object. Both cores
 * pass through the wrap, so it dispatches on the core id -- core 0 goes on into
 * the real scheduler, core 1 comes here and never returns. Everything IDF does
 * to bring core 1 up before that point (crosscore interrupt, the INT WDT tick
 * hook) is left in place and then undone by hyp_clic_sanitize(). */
static void hyp_guest_core_entry(void)
{
    /* Tell core 0 this core has finished starting up.
     *
     * main_task() blocks in wait_for_all_cores_ready() until every core has set
     * its flag, and the real xPortStartScheduler() is what normally sets it --
     * which this core no longer reaches. Without this the two cores deadlock
     * waiting for each other: core 0 inside wait_for_all_cores_ready(), core 1
     * on guest_image_ready just below, and the only symptom is a boot that
     * stops after "main_task: Started on CPU0".
     *
     * Safe to claim here only because __wrap_xPortStartScheduler() has already
     * moved this core onto hyp_boot_stack -- see the note there. */
    extern volatile unsigned port_uxCoreStartupDone[CONFIG_FREERTOS_NUMBER_OF_CORES];
    port_uxCoreStartupDone[rv_utils_get_core_id()] = 1u;

    while (!guest_image_ready) { }

    /* Keep our own bring-up uninterruptible. IDF's call_start_cpu1() left this
     * core with a live cache-error interrupt pointing into _mtvt_table, and
     * mtvec is about to become ours -- taking anything before hyp_clic_sanitize
     * has run would land in hypervisor() with linux_running still false. */
    RV_CLEAR_CSR(mstatus, MSTATUS_MIE);

    uint32_t core_id = rv_utils_get_core_id();

    esp_err_t result = pmp_set_rwx_region(linux_base_address, linux_size);
    if (result != ESP_OK) {
        esp_rom_printf("E: pmp: %s\n", esp_err_to_name(result));
        while (1) { }
    }
    esp_rom_printf("I: Core %u: PMP RWX on guest window\n", (unsigned)core_id);

    /* A stack the guest cannot reach. hyp_trap_entry swaps it in from mscratch
     * before touching memory, because the guest owns sp and Linux's reset_regs
     * sets it to 0. */
    RV_WRITE_CSR(mscratch, (uint32_t)&hyp_stack[sizeof(hyp_stack) / sizeof(hyp_stack[0])]);
    esp_cpu_intr_set_ivt_addr(&hyp_vector_table);

    hyp_setup_interrupts(core_id);
    esp_rom_printf("I: Core %u: interrupts armed (timer=%u uart=%u)\n",
                   (unsigned)core_id, HYP_INUM_TIMER, HYP_INUM_UART);

    linux_running = true;
    esp_rom_printf("W: Starting Linux on core %u\n", (unsigned)core_id);
    linux_start();

    while (1) { }
}

/* Divert core 1 into the guest.
 *
 * Called on both cores: core 0 from vTaskStartScheduler(), core 1 from
 * esp_startup_start_app_other_cores(). Only core 1 is taken. */
BaseType_t __real_xPortStartScheduler(void);

BaseType_t __wrap_xPortStartScheduler(void)
{
    if (rv_utils_get_core_id() != 0u) {
        /* Move onto our own stack before doing anything else, then jump. Two
         * instructions in one asm block because both operands have to be in
         * registers before sp changes, and because there is no returning from
         * this: the guest owns sp after the mret at the end. */
        asm volatile ("mv sp, %0\n	"
                      "jalr %1"
                      :: "r"(&hyp_boot_stack[sizeof(hyp_boot_stack) / sizeof(hyp_boot_stack[0])]),
                         "r"(&hyp_guest_core_entry)
                      : "memory");
        __builtin_unreachable();
    }
    return __real_xPortStartScheduler();
}
