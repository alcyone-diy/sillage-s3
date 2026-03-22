#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "CH422GController.hpp"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "TileEngine.hpp"

static const char *TAG = "TILES_PROTOTYPE";

#define TEST_SCREEN          1
// --- Hardware Pins ---
#define I2C_SDA_PIN          8
#define I2C_SCL_PIN          9
#define TP_INT_PIN           4

#define SD_MOSI_PIN          11
#define SD_SCK_PIN           12
#define SD_MISO_PIN          13

// --- RGB LCD Settings (800x480) ---
#define LCD_H_RES            800
#define LCD_V_RES            480
#define LCD_PIXEL_CLOCK_HZ   (12 * 1000 * 1000) // 12MHz for stability

// --- LCD RGB Pinout ---
#define LCD_PIN_R3           1
#define LCD_PIN_R4           2
#define LCD_PIN_R5           42
#define LCD_PIN_R6           41
#define LCD_PIN_R7           40
#define LCD_PIN_G2           39
#define LCD_PIN_G3           0
#define LCD_PIN_G4           45
#define LCD_PIN_G5           48
#define LCD_PIN_G6           47
#define LCD_PIN_G7           21
#define LCD_PIN_B3           14
#define LCD_PIN_B4           38
#define LCD_PIN_B5           18
#define LCD_PIN_B6           17
#define LCD_PIN_B7           10
#define LCD_PIN_PCLK         7
#define LCD_PIN_HSYNC        46
#define LCD_PIN_VSYNC        3
#define LCD_PIN_DE           5

// Global Handles
i2c_master_bus_handle_t i2c_bus = NULL;
CH422GController *ch422g_controller = nullptr;
esp_lcd_panel_handle_t lcd_panel = NULL;
esp_lcd_touch_handle_t tp_handle = NULL;
SemaphoreHandle_t lvgl_mux = NULL;
sdmmc_card_t *card = NULL;

// Timing measurement
extern "C" {
    volatile int64_t update_start_time = 0;
    volatile bool measure_next_flush = false;
}

// Structure to hold multi-touch data for the TileEngine
struct TouchUserData {
    esp_lcd_touch_handle_t tp;
    lv_point_t points[2];
    uint8_t count;
};

void hardware_init(void);
esp_err_t init_sd_card(void);
void lvgl_init_task(void *arg);

void i2c_scan(void) {
    ESP_LOGI(TAG, "Scanning I2C bus...");
    for (uint8_t i = 1; i < 127; i++) {
        esp_err_t ret = i2c_master_probe(i2c_bus, i, 200);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Found device at address 0x%02X", i);
        }
    }
}

extern "C" void app_main(void) {
    hardware_init();
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreate(lvgl_init_task, "LVGL", 1024 * 16, NULL, 5, NULL);
}

