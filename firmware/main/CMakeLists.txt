# core/    pure C, no ESP-IDF (also built and unit-tested on the PC: make -C host)
# model/   INT8 inference engine + generated model data
# drivers/ hardware: LEDs + servo, SSD1306 OLED (I2C), RC522 RFID (SPI)
# app/     FreeRTOS tasks: UART link, command handling, state, display/control
idf_component_register(
    SRCS "main.c"
         "core/crc16.c" "core/proto.c" "core/gesture_filter.c" "core/system_fsm.c"
         "model/model.c" "model/model_data.c" "model/test_images.c"
         "drivers/outputs.c" "drivers/oled.c" "drivers/font5x7.c" "drivers/rc522.c"
         "app/link.c" "app/commands.c" "app/app_state.c" "app/ui.c"
    INCLUDE_DIRS "." "core" "model" "drivers" "app"
    PRIV_REQUIRES esp_timer heap esp_driver_uart esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_spi)

# warnings are errors in our own code (NFR-MNT-02)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wall -Wextra -Werror)
