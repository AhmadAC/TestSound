// File: Smartwatch_OS/main/file_explorer.h
#ifndef FILE_EXPLORER_H
#define FILE_EXPLORER_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

bool mount_sd_card(void);
void start_file_explorer(void);
void close_file_explorer(void);
void stop_file_explorer_media(void);

#endif // FILE_EXPLORER_H