void hardware_init(void) {
    ESP_LOGI(TAG, "Hardware stabilization (1.5s)...");
    vTaskDelay(pdMS_TO_TICKS(1500));

    // I2C Bus Initialization
    i2c_master_bus_config_t i2c_bus_conf = {};
    i2c_bus_conf.clk_source = I2C_CLK_SRC_DEFAULT;
    i2c_bus_conf.i2c_port = I2C_NUM_0;
    i2c_bus_conf.sda_io_num = (gpio_num_t)I2C_SDA_PIN;
    i2c_bus_conf.scl_io_num = (gpio_num_t)I2C_SCL_PIN;
    i2c_bus_conf.glitch_ignore_cnt = 7;
    i2c_bus_conf.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_conf, &i2c_bus));

    // CH422G IO Expander Initialization
    ESP_LOGI(TAG, "Initializing CH422G...");
    ch422g_controller = new CH422GController(i2c_bus);
    ESP_ERROR_CHECK(ch422g_controller->init());
    ESP_ERROR_CHECK(ch422g_controller->setIOOutputEnable(CH422GController::IODirection::Output));

    // Ensure backlight is ON as soon as possible
    ESP_ERROR_CHECK(ch422g_controller->setBacklight(true));

    // --- GT911 Reset Sequence for Address 0x5D ---
    ESP_LOGI(TAG, "Performing GT911 Reset Sequence (Address 0x5D)...");
    gpio_config_t tp_int_conf = {};
    tp_int_conf.pin_bit_mask = (1ULL << TP_INT_PIN);
    tp_int_conf.mode = GPIO_MODE_OUTPUT;
    tp_int_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    tp_int_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&tp_int_conf));

    // 1. Hold Reset Low, Hold INT Low
    ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)TP_INT_PIN, 0));
    ESP_ERROR_CHECK(ch422g_controller->setTouchReset(true)); // Active LOW
    vTaskDelay(pdMS_TO_TICKS(10));

    // 2. Release Reset, Keep INT Low
    ESP_ERROR_CHECK(ch422g_controller->setTouchReset(false)); // Release
    vTaskDelay(pdMS_TO_TICKS(10));

    // 3. Set INT back to Input
    tp_int_conf.mode = GPIO_MODE_INPUT;
    tp_int_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&tp_int_conf));
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_scan();

    // RGB LCD Initialization
    ESP_LOGI(TAG, "Initializing RGB LCD Panel...");
    esp_lcd_rgb_panel_config_t panel_conf = {};
    panel_conf.data_width = 16;
    panel_conf.clk_src = LCD_CLK_SRC_DEFAULT;
    panel_conf.disp_gpio_num = (gpio_num_t)-1;
    panel_conf.pclk_gpio_num = (gpio_num_t)LCD_PIN_PCLK;
    panel_conf.vsync_gpio_num = (gpio_num_t)LCD_PIN_VSYNC;
    panel_conf.hsync_gpio_num = (gpio_num_t)LCD_PIN_HSYNC;
    panel_conf.de_gpio_num = (gpio_num_t)LCD_PIN_DE;

    int data_gpios[] = {
        LCD_PIN_B3, LCD_PIN_B4, LCD_PIN_B5, LCD_PIN_B6, LCD_PIN_B7,
        LCD_PIN_G2, LCD_PIN_G3, LCD_PIN_G4, LCD_PIN_G5, LCD_PIN_G6, LCD_PIN_G7,
        LCD_PIN_R3, LCD_PIN_R4, LCD_PIN_R5, LCD_PIN_R6, LCD_PIN_R7,
    };
    for (int i = 0; i < 16; i++) {
        panel_conf.data_gpio_nums[i] = (gpio_num_t)data_gpios[i];
    }

    panel_conf.timings.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    panel_conf.timings.h_res = LCD_H_RES;
    panel_conf.timings.v_res = LCD_V_RES;
    panel_conf.timings.hsync_back_porch = 8;
    panel_conf.timings.hsync_front_porch = 8;
    panel_conf.timings.hsync_pulse_width = 4;
    panel_conf.timings.vsync_back_porch = 8;
    panel_conf.timings.vsync_front_porch = 8;
    panel_conf.timings.vsync_pulse_width = 4;
    panel_conf.timings.flags.pclk_active_neg = 1;
    panel_conf.flags.fb_in_psram = 1;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_conf, &lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_LOGI(TAG, "LCD RGB panel initialized successfully.");

    // GT911 Initialization
    uint16_t tp_addr = 0x5D;
    uint8_t prod_id[4] = {0};
    esp_lcd_panel_io_i2c_config_t probe_io_conf = {};
    probe_io_conf.dev_addr = 0x5D;
    probe_io_conf.control_phase_bytes = 1;
    probe_io_conf.lcd_cmd_bits = 16;
    probe_io_conf.flags.disable_control_phase = 1;
    probe_io_conf.scl_speed_hz = 400000;

    esp_lcd_panel_io_handle_t probe_io = NULL;

    ESP_LOGI(TAG, "Probing GT911...");
    if (esp_lcd_new_panel_io_i2c(i2c_bus, &probe_io_conf, &probe_io) == ESP_OK) {
        if (esp_lcd_panel_io_rx_param(probe_io, 0x8140, prod_id, 3) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 detected at 0x5D. ID: %c%c%c", prod_id[0], prod_id[1], prod_id[2]);
            tp_addr = 0x5D;
        } else {
            ESP_LOGW(TAG, "GT911 not at 0x5D, trying 0x14...");
            probe_io_conf.dev_addr = 0x14;
            esp_lcd_panel_io_del(probe_io);
            if (esp_lcd_new_panel_io_i2c(i2c_bus, &probe_io_conf, &probe_io) == ESP_OK) {
                if (esp_lcd_panel_io_rx_param(probe_io, 0x8140, prod_id, 3) == ESP_OK) {
                    ESP_LOGI(TAG, "GT911 detected at 0x14. ID: %c%c%c", prod_id[0], prod_id[1], prod_id[2]);
                    tp_addr = 0x14;
                } else {
                    ESP_LOGE(TAG, "GT911 not detected!");
                }
            }
        }
        if (probe_io) esp_lcd_panel_io_del(probe_io);
    }

    ESP_LOGI(TAG, "Initializing GT911 driver at 0x%02X...", tp_addr);
    esp_lcd_panel_io_i2c_config_t tp_io_config = {};
    tp_io_config.dev_addr = tp_addr;
    tp_io_config.scl_speed_hz = 400000;
    tp_io_config.control_phase_bytes = 1;
    tp_io_config.lcd_cmd_bits = 16;
    tp_io_config.flags.disable_control_phase = 1;

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle));

    static esp_lcd_touch_io_gt911_config_t gt911_dev_conf;
    gt911_dev_conf.dev_addr = tp_addr;

    esp_lcd_touch_config_t tp_cfg = {};
    tp_cfg.x_max = LCD_H_RES;
    tp_cfg.y_max = LCD_V_RES;
    tp_cfg.rst_gpio_num = (gpio_num_t)-1; 
    tp_cfg.int_gpio_num = (gpio_num_t)TP_INT_PIN;
    tp_cfg.levels.reset = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy = 0;
    tp_cfg.flags.mirror_x = 0;
    tp_cfg.flags.mirror_y = 0;
    tp_cfg.driver_data = &gt911_dev_conf;

    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp_handle));
    ESP_LOGI(TAG, "Touch controller initialized successfully.");

   // SD Card Initialization
    esp_err_t ret = init_sd_card();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (%s).", esp_err_to_name(ret));
    }
}

