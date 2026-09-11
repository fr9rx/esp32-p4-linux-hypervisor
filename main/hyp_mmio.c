/* MMIO device table and the load/store decoder
 *
 * Split out of main.c, which had grown to 2,300 lines. hyp.h carries the
 * contract between the modules and the map of which file does what.
 */
#include "hyp.h"

/* ===========================================================================
 * MMIO dispatch
 *
 * The device table is the reason this is split out. Before, hypervisor_mmio
 * bounds-checked the UART by name and called uart_read/uart_write directly, so
 * every new device meant editing the middle of the load/store decoder. Now a
 * device is one row here and the decoder never changes.
 * ===========================================================================
 */


const MmioDevice mmio_devices[] = {
    { HYP_CLINT_BASE, HYP_CLINT_SIZE, clint_read, clint_write, "clint" },
    { HYP_UART_BASE,  HYP_UART_SIZE,  uart_read,  uart_write,  "uart"  },
    { HYP_PLIC_BASE,  HYP_PLIC_SIZE,  plic_read,  plic_write,  "plic"  },
    { HYP_VIRTIO_BLK_BASE, HYP_VIRTIO_BLK_SIZE,
      hyp_virtio_blk_read, hyp_virtio_blk_write, "vblk" },
};

/* Published so hyp_trace.c can name the devices in the statistics dump
 * without keeping a second, drifting copy of the list. */
const uint32_t mmio_device_count =
        sizeof(mmio_devices) / sizeof(mmio_devices[0]);

_Static_assert(sizeof(mmio_devices) / sizeof(mmio_devices[0])
                       <= HYP_MMIO_MAX_DEVICES,
               "more MMIO devices than hyp_stats.mmio[] can count");

const MmioDevice *mmio_find(uint32_t address)
{
    for (uint32_t i = 0u; i < mmio_device_count; i++) {
        const MmioDevice *device = &mmio_devices[i];
        if (address >= device->base && address < device->base + device->size) {
            return device;
        }
    }
    return null;
}

TrapResult hypervisor_mmio(GuestTrap *trap)
{
    /* mtval is the faulting address on causes 5 and 7. */
    const MmioDevice *device = mmio_find(trap->tval);
    if (device == null) {
        return TRAP_UNHANDLED;    /* a genuine bad access, not a device window */
    }
    if (!trap_decode(trap)) {
        return TRAP_UNHANDLED;
    }

    uint32_t instruction = trap->encoding;

    /* Loads are opcode 0x03 and stores 0x23: the same seven bits apart from
     * bit 5. Masking with 0x5f accepts both and rejects everything else. */
    if ((instruction & 0x5fu) != 0x03u) {
        return TRAP_UNHANDLED;
    }

    hyp_stats.mmio[(uint32_t)(device - mmio_devices) % HYP_MMIO_MAX_DEVICES]++;

    uint32_t offset = trap->tval - device->base;
    uint32_t rd  = (instruction >>  7) & 0x1fu;   /* loads:  destination  */
    uint32_t rs2 = (instruction >> 20) & 0x1fu;   /* stores: value source */
    uint32_t src = reg_read(trap->frame, rs2);

    /* Dense key: funct3 in bits 0-2, opcode bit 5 (load vs store) in bit 3.
     * Verified against the assembler: lb/lh/lw/lbu/lhu -> 0,1,2,4,5 and
     * sb/sh/sw -> 8,9,a. */
    switch (((instruction >> 12) & 0x7u) | ((instruction >> 2) & 0x8u)) {
    /* Loads write rd. The sign-extending casts are the whole difference between
     * lb/lh and lbu/lhu -- note that readb() compiles to lb, not lbu, so the
     * signed forms are not theoretical. */
    case 0x0u: reg_write(trap->frame, rd, (uint32_t)(int32_t)(int8_t) device->read(offset, 1u)); break;  /* lb  */
    case 0x1u: reg_write(trap->frame, rd, (uint32_t)(int32_t)(int16_t)device->read(offset, 2u)); break;  /* lh  */
    case 0x2u: reg_write(trap->frame, rd,                             device->read(offset, 4u)); break;  /* lw  */
    case 0x4u: reg_write(trap->frame, rd,                             device->read(offset, 1u)); break;  /* lbu */
    case 0x5u: reg_write(trap->frame, rd,                             device->read(offset, 2u)); break;  /* lhu */

    /* Stores have no rd, so they simply do not write one. */
    case 0x8u: device->write(offset, src, 1u); break;   /* sb */
    case 0x9u: device->write(offset, src, 2u); break;   /* sh */
    case 0xau: device->write(offset, src, 4u); break;   /* sw */

    default:   return TRAP_UNHANDLED;
    }

    return TRAP_ADVANCE;
}


