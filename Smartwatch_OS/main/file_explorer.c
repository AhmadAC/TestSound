// File: Smartwatch_OS/main/file_explorer.c
#include "file_explorer.h"
#include "camera_recv.h"
#include "ui_app.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "FILE_EXPLORER";

static sdmmc_card_t *card = NULL;

static lv_obj_t * explorer_container = NULL;
static lv_obj_t * file_list = NULL;
static lv_obj_t * text_viewer = NULL;
static lv_obj_t * img_viewer = NULL;

static char current_dir[256] = "/sdcard";

static void refresh_directory_list(const char *path);
static void file_click_cb(lv_event_t *e);

static volatile bool mjpeg_playing = false;
static TaskHandle_t mjpeg_task_handle = NULL;
static lv_obj_t *canvas_img = NULL;
static uint8_t *mjpeg_canvas_buf = NULL;

bool mount_sd_card(void) {
    if (card != NULL) return true;

    ESP_LOGI(TAG, "[SD Mount] Initializing SPI bus and mounting card filesystem...\n"
                  "           * MOSI -> GPIO 1\n"
                  "           * MISO -> GPIO 3\n"
                  "           * SCK  -> GPIO 2\n"
                  "           * CS   -> GPIO 17");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = GPIO_NUM_1,
        .miso_io_num = GPIO_NUM_3,
        .sclk_io_num = GPIO_NUM_2,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[SD Mount] Failed to initialize SPI bus.");
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = GPIO_NUM_17;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[SD Mount] Failed to mount filesystem.");
        spi_bus_free(host.slot);
        card = NULL;
        return false;
    }

    ESP_LOGI(TAG, "[SD Mount] SD Card mounted successfully onto virtual path /sdcard.");
    return true;
}

static void btn_delete_cb(lv_event_t *e) {
    void *user_data = lv_event_get_user_data(e);
    if (user_data) {
        const char *str = (const char *)user_data;
        if (strcmp(str, "..") != 0) {
            free(user_data);
        }
    }
}

static void close_text_viewer_cb(lv_event_t *e) {
    if (text_viewer) {
        lv_obj_delete(text_viewer);
        text_viewer = NULL;
    }
}

static void view_text_file(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return;

    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t bytes_read = fread(buf, 1, 4095, f);
    buf[bytes_read] = '\0';
    fclose(f);

    text_viewer = lv_obj_create(explorer_container);
    lv_obj_set_size(text_viewer, 410, 502);
    lv_obj_set_style_bg_color(text_viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(text_viewer, LV_OPA_COVER, 0);
    lv_obj_center(text_viewer);
    lv_obj_move_foreground(text_viewer);

    lv_obj_t *ta = lv_textarea_create(text_viewer);
    lv_obj_set_size(ta, 390, 420);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 10);
    lv_textarea_set_text(ta, buf);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(ta, lv_color_black(), 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);

    lv_obj_t *btn_close = lv_button_create(text_viewer);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Close");
    lv_obj_center(lbl_close);
    lv_obj_add_event_cb(btn_close, close_text_viewer_cb, LV_EVENT_CLICKED, NULL);

    free(buf);
}

static void close_img_viewer_cb(lv_event_t *e) {
    mjpeg_playing = false;
    canvas_img = NULL;
    if (img_viewer) {
        lv_obj_delete(img_viewer);
        img_viewer = NULL;
    }
}

