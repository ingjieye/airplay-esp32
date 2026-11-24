// Boot-failure counter: start the factory (recovery) app after repeated failed boots.
//
// Every boot increments a counter kept in RTC_CNTL_STORE0 (reserved on ESP32, unused by
// ESP-IDF; it survives software, panic and watchdog resets but is cleared on power-on).
// The application clears it once it has been running for a while (see livelab.c). If
// BOOT_LIMIT boots in a row never get that far, otadata is erased; the bootloader then
// falls back to the factory partition, which holds the WiFi recovery app.
//
// Does nothing on partition tables without a factory partition, so other boards built
// from this repository are unaffected.
#include "bootloader_flash_priv.h"
#include "bootloader_utility.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "soc/reset_reasons.h"
#include "soc/rtc_cntl_reg.h"

#define BOOT_MAGIC 0xB0070000u  // must match livelab.c and recovery/main/main.c
#define BOOT_LIMIT 5

static const char *TAG = "boot_guard";

void bootloader_hooks_include(void) {}

void bootloader_after_init(void)
{
    bootloader_state_t bs = {0};
    if (!bootloader_utility_load_partition_table(&bs) || bs.factory.offset == 0 || bs.ota_info.offset == 0) return;

    uint32_t v = REG_READ(RTC_CNTL_STORE0_REG);
    uint32_t n = (v & 0xFFFF0000u) == BOOT_MAGIC ? (v & 0xFFFFu) : 0;
    soc_reset_reason_t r = esp_rom_get_reset_reason(0);
    if (r == RESET_REASON_CHIP_POWER_ON || r == RESET_REASON_SYS_BROWN_OUT) n = 0;
    n++;
    if (n >= BOOT_LIMIT) {
        ESP_LOGW(TAG, "%lu boots in a row without a healthy run: erasing otadata to start the recovery app",
                 (unsigned long)n);
        bootloader_flash_erase_range(bs.ota_info.offset, bs.ota_info.size);
        n = 0;
    } else {
        ESP_LOGI(TAG, "boot %lu of %d without a healthy run", (unsigned long)n, BOOT_LIMIT);
    }
    REG_WRITE(RTC_CNTL_STORE0_REG, BOOT_MAGIC | n);
}
