#define _USE_MATH_DEFINES
#include "TileEngine.hpp"
#include "esp_lcd_touch.h"
#include "src/draw/lv_image_decoder_private.h"
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"

#include <sys/stat.h>

#define JPEG_FORMAT   1
#define PNG_FORMAT    2
#define RGB565_FORMAT 3
#define TILE_FORMAT JPEG_FORMAT

#if TILE_FORMAT == JPEG_FORMAT
#define TILE_EXTENTION "jpg"
#define TILE_PATH_BASE_DIR "/tiles-jpg"
#elif TILE_FORMAT == PNG_FORMAT
#define TILE_EXTENTION "png"
#define TILE_PATH_BASE_DIR "/tiles-png"
#elif TILE_FORMAT == RGB565_FORMAT
#define TILE_EXTENTION "rgb565"
#define TILE_PATH_BASE_DIR "/tiles-rgb565"
#endif

static const char* TAG = "TileEngine";

extern "C" {
    extern volatile int64_t update_start_time;
    extern volatile bool measure_next_flush;
}

// Optimized JPEG Decoder using esp_new_jpeg for ESP32-S3 SIMD speedup
static lv_result_t jpeg_esp_decoder_info(lv_image_decoder_t * decoder, lv_image_decoder_dsc_t * dsc, lv_image_header_t * header) {
    if(dsc->src_type == LV_IMAGE_SRC_FILE) {
        const char * path = (const char *)dsc->src;
        if(strstr(path, ".jpg") != NULL || strstr(path, ".jpeg") != NULL) {
            header->cf = LV_COLOR_FORMAT_RGB565;
            header->w = 256;
            header->h = 256;
            header->stride = 256 * 2;
            header->magic = LV_IMAGE_HEADER_MAGIC;
            return LV_RESULT_OK;
        }
    }
    return LV_RESULT_INVALID;
}

