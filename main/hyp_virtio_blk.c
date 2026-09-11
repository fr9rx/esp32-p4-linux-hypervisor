/* A virtio-mmio block device for the guest, backed by the microSD card.
 *
 * WHY VIRTIO AND NOT A DEVICE OF OUR OWN
 *
 * Because the guest already has the driver. Its kernel is built with
 * CONFIG_VIRTIO_MMIO=y and CONFIG_VIRTIO_BLK=y, so presenting a virtio-mmio
 * device gets a working /dev/vda with no guest kernel patch, no out-of-tree
 * driver and no maintenance -- the whole cost is on this side of the trap.
 * buildroot/README.md called this "the real prize" and it was right.
 *
 * The alternative was an ad-hoc register interface plus a Linux driver to
 * match, which is more code in two places and a patch to carry forever.
 *
 * LEGACY (VERSION 1), NOT MODERN
 *
 * The modern layout splits the ring into three separately-addressed areas
 * (QueueDescLow/High, QueueDriverLow/High, QueueDeviceLow/High). Legacy uses a
 * single QueuePFN and derives the three from it, which is less state here and
 * less to get wrong. Linux's virtio_mmio driver supports both and picks by the
 * Version register, so this is purely our choice.
 *
 * HOW A REQUEST CROSSES THE CORES
 *
 * The guest runs on core 1, and so does every MMIO trap. The SD card can only
 * be touched from core 0, because ESP-IDF's driver blocks on semaphores and ISR
 * dispatch that do not exist in a trap handler. So:
 *
 *   core 1   guest writes QueueNotify -> hyp_virtio_blk_write() sets s_kick
 *   core 0   the service task sees s_kick, walks the ring, does the SD I/O,
 *            writes the used ring, then sets s_irq
 *   core 1   hyp_poll_host_devices() sees s_irq, sets InterruptStatus and
 *            raises the PLIC line; the guest takes an interrupt
 *   core 1   guest writes InterruptACK -> the line drops
 *
 * The descriptors hold guest physical addresses, which are host physical
 * addresses here -- the guest's RAM is PSRAM at 0x48000000, mapped identically
 * for both cores -- so core 0 can follow them directly with no translation.
 */
#include "hyp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

/* --- virtio-mmio register offsets (legacy) ------------------------------- */
#define VIRTIO_MMIO_MAGIC_VALUE          0x000
#define VIRTIO_MMIO_VERSION              0x004
#define VIRTIO_MMIO_DEVICE_ID            0x008
#define VIRTIO_MMIO_VENDOR_ID            0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES      0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL  0x014
#define VIRTIO_MMIO_DRIVER_FEATURES      0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL  0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE      0x028
#define VIRTIO_MMIO_QUEUE_SEL            0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX        0x034
#define VIRTIO_MMIO_QUEUE_NUM            0x038
#define VIRTIO_MMIO_QUEUE_ALIGN          0x03c
#define VIRTIO_MMIO_QUEUE_PFN            0x040
#define VIRTIO_MMIO_QUEUE_NOTIFY         0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS     0x060
#define VIRTIO_MMIO_INTERRUPT_ACK        0x064
#define VIRTIO_MMIO_STATUS               0x070
#define VIRTIO_MMIO_CONFIG               0x100

#define VIRTIO_MMIO_MAGIC                0x74726976u   /* "virt" */
#define VIRTIO_ID_BLOCK                  2u
#define HYP_VIRTIO_VENDOR                0x50345052u   /* "P4PR" -- ours */

/* Queue size. 128 descriptors is what a legacy ring of this size costs in
 * guest memory (about 6 KB) and is far more than a NOMMU guest with a 32 MB
 * page cache will ever have in flight. */
#define HYP_VQ_SIZE                      128u

/* --- the split virtqueue, as the guest lays it out ----------------------- */
#define VRING_DESC_F_NEXT   1u
#define VRING_DESC_F_WRITE  2u

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
};

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
};

