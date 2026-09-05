# A stand-in for the `usb` component that ESP-IDF v6.1-beta1 no longer ships.
#
# The USB host stack was removed from IDF core in the 6.x componentisation and
# has not been republished on the component registry -- components/usb is gone,
# usb_host.h does not exist anywhere in the install, and the in-tree
# examples/peripherals/usb/host/* still reference a `usb_host_hid` that cannot
# resolve. So USB host on this IDF means bringing your own stack.
#
# The stack here is CherryUSB (a managed dependency of main), which has a DWC2
# host driver and an Espressif glue layer. That glue needs exactly one thing
# from IDF's old `usb` component -- esp_private/usb_phy.h, i.e. usb_new_phy()
# and friends -- plus soc/usb_dwc_periph.h, which went with it.
#
# Those two files are vendored here verbatim from ESP-IDF v5.5.1 rather than
# rewritten, because they are the authority on the P4's USB pad and PHY wiring
# and there is nothing to gain from paraphrasing them. Everything they depend on
# (usb_wrap_hal, usb_utmi_hal, usb_serial_jtag_hal, usb_dwc_struct) is still
# present in 6.1 under esp_hal_usb, which is why 461 lines is the whole port
# rather than the ~530 KB of components/usb.
idf_component_register(SRCS "usb_phy.c" "usb_dwc_periph.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_hal_usb esp_hw_support esp_driver_gpio
                                     esp_system soc hal esp_rom log esp_common)
