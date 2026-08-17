#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "riscv/csr.h"
#include "esp_partition.h"

#define  pmp_entery 5
#define null ((void*)0)
uint32_t linux_base_address = 0x48000000;
uint32_t linux_size = 0x01000000;
uint32_t dtb_address = 0x48F00000;

typedef struct {
    uint32_t reg[32];
    uint32_t pc;
} linux_registers_t;

static inline void linux_start(void)
{
    asm volatile ("csrc mstatus, %0" :: "r"(3u << 11));
    asm volatile ("csrw mepc, %0" :: "r"(linux_base_address));
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

static esp_err_t linux_cp_to_psram(void)
{
    esp_err_t err_code;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, "kernel");
    if (p == null) {
        return ESP_ERR_NO_MEM;
    }

    const void* src;
    esp_partition_mmap_handle_t h;
    err_code = esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &src, &h);
    if (err_code != ESP_OK) {
        return err_code;
    }
    memcpy((void *)linux_base_address, src, p->size);
    esp_partition_munmap(h);
    return ESP_OK;
}

static esp_err_t linux_dtb_cp_to_psram(void) {
    esp_err_t err_code;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, "dtb");
    if (p == null) {
        return ESP_ERR_NO_MEM;
    }

    const void* src;
    esp_partition_mmap_handle_t h;
    err_code = esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &src, &h);
    if (err_code != ESP_OK) {
        return err_code;
    }
    memcpy((void *)dtb_address, src, p->size);
    esp_partition_munmap(h);
    return ESP_OK;
}

void app_main(void)
{
    // 1. Adding the Linux Memory Address range to PMP RWX Rgion
    esp_err_t result = pmp_set_rwx_region(linux_base_address, linux_size);
    if (result != ESP_OK) {
        printf("E: %s\n", esp_err_to_name(result));
        return;
    }
    printf("I: Set PMP RWX on Linux Memory Address Range\n");

    // 2. Copying the Linux Image to PSRAM
    result = linux_cp_to_psram();
    if (result != ESP_OK) {
        printf("E: %s\n", esp_err_to_name(result));
        return;
    }
    printf("I: Copied Linux Image to PSRAM");

    // 3. Copying the DTB to PSRAM
    result = linux_dtb_cp_to_psram();
    if (result != ESP_OK) {
        printf("E: %s\n", esp_err_to_name(result));
        return;
    }
    printf("I: Copied DTB to PSRAM");
}
