# File: Smartwatch_OS/main/CMakeLists.txt
idf_component_register(SRCS "main.c" "ui_app.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver bsp)