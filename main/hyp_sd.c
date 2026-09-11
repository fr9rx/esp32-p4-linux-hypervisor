/* The microSD card, as raw sectors.
 *
 * This is the backing store for the guest's virtio block device. It deals in
 * 512-byte sectors and knows nothing about filesystems: the guest puts whatever
 * it likes on the card and this file moves blocks.
 *
 * WHY ESP-IDF'S DRIVER AND NOT LINUX'S
 *
 * The native (non-hypervisor) line of this project spent a night on Linux's
 * dw_mmc driver and could not get a reliable filesystem out of it -- raw block
 * reads and writes were fine at any size, but the scattered multi-block
 * transfers a filesystem generates hung the controller, reproducibly, and the
 * watchdog then reset the board. ESP-IDF's driver drives the same silicon and
 * works. Borrowing it is the whole point of having a hypervisor here.
 *
 * WHY THIS RUNS ON CORE 0
 *
 * Every function below blocks: sdmmc_card_init() waits on the card's own
 * timings, and sdmmc_read_sectors() waits on a transfer to complete through
 * semaphores and ISR dispatch. None of that can happen inside core 1's trap
 * handler, which has no FreeRTOS at all -- an earlier attempt at an SD rootfs
 * in this project died exactly there, and the CMakeLists still carries the
 * warning. So core 1 records that the guest kicked the queue and core 0 does
 * the I/O; hyp_virtio_blk.c owns that handshake.
 */
#include "hyp.h"

#include "esp_err.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

/* The slot the board wires its microSD socket to, and the LDO channel that
 * powers it.
 *
 * Slot 0 with the dedicated IOMUX pads -- clk 43, cmd 44, d0-d3 39-42, which is
 * what SDMMC_SLOT_CONFIG_DEFAULT() already gives on this target. The datasheet
 * labels those pads SD1_*, which reads like slot 1; they are physically slot 0.
 *
 * SOC_SDMMC_IO_POWER_EXTERNAL is set on the P4, so the slot's VDD does not come
 * from a fixed rail -- it comes from on-chip LDO channel 4, and the driver will
 * not talk to a card until something turns that on. sd_pwr_ctrl_new_on_chip_ldo
 * is ESP-IDF's own way to do it, so the LDO is handed to the host rather than
 * poked directly. */
#define HYP_SD_SLOT          SDMMC_HOST_SLOT_0
#define HYP_SD_LDO_CHANNEL   4

/* 4-bit, and the default 20 MHz.
 *
 * Not because faster is impossible -- the host supports high speed -- but
 * because the guest's throughput here is bounded by the virtqueue round trip
 * across two cores, not by the wire. Raise it once that is no longer true. */
#define HYP_SD_BUS_WIDTH     4

static sdmmc_card_t s_card;
static bool s_ready;

bool hyp_sd_ready(void)
{
    return s_ready;
}

uint32_t hyp_sd_sector_count(void)
{
    return s_ready ? s_card.csd.capacity : 0u;
}

bool hyp_sd_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = HYP_SD_SLOT;

    /* Power first: without the LDO the slot is unpowered and card init simply
     * times out, which looks exactly like an empty socket. */
    sd_pwr_ctrl_ldo_config_t ldo_config = { .ldo_chan_id = HYP_SD_LDO_CHANNEL };
    sd_pwr_ctrl_handle_t pwr = NULL;
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr);
    if (err != ESP_OK) {
        esp_rom_printf("E: sd: LDO ch%d: %s\n", HYP_SD_LDO_CHANNEL,
                       esp_err_to_name(err));
        return false;
    }
    host.pwr_ctrl_handle = pwr;

    err = host.init();
    if (err != ESP_OK) {
        esp_rom_printf("E: sd: host init: %s\n", esp_err_to_name(err));
        return false;
    }

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = HYP_SD_BUS_WIDTH;
    /* No card-detect or write-protect line is wired on this board, and the
     * defaults already say GPIO_NUM_NC for both. Left explicit because a stray
     * cd pin makes the driver report an empty slot forever. */
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;

    err = sdmmc_host_init_slot(HYP_SD_SLOT, &slot);
    if (err != ESP_OK) {
        esp_rom_printf("E: sd: slot init: %s\n", esp_err_to_name(err));
        return false;
    }

    err = sdmmc_card_init(&host, &s_card);
    if (err != ESP_OK) {
        esp_rom_printf("E: sd: no card (%s)\n", esp_err_to_name(err));
        return false;
    }

    s_ready = true;

    /* Sector count rather than bytes: the guest's virtio config space is in
     * 512-byte units and this is the number that ends up there. */
    esp_rom_printf("I: sd: %s %uMB, %u sectors of %u bytes, bus %ubit %ukHz\n",
                   s_card.is_sdio ? "SDIO" : (s_card.is_mmc ? "MMC" : "SD"),
                   (unsigned)(((uint64_t)s_card.csd.capacity *
                               s_card.csd.sector_size) >> 20),
                   (unsigned)s_card.csd.capacity,
                   (unsigned)s_card.csd.sector_size,
                   (unsigned)s_card.log_bus_width,
                   (unsigned)(s_card.max_freq_khz));
    return true;
}

/* Read/write `count` sectors at `sector`.
 *
 * The buffer must be word-aligned and in memory the SDMMC DMA can reach.
 * hyp_virtio_blk.c stages every transfer through an internal-RAM bounce buffer
 * for that reason: the guest's own buffers are in PSRAM, and while the DMA can
 * reach PSRAM, the descriptors the guest hands us are neither aligned nor
 * necessarily contiguous. */
bool hyp_sd_read(uint32_t sector, uint32_t count, void *dst)
{
    if (!s_ready) {
        return false;
    }
    esp_err_t err = sdmmc_read_sectors(&s_card, dst, sector, count);
    if (err != ESP_OK) {
        esp_rom_printf("E: sd: read %u+%u: %s\n", (unsigned)sector,
                       (unsigned)count, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool hyp_sd_write(uint32_t sector, uint32_t count, const void *src)
{
    if (!s_ready) {
        return false;
    }
    esp_err_t err = sdmmc_write_sectors(&s_card, src, sector, count);
    if (err != ESP_OK) {
        esp_rom_printf("E: sd: write %u+%u: %s\n", (unsigned)sector,
                       (unsigned)count, esp_err_to_name(err));
        return false;
    }
    return true;
}
