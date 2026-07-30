idf_component_register(
    SRCS
        "main.c"
        "matrix.c"
        "hid_core.c"
       
        "ble_transport.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_driver_gpio
        bt
        nvs_flash
)