/* --- virtio-blk request format ------------------------------------------- */
#define VIRTIO_BLK_T_IN       0u
#define VIRTIO_BLK_T_OUT      1u
#define VIRTIO_BLK_T_FLUSH    4u
#define VIRTIO_BLK_T_GET_ID   8u

#define VIRTIO_BLK_S_OK       0u
#define VIRTIO_BLK_S_IOERR    1u
#define VIRTIO_BLK_S_UNSUPP   2u

struct virtio_blk_outhdr {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
};

#define HYP_BLK_SECTOR_SIZE   512u

/* --- device state -------------------------------------------------------- */
static struct {
    uint32_t status;                /* the guest's driver status byte     */
    uint32_t device_features_sel;
    uint32_t driver_features;
    uint32_t guest_page_size;
    uint32_t queue_sel;
    uint32_t queue_num;
    uint32_t queue_align;
    uint32_t queue_pfn;
    uint32_t interrupt_status;
    uint16_t last_avail;            /* how far we have consumed the avail ring */
    bool     live;                  /* ring is set up and usable          */
} blk;

/* Set by core 1 when the guest kicks the queue; cleared by core 0. */
static volatile bool s_kick;
/* Set by core 0 when it has published used-ring entries; cleared by core 1. */
static volatile bool s_irq;

/* Counters, reported by the statistics dump. */
static uint32_t s_reads, s_writes, s_errors, s_requests;

/* Bounce buffer.
 *
 * The guest's buffers are in PSRAM at arbitrary alignment; ESP-IDF's SD driver
 * wants DMA-capable, aligned memory and checks. Staging through internal RAM is
 * one memcpy per transfer and removes the whole question. 32 KB = 64 sectors,
 * which comfortably covers the largest segment Linux will send. */
#define HYP_BLK_BOUNCE_SECTORS  64u
static uint8_t *s_bounce;


/* Guest physical address -> host pointer. The identity, but written out so the
 * one assumption this file makes about the address space is visible. */
static inline void *gpa(uint64_t addr)
{
    return (void *)(uintptr_t)(uint32_t)addr;
}

static struct vring_desc *desc_table(void)
{
    return (struct vring_desc *)(uintptr_t)(blk.queue_pfn * blk.guest_page_size);
}

static struct vring_avail *avail_ring(void)
{
    return (struct vring_avail *)((uint8_t *)desc_table() +
                                  blk.queue_num * sizeof(struct vring_desc));
}

static struct vring_used *used_ring(void)
{
    /* Legacy layout: desc table, then the avail ring, then the used ring
     * aligned up to QueueAlign. */
    uintptr_t after_avail = (uintptr_t)avail_ring() +
                            sizeof(struct vring_avail) +
                            blk.queue_num * sizeof(uint16_t) +
                            sizeof(uint16_t);   /* used_event */
    uintptr_t align = blk.queue_align ? blk.queue_align : 4096u;
    return (struct vring_used *)((after_avail + align - 1u) & ~(align - 1u));
}


/* ===========================================================================
 * Core 0: request processing
 * ===========================================================================
 */

/* Move `len` bytes between a guest buffer and the card, in bounce-sized
 * chunks. Returns false on any I/O error. */
static bool blk_transfer(bool write, uint32_t sector, uint8_t *guest_buf,
                         uint32_t len)
{
    const uint32_t chunk_max = HYP_BLK_BOUNCE_SECTORS * HYP_BLK_SECTOR_SIZE;

    while (len > 0u) {
        uint32_t n = (len > chunk_max) ? chunk_max : len;
        uint32_t sectors = n / HYP_BLK_SECTOR_SIZE;

        if (write) {
            memcpy(s_bounce, guest_buf, n);
            if (!hyp_sd_write(sector, sectors, s_bounce)) {
                return false;
            }
        } else {
            if (!hyp_sd_read(sector, sectors, s_bounce)) {
                return false;
            }
            memcpy(guest_buf, s_bounce, n);
        }

        guest_buf += n;
        sector += sectors;
        len -= n;
    }
    return true;
}