static lv_result_t jpeg_esp_decoder_open(lv_image_decoder_t * decoder, lv_image_decoder_dsc_t * dsc) {
    ESP_LOGI("TileDecoder", "jpeg_esp_decoder_open called for src: %s", (const char*)dsc->src);
    if(dsc->src_type == LV_IMAGE_SRC_FILE) {
        const char * lv_path = (const char *)dsc->src;
        if(strstr(lv_path, ".jpg") != NULL || strstr(lv_path, ".jpeg") != NULL) {
            char posix_path[128];
            if (lv_path[0] == 'S' && lv_path[1] == ':') {
                snprintf(posix_path, sizeof(posix_path), "/sdcard%s", lv_path + 2);
                ESP_LOGI("TileDecoder", "Resolved posix path: %s", posix_path);
            } else {
                ESP_LOGE("TileDecoder", "Invalid prefix format in path: %s", lv_path);
                return LV_RESULT_INVALID;
            }

            FILE* f = fopen(posix_path, "rb");
            if(!f) {
                ESP_LOGE("TileDecoder", "fopen failed for %s", posix_path);
                return LV_RESULT_INVALID;
            }

            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            ESP_LOGI("TileDecoder", "File %s opened, size: %ld", posix_path, file_size);

            // Use INTERNAL RAM for compressed data - MUCH faster for the decoder
            uint8_t* jpeg_data = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if(!jpeg_data) {
                ESP_LOGW("TileDecoder", "INTERNAL RAM allocation failed, fallback to PSRAM");
                // Fallback to PSRAM if internal is full, but it will be slower
                jpeg_data = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }

            if(!jpeg_data) {
                ESP_LOGE("TileDecoder", "Memory allocation failed for %ld bytes", file_size);
                fclose(f);
                return LV_RESULT_INVALID;
            }

            size_t bytes_read = fread(jpeg_data, 1, file_size, f);
            ESP_LOGI("TileDecoder", "Read %zu bytes from %s", bytes_read, posix_path);
            fclose(f);

            jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
            config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

            jpeg_dec_handle_t jpeg_dec;
            jpeg_error_t jerr = jpeg_dec_open(&config, &jpeg_dec);
            if(jerr != JPEG_ERR_OK) {
                ESP_LOGE("TileDecoder", "jpeg_dec_open failed with error: %d", jerr);
                free(jpeg_data);
                return LV_RESULT_INVALID;
            }

            jpeg_dec_io_t io = {
                .inbuf = jpeg_data,
                .inbuf_len = (int)file_size,
                .inbuf_remain = 0,
                .outbuf = NULL,
                .out_size = 0
            };

            jpeg_dec_header_info_t out_info;
            jerr = jpeg_dec_parse_header(jpeg_dec, &io, &out_info);
            if(jerr != JPEG_ERR_OK) {
                ESP_LOGE("TileDecoder", "jpeg_dec_parse_header failed with error: %d", jerr);
                jpeg_dec_close(jpeg_dec);
                free(jpeg_data);
                return LV_RESULT_INVALID;
            }

            uint32_t w = out_info.width;
            uint32_t h = out_info.height;
            uint32_t stride = w * 2;
            uint32_t data_size = stride * h;
            ESP_LOGI("TileDecoder", "JPEG parsed. W: %lu, H: %lu, Stride: %lu, Size: %lu", w, h, stride, data_size);

            lv_draw_buf_t * draw_buf = (lv_draw_buf_t *)lv_malloc(sizeof(lv_draw_buf_t));
            if(!draw_buf) {
                ESP_LOGE("TileDecoder", "lv_malloc failed for draw_buf");
                jpeg_dec_close(jpeg_dec);
                free(jpeg_data);
                return LV_RESULT_INVALID;
            }

            void * data = heap_caps_aligned_alloc(16, data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(!data) {
                ESP_LOGE("TileDecoder", "heap_caps_aligned_alloc failed for %lu bytes", data_size);
                lv_free(draw_buf);
                jpeg_dec_close(jpeg_dec);
                free(jpeg_data);
                return LV_RESULT_INVALID;
            }

            lv_draw_buf_init(draw_buf, w, h, LV_COLOR_FORMAT_RGB565, stride, data, data_size);

            io.outbuf = (uint8_t*)draw_buf->data;
            io.out_size = (int)data_size;

            jerr = jpeg_dec_process(jpeg_dec, &io);
            jpeg_dec_close(jpeg_dec);
            free(jpeg_data);

            if (jerr != JPEG_ERR_OK) {
                ESP_LOGE("TileDecoder", "jpeg_dec_process failed with error: %d", jerr);
                heap_caps_free(data);
                lv_free(draw_buf);
                return LV_RESULT_INVALID;
            }

            dsc->decoded = draw_buf;
            ESP_LOGI("TileDecoder", "jpeg_esp_decoder_open SUCCESS!");
            return LV_RESULT_OK;
        }
    }
    return LV_RESULT_INVALID;
}

static void jpeg_esp_decoder_close(lv_image_decoder_t * decoder, lv_image_decoder_dsc_t * dsc) {
    if(dsc->decoded) {
        lv_draw_buf_t * draw_buf = (lv_draw_buf_t *)dsc->decoded;
        if(draw_buf->data) heap_caps_free(draw_buf->data);
        lv_free(draw_buf);
        dsc->decoded = NULL;
    }
}

void TileEngine::lv_jpeg_esp_decoder_init() {
    ESP_LOGI("TileDecoder", "Registering optimized S3 JPEG decoder");
    lv_image_decoder_t * decoder = lv_image_decoder_create();
    lv_image_decoder_set_info_cb(decoder, jpeg_esp_decoder_info);
    lv_image_decoder_set_open_cb(decoder, jpeg_esp_decoder_open);
    lv_image_decoder_set_close_cb(decoder, jpeg_esp_decoder_close);
}

TileEngine::TileEngine() : _map_container(nullptr), _tile_layer(nullptr) {}

TileEngine::~TileEngine() {}

static lv_result_t rgb565_decoder_info(lv_image_decoder_t * decoder, lv_image_decoder_dsc_t * dsc, lv_image_header_t * header) {
    if(dsc->src_type == LV_IMAGE_SRC_FILE) {
        const char * path = (const char *)dsc->src;
        if(strstr(path, ".rgb565") != NULL) {
            header->cf = LV_COLOR_FORMAT_RGB565;
            header->w = 256;
            header->h = 256;
            header->stride = 256 * 2;
            header->magic = LV_IMAGE_HEADER_MAGIC;
            return LV_RESULT_OK;
        }
    }
    return LV_RESULT_INVALID;
}

static lv_result_t rgb565_decoder_open(lv_image_decoder_t * decoder, lv_image_decoder_dsc_t * dsc) {
    if(dsc->src_type == LV_IMAGE_SRC_FILE) {
        const char * lv_path = (const char *)dsc->src;
        if(strstr(lv_path, ".rgb565") != NULL) {
            char posix_path[128];
            if (lv_path[0] == 'S' && lv_path[1] == ':') {
                snprintf(posix_path, sizeof(posix_path), "/sdcard%s", lv_path + 2);
            } else {
                return LV_RESULT_INVALID;
            }

            uint32_t w = 256;
            uint32_t h = 256;
            uint32_t stride = w * 2;
            uint32_t data_size = stride * h;

            lv_draw_buf_t * draw_buf = (lv_draw_buf_t *)lv_malloc(sizeof(lv_draw_buf_t));
            if(!draw_buf) return LV_RESULT_INVALID;

            void * data = heap_caps_aligned_alloc(64, data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(!data) {
                lv_free(draw_buf);
                return LV_RESULT_INVALID;
            }

            lv_draw_buf_init(draw_buf, w, h, LV_COLOR_FORMAT_RGB565, stride, data, data_size);

            FILE* f = fopen(posix_path, "rb");
            if(!f) {
                heap_caps_free(data);
                lv_free(draw_buf);
                return LV_RESULT_INVALID;
            }

            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);

            if (file_size > (long)data_size) {
                fseek(f, file_size - data_size, SEEK_SET);
            } else {
                fseek(f, 0, SEEK_SET);
            }

            size_t br = fread(draw_buf->data, 1, data_size, f);
            fclose(f);

            if (br != data_size) {
                heap_caps_free(data);
                lv_free(draw_buf);
                return LV_RESULT_INVALID;
            }

            dsc->decoded = draw_buf;
            return LV_RESULT_OK;
        }
    }
    return LV_RESULT_INVALID;
}

static void rgb565_decoder_close(lv_image_decoder_t * decoder, lv_image_decoder_dsc_t * dsc) {
    if(dsc->decoded) {
        lv_draw_buf_t * draw_buf = (lv_draw_buf_t *)dsc->decoded;
        if(draw_buf->data) heap_caps_free(draw_buf->data);
        lv_free(draw_buf);
        dsc->decoded = NULL;
    }
}

void TileEngine::lv_rgb565_decoder_init() {
    ESP_LOGI("TileDecoder", "Registering custom RGB565 decoder");
    lv_image_decoder_t * decoder = lv_image_decoder_create();
    lv_image_decoder_set_info_cb(decoder, rgb565_decoder_info);
    lv_image_decoder_set_open_cb(decoder, rgb565_decoder_open);
    lv_image_decoder_set_close_cb(decoder, rgb565_decoder_close);
}

void TileEngine::init() {
    ESP_LOGI(TAG, "Initializing TileEngine with container layer");
    loadConfig();
    lv_rgb565_decoder_init();
    lv_jpeg_esp_decoder_init();

    _map_container = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(_map_container);
    lv_obj_set_size(_map_container, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(_map_container, lv_color_make(30, 30, 30), 0);
    lv_obj_set_style_bg_opa(_map_container, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(_map_container, 0, 0);
    lv_obj_set_style_border_width(_map_container, 0, 0);
    lv_obj_remove_flag(_map_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_map_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(_map_container, this);
    lv_obj_add_event_cb(_map_container, event_handler, LV_EVENT_ALL, NULL);

    _tile_layer = lv_obj_create(_map_container);
    lv_obj_remove_style_all(_tile_layer);
    lv_obj_set_size(_tile_layer, TILE_SIZE * GRID_COLS, TILE_SIZE * GRID_ROWS);
    lv_obj_set_style_pad_all(_tile_layer, 0, 0);
    lv_obj_set_style_border_width(_tile_layer, 0, 0);
    lv_obj_remove_flag(_tile_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(_tile_layer, LV_OBJ_FLAG_CLICKABLE);

    for (int r = 0; r < GRID_ROWS; ++r) {
        for (int c = 0; c < GRID_COLS; ++c) {
            lv_obj_t* img = lv_image_create(_tile_layer);
            lv_obj_remove_style_all(img);
            lv_obj_set_size(img, TILE_SIZE, TILE_SIZE);
            lv_obj_set_pos(img, c * TILE_SIZE, r * TILE_SIZE);
            // Ensure no border, outline or padding is added by default themes
            lv_obj_set_style_border_width(img, 0, 0);
            lv_obj_set_style_outline_width(img, 0, 0);
            lv_obj_set_style_pad_all(img, 0, 0);

            TileInfo info = {img, -1, -1, -1, ""};
            _tile_grid.push_back(info);
        }
    }
}

void TileEngine::setMapCenter(double lat, double lon, int zoom) {
    _current_lat = lat;
    _current_lon = lon;

    if (zoom < _min_zoom) zoom = _min_zoom;
    if (zoom > _max_zoom) zoom = _max_zoom;
    _current_zoom = zoom;

    _base_tile_x = -1; // Force full refresh
    _base_tile_y = -1;
    updateTiles(lat, lon, zoom);
}

void TileEngine::latLonToTile(double lat, double lon, int zoom, double& x, double& y) {
    double n = std::pow(2.0, zoom);
    x = (lon + 180.0) / 360.0 * n;
    double lat_rad = lat * M_PI / 180.0;
    y = (1.0 - std::log(std::tan(lat_rad) + (1.0 / std::cos(lat_rad))) / M_PI) / 2.0 * n;
}

void TileEngine::tileToLatLon(double x, double y, int zoom, double& lat, double& lon) {
    double n = std::pow(2.0, zoom);
    lon = x / n * 360.0 - 180.0;
    double lat_rad = std::atan(std::sinh(M_PI * (1.0 - 2.0 * y / n)));
    lat = lat_rad * 180.0 / M_PI;
}

void TileEngine::getTilePath(char* buf, size_t buf_size, int zoom, int x, int y, bool for_lvgl) {
    if (for_lvgl) {
        snprintf(buf, buf_size, "%s%s/%d/%d/%d." TILE_EXTENTION, LV_DRIVE_PREFIX, TILE_PATH_BASE_DIR, zoom, x, y);
    } else {
        snprintf(buf, buf_size, "/sdcard%s/%d/%d/%d." TILE_EXTENTION, TILE_PATH_BASE_DIR, zoom, x, y);
    }
}

// Structure to hold multi-touch data (shared with main.cpp)
struct TouchUserData {
    esp_lcd_touch_handle_t tp;
    lv_point_t points[2];
    uint8_t count;
};

void TileEngine::event_handler(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* obj = (lv_obj_t*)lv_event_get_target(e);
    TileEngine* engine = (TileEngine*)lv_obj_get_user_data(obj);

    lv_indev_t* indev = lv_indev_active();
    TouchUserData* touch_data = (TouchUserData*)lv_indev_get_user_data(indev);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &engine->_last_drag_point);
        engine->_is_pinching = false;
        engine->_last_pinch_dist = 0;
    } else if (code == LV_EVENT_PRESSING) {
        // --- Multi-touch Pinch to Zoom ---
        if (touch_data && touch_data->count == 2) {
            double dx = touch_data->points[0].x - touch_data->points[1].x;
            double dy = touch_data->points[0].y - touch_data->points[1].y;
            double dist = sqrt(dx*dx + dy*dy);

            if (!engine->_is_pinching) {
                engine->_is_pinching = true;
                engine->_last_pinch_dist = dist;
            } else {
                uint32_t now = lv_tick_get();
                if (now - engine->_last_zoom_time > 500) { // Cooldown between zoom steps
                    bool zoomed = false;
                    if (dist > engine->_last_pinch_dist * 1.5) { // Zoom In
                        if (engine->_current_zoom < engine->_max_zoom) {
                            engine->_current_zoom++;
                            zoomed = true;
                        }
                        engine->_last_pinch_dist = dist;
                    } else if (dist < engine->_last_pinch_dist * 0.6) { // Zoom Out
                        if (engine->_current_zoom > engine->_min_zoom) {
                            engine->_current_zoom--;
                            zoomed = true;
                        }
                        engine->_last_pinch_dist = dist;
                    }

                    if (zoomed) {
                        engine->_last_zoom_time = now;
                        // Reset layer pos so it doesn't jump during zoom
                        lv_obj_set_pos(engine->_tile_layer, 0, 0);
                        engine->_base_tile_x = -1; // Force full refresh
                        engine->updateTiles(engine->_current_lat, engine->_current_lon, engine->_current_zoom);
                    }
                }
            }
            return; // Skip dragging if we are pinching
        }

        // --- Single-touch Dragging ---
        static uint32_t last_update = 0;
        uint32_t now = lv_tick_get();
        if (now - last_update < 20) return; // Limit logic updates to ~50fps

        lv_point_t current_point;
        lv_indev_get_point(indev, &current_point);

        int dx = current_point.x - engine->_last_drag_point.x;
        int dy = current_point.y - engine->_last_drag_point.y;

        if (dx != 0 || dy != 0) {
            // Drop frame if we are still busy rendering the previous one
            if (measure_next_flush) return;

            double tile_x, tile_y;
            engine->latLonToTile(engine->_current_lat, engine->_current_lon, engine->_current_zoom, tile_x, tile_y);

            tile_x -= (double)dx / TILE_SIZE;
            tile_y -= (double)dy / TILE_SIZE;

            engine->tileToLatLon(tile_x, tile_y, engine->_current_zoom, engine->_current_lat, engine->_current_lon);
            engine->updateTiles(engine->_current_lat, engine->_current_lon, engine->_current_zoom);

            engine->_last_drag_point = current_point;
            last_update = now;
        }
    }
}

void TileEngine::updateTiles(double lat, double lon, int zoom) {
    update_start_time = esp_timer_get_time();
    measure_next_flush = true;
    double tile_x, tile_y;
    latLonToTile(lat, lon, zoom, tile_x, tile_y);

    double center_pixel_x = tile_x * TILE_SIZE;
    double center_pixel_y = tile_y * TILE_SIZE;

    int screen_tl_x = (int)(center_pixel_x - (SCREEN_WIDTH / 2));
    int screen_tl_y = (int)(center_pixel_y - (SCREEN_HEIGHT / 2));

    int base_tile_x = (int)std::floor((double)screen_tl_x / TILE_SIZE);
    int base_tile_y = (int)std::floor((double)screen_tl_y / TILE_SIZE);

    int offset_x = (base_tile_x * TILE_SIZE) - screen_tl_x;
    int offset_y = (base_tile_y * TILE_SIZE) - screen_tl_y;

    // Use container-based movement if base tiles haven't changed
    if (base_tile_x == _base_tile_x && base_tile_y == _base_tile_y && zoom == _current_zoom) {
        lv_obj_set_pos(_tile_layer, offset_x, offset_y);
        return;
    }

    // Otherwise, do a full repositioning and update sources
    _base_tile_x = base_tile_x;
    _base_tile_y = base_tile_y;
    _current_zoom = zoom;

    lv_obj_set_pos(_tile_layer, offset_x, offset_y);

    for (int r = 0; r < GRID_ROWS; ++r) {
        for (int c = 0; c < GRID_COLS; ++c) {
            int tile_idx_x = base_tile_x + c;
            int tile_idx_y = base_tile_y + r;
            auto& tile = _tile_grid[r * GRID_COLS + c];

            if (tile.x_idx != tile_idx_x || tile.y_idx != tile_idx_y || tile.zoom != zoom) {
                tile.x_idx = tile_idx_x;
                tile.y_idx = tile_idx_y;
                tile.zoom = zoom;
                getTilePath(tile.path, sizeof(tile.path), zoom, tile_idx_x, tile_idx_y, true);
                lv_image_set_src(tile.img_obj, tile.path);
            }
        }
    }
}

void TileEngine::loadConfig() {
    char base_path[128];
    snprintf(base_path, sizeof(base_path), "/sdcard%s", TILE_PATH_BASE_DIR);

    DIR* dir = opendir(base_path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "Could not open tiles directory: %s. Using defaults.", base_path);
        _min_zoom = 1;
        _max_zoom = 18;
        return;
    }

    struct dirent* entry;
    int min = 999;
    int max = -1;

    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (entry->d_name[0] == '.') continue;

        // Try to parse the directory name as an integer (zoom level)
        char* endptr;
        int zoom = strtol(entry->d_name, &endptr, 10);
        if (endptr != entry->d_name && *endptr == '\0') {
            if (zoom < min) min = zoom;
            if (zoom > max) max = zoom;
        }
    }
    closedir(dir);

    if (max != -1) {
        _min_zoom = min;
        _max_zoom = max;
        ESP_LOGI(TAG, "Detected zoom range from SD directories: %d to %d", _min_zoom, _max_zoom);
    } else {
        ESP_LOGW(TAG, "No zoom level directories found in %s. Using defaults (1-18).", base_path);
        _min_zoom = 1;
        _max_zoom = 18;
    }
}

#if TILE_DEBUG
void TileEngine::debug(double lat, double lon, int zoom) {
    ESP_LOGI(TAG, "--- Tile Engine Debug Start (Lat: %f, Lon: %f, Zoom: %d) ---", lat, lon, zoom);
}
#endif
