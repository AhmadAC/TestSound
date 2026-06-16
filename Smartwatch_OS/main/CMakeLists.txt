# File: Smartwatch_OS/main/CMakeLists.txt
idf_component_register(SRCS "main.c" "ui_app.c" "file_explorer.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver lvgl bsp esp_lcd esp_vfs_fat sdmmc)