/* Handle one descriptor chain. Returns the number of bytes the device wrote
 * into guest buffers, which is what the used ring reports. */
static uint32_t blk_do_request(uint16_t head)
{
    struct vring_desc *desc = desc_table();

    /* Descriptor 0 is always the request header. */
    struct vring_desc *d = &desc[head % blk.queue_num];
    if (d->len < sizeof(struct virtio_blk_outhdr)) {
        s_errors++;
        return 0u;
    }
    struct virtio_blk_outhdr *hdr = gpa(d->addr);
    uint32_t type = hdr->type;
    uint32_t sector = (uint32_t)hdr->sector;

    /* Walk to collect the data descriptors, keeping the last one aside: for
     * virtio-blk it is always the one-byte status. */
    uint16_t idx = d->next;
    bool has_next = (d->flags & VRING_DESC_F_NEXT) != 0u;

    uint8_t *status_byte = null;
    uint32_t written = 0u;
    bool ok = true;

    while (has_next) {
        d = &desc[idx % blk.queue_num];
        bool next = (d->flags & VRING_DESC_F_NEXT) != 0u;

        if (!next) {
            /* Last link: the status byte. */
            status_byte = gpa(d->addr);
            break;
        }

        uint8_t *buf = gpa(d->addr);
        uint32_t len = d->len;

        switch (type) {
        case VIRTIO_BLK_T_IN:
            if ((len % HYP_BLK_SECTOR_SIZE) != 0u) {
                ok = false;
                break;
            }
            ok = blk_transfer(false, sector, buf, len);
            sector += len / HYP_BLK_SECTOR_SIZE;
            written += len;
            s_reads++;
            break;

        case VIRTIO_BLK_T_OUT:
            if ((len % HYP_BLK_SECTOR_SIZE) != 0u) {
                ok = false;
                break;
            }
            ok = blk_transfer(true, sector, buf, len);
            sector += len / HYP_BLK_SECTOR_SIZE;
            s_writes++;
            break;

        case VIRTIO_BLK_T_GET_ID: {
            /* A 20-byte serial the guest exposes in sysfs. Purely cosmetic,
             * but Linux asks and an unanswered request stalls the probe. */
            static const char id[] = "esp32p4-sd";
            uint32_t n = (len < sizeof(id)) ? len : sizeof(id);
            memset(buf, 0, len);
            memcpy(buf, id, n);
            written += len;
            break;
        }

        case VIRTIO_BLK_T_FLUSH:
            /* Writes go straight to the card and sdmmc_write_sectors() does
             * not return until the card has them, so there is nothing held
             * back to flush. Answering OK is honest here, not a shortcut. */
            break;

        default:
            ok = false;
            break;
        }

        if (!ok) {
            break;
        }
        idx = d->next;
        has_next = true;
    }

    if (status_byte != null) {
        *status_byte = ok ? VIRTIO_BLK_S_OK
                          : (type > VIRTIO_BLK_T_GET_ID ? VIRTIO_BLK_S_UNSUPP
                                                        : VIRTIO_BLK_S_IOERR);
        written += 1u;
    }
    if (!ok) {
        s_errors++;
    }
    s_requests++;
    return written;
}

/* Drain the avail ring. Core 0 only. */
static void blk_service(void)
{
    if (!blk.live) {
        return;
    }

    struct vring_avail *avail = avail_ring();
    struct vring_used *used = used_ring();
    bool published = false;

    while (blk.last_avail != avail->idx) {
        uint16_t head = avail->ring[blk.last_avail % blk.queue_num];
        uint32_t written = blk_do_request(head);

        used->ring[used->idx % blk.queue_num].id = head;
        used->ring[used->idx % blk.queue_num].len = written;

        /* The guest must not see the index move before the entry it points
         * at. Same reason a real device orders its writes. */
        __sync_synchronize();
        used->idx++;
        blk.last_avail++;
        published = true;
    }

    if (published) {
        /* And core 1 must not raise the interrupt before the ring is visible. */
        __sync_synchronize();
        s_irq = true;
    }
}

