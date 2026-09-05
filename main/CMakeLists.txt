idf_component_register(SRCS "main.c" "hyp_vectors.S" "hyp_kbd.c"
                    REQUIRES esp_psram esp_partition esp_mm
                             esp_hal_systimer esp_hal_uart
                             esp_hw_support cherryusb
                    INCLUDE_DIRS "."
                    LDFRAGMENTS "linker.lf")

# NOTE: esp_startup_start_app() is NOT wrapped any more.
#
# It used to be, so that app_main() ran with no FreeRTOS at all. That kept the
# monitor minimal but made core 0 useless: every IDF driver that blocks on a
# semaphore or waits on ISR dispatch hangs the boot with no error, which is
# what killed the SD-card rootfs (see sd_cp_to_psram() history) and would kill
# the DSI panel the same way.
#
# Now core 0 boots normally -- full FreeRTOS, app_main() as a task -- and only
# core 1 is taken over, through the weak esp_startup_start_app_other_cores()
# overridden in main.c. That split is safe: startup.c sets s_system_full_inited
# and calls esp_startup_start_app() on core 0 *before* core 1 is released, and
# core 0 never waits for core 1's scheduler.
#
# Two things had to move because core 0 now runs FreeRTOS:
#   - the guest's host timer, from SYSTIMER alarm 0 to alarm 1 (alarm 0 is
#     SYSTIMER_ALARM_OS_TICK_CORE0 and FreeRTOS reprograms it)
#   - ESP_INT_WDT_CHECK_CPU1 and ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1, both off:
#     core 1 has no tick and no idle task, so both watchdogs would fire.

# Take over core 1, and only core 1.
#
# xPortStartScheduler() is the hook because it is the only one that actually
# works here. startup.c declares esp_startup_start_app_other_cores() weak but
# freertos/app_startup.c defines it strongly, so overriding the weak symbol
# collides at link time; and --wrap on that symbol is silently ineffective,
# because --wrap only redirects undefined references and startup.c both defines
# it weakly and calls it in the same object.
#
# xPortStartScheduler() lives in the FreeRTOS port and is only ever called from
# other objects, so the wrap takes. Both cores reach it; main.c dispatches on
# the core id.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=xPortStartScheduler")

# Get in before vTaskStartScheduler() to load the guest out of flash.
#
# Not an aesthetic choice: spi_flash IPCs the other core into an IRAM spin for
# the duration of any flash operation, and core 1 is not running FreeRTOS to
# service that IPC -- so a flash read from a task deadlocks core 0 inside
# spi_flash_disable_interrupts_caches_and_other_cpu(). cache_utils.c skips the
# IPC entirely while the scheduler has not started, which is the window this
# wrap creates. __real_ is called afterwards, so core 0 startup is otherwise
# completely stock.
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=esp_startup_start_app")
