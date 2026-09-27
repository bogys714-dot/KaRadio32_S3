# Vendored espressif/esp-usb "usb_host_msc" component (USB Host MSC class
# driver: bulk-only transport + SCSI + FATFS glue). Normally fetched via the
# ESP-IDF Component Manager, but vendored here directly (same approach used
# by the yoRadio project) so the build has no dependency on network access
# to the component registry.
#
# Requires the ESP-IDF built-in "usb" component (USB Host Library,
# usb_host_install()/usb_host_client_*) and "fatfs" (FAT filesystem +
# diskio glue), both of which are already part of a normal ESP-IDF v5.x
# install targeting ESP32-S3 (confirmed by CONFIG_USB_HOST_* options
# already present in this project's sdkconfig).

idf_component_register(
    SRCS
        "src/msc_host.c"
        "src/msc_host_vfs.c"
        "src/diskio_usb.c"
        "src/msc_scsi_bot.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
        "include/esp_private"
    PRIV_REQUIRES
        usb
        fatfs
        soc
)

component_compile_options(-Wno-error=unused-but-set-variable -Wno-error=unused-variable)