esp_err_t init_sd_card(void) {
    ESP_LOGI(TAG, "Initializing SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 40000;

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = (gpio_num_t)SD_MOSI_PIN;
    bus_cfg.miso_io_num = (gpio_num_t)SD_MISO_PIN;
    bus_cfg.sclk_io_num = (gpio_num_t)SD_SCK_PIN;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;

    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = (gpio_num_t)-1;
    slot_config.host_id = (spi_host_device_t)host.slot;

    ch422g_controller->setSDCardSelected(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        ch422g_controller->setSDCardSelected(false);
    }

    return ret;
}

// LVGL Flush Callback
void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_draw_bitmap(lcd_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    lv_display_flush_ready(disp);

    if (measure_next_flush && lv_display_flush_is_last(disp)) {
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI("TIMING", "TOTAL SCREEN UPDATE TIME: %lld us", (end_time - update_start_time));
        measure_next_flush = false;
    }
}

// LVGL Tick Callback
static uint32_t lv_tick_cb(void) {
    return esp_timer_get_time() / 1000;
}

// LVGL Touch Read Callback
static void lvgl_touch_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    TouchUserData * user_data = (TouchUserData *)lv_indev_get_user_data(indev);
    if (!user_data || !user_data->tp) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    uint8_t point_cnt = 0;
    esp_lcd_touch_point_data_t pts[2];

    if (esp_lcd_touch_read_data(user_data->tp) == ESP_OK) {
        if (esp_lcd_touch_get_data(user_data->tp, pts, &point_cnt, 2) == ESP_OK && point_cnt > 0) {
            user_data->count = point_cnt;
            user_data->points[0].x = pts[0].x;
            user_data->points[0].y = pts[0].y;

            data->point.x = pts[0].x;
            data->point.y = pts[0].y;
            data->state = LV_INDEV_STATE_PRESSED;

            if (point_cnt > 1) {
                user_data->points[1].x = pts[1].x;
                user_data->points[1].y = pts[1].y;
            }
            return;
        }
    }
    user_data->count = 0;
    data->state = LV_INDEV_STATE_RELEASED;
}

static void btn_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Button clicked!");
    }
}

void lvgl_init_task(void *arg) {
    ESP_LOGI(TAG, "Starting LVGL task...");
    lv_init();
    lv_tick_set_cb(lv_tick_cb);

    // Resize image cache (4MB in PSRAM)
    lv_image_cache_resize(4 * 1024 * 1024, false);

    // Use full framebuffers in PSRAM for smooth double-buffering
    uint32_t buffer_size = LCD_H_RES * LCD_V_RES;
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buffer_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers in PSRAM");
        abort();
    }

    // Initialize LVGL Display
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_buffers(disp, buf1, buf2, buffer_size * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    // Register Touch Input Device
    if (tp_handle) {
        static TouchUserData touch_user_data;
        touch_user_data.tp = tp_handle;
        touch_user_data.count = 0;

        lv_indev_t * indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_user_data(indev, &touch_user_data);
        lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
    }

#if TEST_SCREEN
    ESP_LOGI(TAG, "Entering TEST_SCREEN mode");
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    TileEngine::lv_jpeg_esp_decoder_init();

    const char* path = "/sdcard/tiles-jpg/12/2034/1455.jpg";
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        ESP_LOGI(TAG, "SUCCESS: File %s found, size: %ld bytes", path, size);
    } else {
        ESP_LOGE(TAG, "ERROR: Could NOT open file %s", path);
    }

    lv_obj_t *img = lv_image_create(scr);
    // When using CONFIG_LV_FS_STDIO_PATH="/sdcard/", the path is relative to it
    // Wait, let's verify if the path should be S:tiles-jpg/... or S:/tiles-jpg/...
    // By default in LVGL, the drive letter is stripped and the rest is appended to the root path.
    // If root path is "/sdcard/", "S:tiles-jpg" becomes "/sdcard/tiles-jpg".
    // If it's "S:/tiles-jpg", it becomes "/sdcard//tiles-jpg" which is also fine in POSIX.
    lv_image_set_src(img, "S:/tiles-jpg/12/2034/1455.jpg");
    lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
    ESP_LOGI(TAG, "Image source set to S:/tiles-jpg/12/2034/1455.jpg");
#else
    // Initialize Tile Engine
    static TileEngine engine;
    engine.init();
    engine.setMapCenter(/*lat=*/-25.0, /*lon=*/25.0, /*zoom=*/8);
#endif

    ESP_LOGI(TAG, "LVGL initialization complete. Entering main loop...");

    // Periodic LVGL Task execution
    while (1) {
        xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY);
        uint32_t time_till_next = lv_timer_handler();
        xSemaphoreGiveRecursive(lvgl_mux);
        vTaskDelay(pdMS_TO_TICKS(time_till_next > 0 ? time_till_next : 1));
    }
}
