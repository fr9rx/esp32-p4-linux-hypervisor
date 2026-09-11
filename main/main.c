/* ESP32-P4 hypervisor: core 0.
 *
 * This file used to be the entire monitor -- 2,300 lines of instruction decode,
 * three emulated devices, CSR emulation, interrupt injection, trap dispatch,
 * guest loading and core bring-up. It is now just core 0's service loop; the
 * rest lives in the modules hyp.h lists.
 *
 * The division is by core as much as by topic, and that is the useful way to
 * read it: every hyp_*.c except hyp_boot.c runs on core 1 inside a trap
 * handler, where FreeRTOS does not exist and nothing may block. This file runs
 * as an ordinary FreeRTOS task on core 0, so it is where anything needing a
 * scheduler has to live.
 */
#include "hyp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Set by hyp_boot.c's esp_startup_start_app wrap, before the scheduler runs. */
extern bool guest_loaded;


/* Core 0, as a FreeRTOS task.
 *
 * Nothing in here touches flash. What is left is servicing the devices that
 * need the scheduler -- anything that blocks on a semaphore or waits on ISR
 * dispatch has to live on this side, not in core 1's trap handler -- and then
 * watching the guest. */
void app_main(void)
{
    if (!guest_loaded) {
        esp_rom_printf("E: guest was not loaded; core 1 stays parked\n");
        return;
    }

    /* The SD card. Core 0, because ESP-IDF's driver blocks, and not fatal: the
     * guest's root filesystem is romfs out of flash, so a board with no card
     * still boots -- it just has nowhere to write. */
    if (!hyp_sd_init()) {
        esp_rom_printf("W: sd: no card; the guest gets no block device\n");
    } else if (!hyp_virtio_blk_init()) {
        esp_rom_printf("W: virtio-blk: not offered to the guest\n");
    }

    guest_image_ready = true;
    esp_rom_printf("W: Core 0: guest released\n");

    /* ------------------------------------------------------------------
     * The service loop.
     *
     * 10 Hz is plenty now that there is no framebuffer to repaint -- all this
     * loop does is watch the guest. vTaskDelay yields, so the idle task still
     * runs and the task watchdog stays fed.
     *
     * The liveness check reports rather than resets. A hypervisor that does a
     * privilege-dropping mret on every single trap is maximally exposed to the
     * CLIC erratum on this silicon (mcause.mpil latching at 0xff, after which
     * no interrupt can ever be delivered because level > mil is 255 > 255), and
     * the failure mode is a guest that simply stops. Resetting would hide it;
     * printing the trap ring names it.
     * ------------------------------------------------------------------ */
    const TickType_t period = pdMS_TO_TICKS(100);
    uint32_t last_traps = 0u;
    uint32_t quiet_ticks = 0u;
    uint32_t ticks = 0u;
    bool reported = false;

    for (;;) {
        if (hyp_stats_wanted) {
            hyp_stats_wanted = false;
            hyp_stats_dump();
        }

        if (++ticks >= 10u) {                   /* ~1 s */
            ticks = 0u;
            if (hyp_trace_total == last_traps) {
                quiet_ticks++;
            } else {
                quiet_ticks = 0u;
                reported = false;
            }
            last_traps = hyp_trace_total;

            /* 5 s of no traps at all, and not parked in a wfi. The hyp_in_wfi
             * test is what stops this from crying wedge at an idle guest, now
             * that wfi actually blocks. */
            if (quiet_ticks >= 5u && !reported && linux_running && !hyp_in_wfi) {
                reported = true;
                esp_rom_printf("\nW: guest has taken no trap for %u s -- wedged?\n",
                               (unsigned)quiet_ticks);
                hyp_trace_dumps = 0u;
                hyp_trace_dump("liveness");
                hyp_stats_dump();
            }
        }

        vTaskDelay(period);
    }
}