/* The service task.
 *
 * Priority 0, pinned to core 0, and busy-polling with taskYIELD(). That
 * combination is deliberate: at priority 0 it round-robins with the idle task,
 * so the task watchdog still gets fed, while a yield-based loop responds in
 * microseconds. The obvious alternatives are both worse -- vTaskDelay(1) is
 * 10 ms at CONFIG_FREERTOS_HZ=100, which would make every block request
 * glacial, and giving a semaphore from core 1's trap handler would mean
 * calling FreeRTOS from a core that is not running it. */
static void blk_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_kick) {
            s_kick = false;
            blk_service();
        }
        taskYIELD();
    }
}


/* ===========================================================================
 * Core 1: the MMIO registers
 * ===========================================================================
 */

uint32_t hyp_virtio_blk_read(uint32_t offset, uint32_t width)
{
    if (offset >= VIRTIO_MMIO_CONFIG) {
        /* Config space, served a byte at a time from a little-endian image.
         *
         * IT MUST BE BYTE-ADDRESSABLE, and that is not obvious. A legacy
         * virtio-mmio device's config space is read by vm_get() in
         * drivers/virtio/virtio_mmio.c, which for version 1 does
         *
         *     for (i = 0; i < len; i++)
         *             ptr[i] = readb(base + offset + i);
         *
         * -- eight separate one-byte reads for the 64-bit capacity, not one
         * 32-bit read per half. Answering only offsets 0 and 4 therefore
         * reported the lowest byte of the capacity and zero for the other
         * seven, and since this card's sector count is 0x0E840000 -- whose low
         * byte is 0 -- the guest read a capacity of exactly zero and printed
         *
         *     virtio_blk virtio0: [vda] 0 512-byte logical blocks (0 B/0 B)
         *
         * which looks like a broken card rather than a broken accessor.
         *
         * virtio_blk_config begins with the capacity in 512-byte sectors; with
         * no feature bits offered, nothing past it is read. */
        uint8_t cfg[16] = { 0 };
        uint64_t capacity = hyp_sd_sector_count();
        for (uint32_t i = 0u; i < 8u; i++) {
            cfg[i] = (uint8_t)(capacity >> (8u * i));
        }

        uint32_t o = offset - VIRTIO_MMIO_CONFIG;
        uint32_t value = 0u;
        for (uint32_t i = 0u; i < width && (o + i) < sizeof(cfg); i++) {
            value |= (uint32_t)cfg[o + i] << (8u * i);
        }
        return value;
    }

    switch (offset) {
    case VIRTIO_MMIO_MAGIC_VALUE:     return VIRTIO_MMIO_MAGIC;
    case VIRTIO_MMIO_VERSION:         return 1u;          /* legacy */
    case VIRTIO_MMIO_DEVICE_ID:       return VIRTIO_ID_BLOCK;
    case VIRTIO_MMIO_VENDOR_ID:       return HYP_VIRTIO_VENDOR;

    /* No feature bits at all.
     *
     * Everything virtio-blk defines is optional, and declining all of it gets
     * the simplest correct device: 512-byte sectors, capacity from config
     * space, writable, and -- because VIRTIO_BLK_F_FLUSH is absent -- no flush
     * requests to answer. The guest fills in its own defaults. */
    case VIRTIO_MMIO_DEVICE_FEATURES: return 0u;

    case VIRTIO_MMIO_QUEUE_NUM_MAX:   return HYP_VQ_SIZE;
    case VIRTIO_MMIO_QUEUE_PFN:       return blk.queue_pfn;
    case VIRTIO_MMIO_INTERRUPT_STATUS: return blk.interrupt_status;
    case VIRTIO_MMIO_STATUS:          return blk.status;
    default:                          return 0u;
    }
}