static void mjpeg_play_task(void *arg) {
    char *filepath = (char *)arg;
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        free(filepath);
        mjpeg_playing = false;
        mjpeg_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    size_t temp_buf_size = 64 * 1024;
    uint8_t *frame_buf = heap_caps_malloc(temp_buf_size, MALLOC_CAP_SPIRAM);
    uint8_t *work_buf = heap_caps_malloc(3100, MALLOC_CAP_SPIRAM);

    if (frame_buf && work_buf) {
        while (mjpeg_playing) {
            int c;
            // Find SOI marker (0xFFD8)
            while ((c = fgetc(f)) != EOF) {
                if (c == 0xFF) {
                    int next = fgetc(f);
                    if (next == 0xD8) {
                        break;
                    }
                }
            }
            if (feof(f)) {
                fseek(f, 0, SEEK_SET);
                continue;
            }

            uint32_t frame_len = 2;
            frame_buf[0] = 0xFF;
            frame_buf[1] = 0xD8;

            bool found_eoi = false;
            while (frame_len < temp_buf_size - 1 && mjpeg_playing) {
                int byte = fgetc(f);
                if (byte == EOF) break;
                frame_buf[frame_len++] = byte;
                if (byte == 0xD9 && frame_buf[frame_len - 2] == 0xFF) {
                    found_eoi = true;
                    break;
                }
            }

            if (found_eoi && mjpeg_playing && canvas_img != NULL) {
                JDEC jd;
                jpeg_decode_t dec = {
                    .data = frame_buf,
                    .len = frame_len,
                    .offset = 0,
                    .out_buf = (uint16_t *)mjpeg_canvas_buf,
                    .out_width = CAM_WIDTH
                };

                if (jd_prepare(&jd, jpg_input_func, work_buf, 3100, &dec) == JDR_OK) {
                    jd_decomp(&jd, jpg_output_func, 0);
                    if (mjpeg_playing && canvas_img != NULL) {
                        if (bsp_display_lock(10)) {
                            if (canvas_img != NULL) lv_obj_invalidate(canvas_img);
                            bsp_display_unlock();
                        }
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }

    free(frame_buf);
    free(work_buf);
    fclose(f);
    free(filepath);
    
    vTaskDelay(pdMS_TO_TICKS(150));
    if (mjpeg_canvas_buf) {
        free(mjpeg_canvas_buf);
        mjpeg_canvas_buf = NULL;
    }
    
    mjpeg_playing = false;
    mjpeg_task_handle = NULL;
    vTaskDelete(NULL);
}

static void view_mjpeg_file(const char *filepath) {
    if (mjpeg_playing) {
        stop_file_explorer_media();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    img_viewer = lv_obj_create(explorer_container);
    lv_obj_set_size(img_viewer, 410, 502);
    lv_obj_set_style_bg_color(img_viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(img_viewer, LV_OPA_COVER, 0);
    lv_obj_center(img_viewer);
    lv_obj_move_foreground(img_viewer);

    canvas_img = lv_canvas_create(img_viewer);
    mjpeg_canvas_buf = heap_caps_malloc(CAM_WIDTH * CAM_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!mjpeg_canvas_buf) {
        lv_obj_delete(img_viewer);
        img_viewer = NULL;
        canvas_img = NULL;
        return;
    }
    memset(mjpeg_canvas_buf, 0, CAM_WIDTH * CAM_HEIGHT * 2);
    lv_canvas_set_buffer(canvas_img, mjpeg_canvas_buf, CAM_WIDTH, CAM_HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(canvas_img);

    lv_obj_t *btn_close = lv_button_create(img_viewer);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "Close");
    lv_obj_center(lbl_close);
    lv_obj_add_event_cb(btn_close, close_img_viewer_cb, LV_EVENT_CLICKED, NULL);

    char *path_copy = strdup(filepath);
    mjpeg_playing = true;
    xTaskCreate(mjpeg_play_task, "mjpeg_play", 8192, path_copy, 5, &mjpeg_task_handle);
}

static void img_viewer_delete_cb(lv_event_t *e) {
    void *img_data = lv_event_get_user_data(e);
    if (img_data) free(img_data);
}

static void img_dsc_delete_cb(lv_event_t *e) {
    void *img_dsc = lv_event_get_user_data(e);
    if (img_dsc) free(img_dsc);
}

static void view_image_file(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *img_data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!img_data) {
        fclose(f);
        return;
    }
    fread(img_data, 1, size, f);
    fclose(f);

    img_viewer = lv_obj_create(explorer_container);
    lv_obj_set_size(img_viewer, 410, 502);
    lv_obj_set_style_bg_color(img_viewer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(img_viewer, LV_OPA_COVER, 0);
    lv_obj_center(img_viewer);
    lv_obj_move_foreground(img_viewer);

    lv_obj_add_flag(img_viewer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(img_viewer, close_img_viewer_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(img_viewer, img_viewer_delete_cb, LV_EVENT_DELETE, (void*)img_data);

    const char *ext = strrchr(filepath, '.');
    bool is_png = (ext && strcasecmp(ext, ".png") == 0);
    bool is_jpg = (ext && (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0));

    if (is_png || is_jpg) {
        lv_image_dsc_t *img_dsc = calloc(1, sizeof(lv_image_dsc_t));
        if (img_dsc) {
            img_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
            img_dsc->header.cf = LV_COLOR_FORMAT_RAW;
            img_dsc->header.flags = 0;
            img_dsc->header.w = 0;
            img_dsc->header.h = 0;
            img_dsc->header.stride = 0;
            img_dsc->header.reserved_2 = 0;
            img_dsc->data_size = size;
            img_dsc->data = img_data;
            img_dsc->reserved = NULL;

            lv_obj_t *img = lv_image_create(img_viewer);
            lv_image_set_src(img, img_dsc);
            lv_obj_center(img);
            
            lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(img, close_img_viewer_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(img, img_dsc_delete_cb, LV_EVENT_DELETE, (void*)img_dsc);
        }
    }
}

static void file_click_cb(lv_event_t *e) {
    char *path = (char*)lv_event_get_user_data(e);
    if (!path) return;

    if (strcmp(path, "..") == 0) {
        char *last_slash = strrchr(current_dir, '/');
        if (last_slash && last_slash != current_dir) {
            *last_slash = '\0';
        } else {
            strcpy(current_dir, "/sdcard");
        }
        refresh_directory_list(current_dir);
        return;
    }

    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            strcpy(current_dir, path);
            refresh_directory_list(current_dir);
        } else {
            char *ext = strrchr(path, '.');
            if (ext) {
                if (strcasecmp(ext, ".c") == 0 || strcasecmp(ext, ".txt") == 0) {
                    view_text_file(path);
                } else if (strcasecmp(ext, ".mjp") == 0 || strcasecmp(ext, ".mjpeg") == 0) {
                    view_mjpeg_file(path);
                } else if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 ||
                           strcasecmp(ext, ".png") == 0) {
                    view_image_file(path);
                }
            }
        }
    }
}

static void refresh_directory_list(const char *path) {
    if (!file_list) return;
    lv_obj_clean(file_list);

    DIR *dir = opendir(path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return;
    }

    if (strcmp(path, "/sdcard") != 0) {
        lv_obj_t *btn = lv_list_add_button(file_list, LV_SYMBOL_DIRECTORY, ".. [Parent]");
        lv_obj_add_event_cb(btn, file_click_cb, LV_EVENT_CLICKED, (void*)"..");
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char full_path[300];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        const char *symbol = LV_SYMBOL_FILE;
        if (entry->d_type == DT_DIR) {
            symbol = LV_SYMBOL_DIRECTORY; 
        } else {
            char *ext = strrchr(entry->d_name, '.');
            if (ext) {
                if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 || 
                    strcasecmp(ext, ".mjp") == 0 || strcasecmp(ext, ".mjpeg") == 0 ||
                    strcasecmp(ext, ".png") == 0) {
                    symbol = LV_SYMBOL_IMAGE;
                }
            }
        }

        lv_obj_t *btn = lv_list_add_button(file_list, symbol, entry->d_name);
        char *allocated_path = strdup(full_path);
        lv_obj_add_event_cb(btn, file_click_cb, LV_EVENT_CLICKED, (void*)allocated_path);
        lv_obj_add_event_cb(btn, btn_delete_cb, LV_EVENT_DELETE, (void*)allocated_path);
    }
    closedir(dir);
}

void start_file_explorer(void) {
    if (!mount_sd_card()) {
        ESP_LOGE(TAG, "Could not launch File Explorer - No SD Card found.");
        return;
    }

    if (explorer_container == NULL) {
        explorer_container = lv_obj_create(tile_tools);
        lv_obj_remove_style_all(explorer_container);
        lv_obj_set_size(explorer_container, 410, 502);
        lv_obj_set_style_bg_color(explorer_container, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(explorer_container, LV_OPA_COVER, 0);

        file_list = lv_list_create(explorer_container);
        lv_obj_set_size(file_list, 390, 440);
        lv_obj_align(file_list, LV_ALIGN_TOP_MID, 0, 10);
        lv_obj_set_style_bg_color(file_list, lv_color_black(), 0);
        lv_obj_set_style_text_color(file_list, lv_color_white(), 0);

        refresh_directory_list(current_dir);
    } else {
        lv_obj_remove_flag(explorer_container, LV_OBJ_FLAG_HIDDEN);
        refresh_directory_list(current_dir);
    }
}

void close_file_explorer(void) {
    if (explorer_container != NULL) {
        lv_obj_add_flag(explorer_container, LV_OBJ_FLAG_HIDDEN);
    }
}

void stop_file_explorer_media(void) {
    if (mjpeg_playing) {
        mjpeg_playing = false;
    }
}