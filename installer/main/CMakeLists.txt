idf_component_register(SRCS "main.c" "usblink.c" "sdstate.c" "sdupdate.c"
                       INCLUDE_DIRS "."
                       REQUIRES fm1proto fwcheck fm1install driver usb app_update)