void hyp_virtio_blk_write(uint32_t offset, uint32_t value, uint32_t width)
{
    (void)width;

    switch (offset) {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL: blk.device_features_sel = value; break;
    case VIRTIO_MMIO_DRIVER_FEATURES:     blk.driver_features = value; break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL: break;
    case VIRTIO_MMIO_GUEST_PAGE_SIZE:     blk.guest_page_size = value; break;
    case VIRTIO_MMIO_QUEUE_SEL:           blk.queue_sel = value; break;
    case VIRTIO_MMIO_QUEUE_NUM:           blk.queue_num = value; break;
    case VIRTIO_MMIO_QUEUE_ALIGN:         blk.queue_align = value; break;

    case VIRTIO_MMIO_QUEUE_PFN:
        blk.queue_pfn = value;
        /* A zero PFN tears the queue down; anything else brings it up. The
         * ring is only usable once the guest has told us its size, its
         * alignment and its page size, so all four are checked together. */
        blk.live = (value != 0u) && (blk.queue_num != 0u) &&
                   (blk.guest_page_size != 0u) && hyp_sd_ready();
        blk.last_avail = 0u;
        break;

    case VIRTIO_MMIO_QUEUE_NOTIFY:
        /* All this may do is record the kick. The SD card cannot be touched
         * from here -- see the file header. */
        s_kick = true;
        break;

    case VIRTIO_MMIO_INTERRUPT_ACK:
        blk.interrupt_status &= ~value;
        if (blk.interrupt_status == 0u) {
            plic_set_pending(PLIC_VIRTIO_BLK_SOURCE, false);
        }
        break;

    case VIRTIO_MMIO_STATUS:
        blk.status = value;
        if (value == 0u) {              /* driver reset */
            blk.live = false;
            blk.interrupt_status = 0u;
            blk.last_avail = 0u;
            plic_set_pending(PLIC_VIRTIO_BLK_SOURCE, false);
        }
        break;

    default:
        break;
    }
}

/* Called from hyp_poll_host_devices() on core 1: turn core 0's completion into
 * a guest interrupt. Split this way because plic_set_pending() and the guest's
 * interrupt state belong to core 1, and core 0 must not touch them. */
void hyp_virtio_blk_poll(void)
{
    if (s_irq) {
        s_irq = false;
        blk.interrupt_status |= 1u;     /* bit 0: used ring updated */
        plic_set_pending(PLIC_VIRTIO_BLK_SOURCE, true);
    }
}

void hyp_virtio_blk_stats(void)
{
    if (s_requests == 0u) {
        return;
    }
    esp_rom_printf("  virtio-blk: %u requests, %u reads, %u writes, %u errors\n",
                   (unsigned)s_requests, (unsigned)s_reads,
                   (unsigned)s_writes, (unsigned)s_errors);
}

/* Core 0, after the card is up. Allocates the bounce buffer and starts the
 * service task. Returns false if the guest should not be offered a device. */
bool hyp_virtio_blk_init(void)
{
    if (!hyp_sd_ready()) {
        return false;
    }

    s_bounce = heap_caps_malloc(HYP_BLK_BOUNCE_SECTORS * HYP_BLK_SECTOR_SIZE,
                                MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_bounce == null) {
        esp_rom_printf("E: virtio-blk: no DMA memory for the bounce buffer\n");
        return false;
    }

    /* Pinned to core 0: it calls into the SD driver, which must not run on
     * core 1. Priority 0 so the idle task still gets scheduled -- see
     * blk_task(). */
    if (xTaskCreatePinnedToCore(blk_task, "virtio-blk", 4096, null, 0, null, 0)
            != pdPASS) {
        esp_rom_printf("E: virtio-blk: could not start the service task\n");
        return false;
    }

    esp_rom_printf("I: virtio-blk at 0x%08x, %u sectors, /dev/vda in the guest\n",
                   (unsigned)HYP_VIRTIO_BLK_BASE,
                   (unsigned)hyp_sd_sector_count());
    return true;
}
