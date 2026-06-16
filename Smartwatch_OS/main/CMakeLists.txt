# File: Smartwatch_OS/main/CMakeLists.txt
idf_component_register(SRCS "main.cpp" "port_axp2101.cpp" "ui_app.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES XPowersLib waveshare__esp32_s3_touch_amoled_2_06 lvgl)