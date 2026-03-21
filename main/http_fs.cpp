#include "lvgl.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include <string.h>

static const char *TAG = "HTTP_FS";

typedef struct {
    uint8_t *buffer;
    uint32_t size;
    uint32_t pos;
} http_file_t;

static void *fs_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode) {
    if (mode != LV_FS_MODE_RD) return NULL;

    char url[256];
    snprintf(url, sizeof(url), "https://%s", path);
    ESP_LOGI(TAG, "Opening URL: %s", url);

    esp_http_client_config_t config = {};
    config.url = url;
    config.buffer_size = 4096;
    config.buffer_size_tx = 1024;
    // Set a timeout to prevent hanging UI
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return NULL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGW(TAG, "Content-Length not provided or 0, trying to read chunked...");
        content_length = 0; // Unknown size
    } else {
        ESP_LOGI(TAG, "Content-Length: %d", content_length);
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP Error: Status Code %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    http_file_t *f = (http_file_t *)lv_malloc(sizeof(http_file_t));
    if (!f) {
        ESP_LOGE(TAG, "Failed to allocate memory for file structure");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    if (content_length > 0) {
        // Known size
        f->buffer = (uint8_t *)lv_malloc(content_length);
        if (!f->buffer) {
            ESP_LOGE(TAG, "Failed to allocate memory for file buffer (%d bytes)", content_length);
            lv_free(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return NULL;
        }

        int total_read = 0;
        while (total_read < content_length) {
            int read_len = esp_http_client_read(client, (char *)f->buffer + total_read, content_length - total_read);
            if (read_len < 0) {
                ESP_LOGE(TAG, "Error reading data");
                lv_free(f->buffer);
                lv_free(f);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return NULL;
            }
            if (read_len == 0) {
                ESP_LOGE(TAG, "Connection closed early");
                break;
            }
            total_read += read_len;
        }
        f->size = total_read;
    } else {
        // Chunked encoding or unknown size
        int buf_size = 4096;
        f->buffer = (uint8_t *)lv_malloc(buf_size);
        if (!f->buffer) {
            lv_free(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return NULL;
        }

        int total_read = 0;
        while (1) {
            if (total_read + 1024 > buf_size) {
                buf_size *= 2;
                f->buffer = (uint8_t *)lv_realloc(f->buffer, buf_size);
                if (!f->buffer) {
                    ESP_LOGE(TAG, "Failed to reallocate memory");
                    lv_free(f);
                    esp_http_client_close(client);
                    esp_http_client_cleanup(client);
                    return NULL;
                }
            }
            int read_len = esp_http_client_read(client, (char *)f->buffer + total_read, 1024);
            if (read_len < 0) {
                ESP_LOGE(TAG, "Error reading data");
                lv_free(f->buffer);
                lv_free(f);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return NULL;
            }
            if (read_len == 0) {
                break; // EOF
            }
            total_read += read_len;
        }
        f->size = total_read;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    f->pos = 0;

    ESP_LOGI(TAG, "Successfully downloaded %lu bytes", (unsigned long)f->size);
    return f;
}

static lv_fs_res_t fs_close(lv_fs_drv_t *drv, void *file_p) {
    http_file_t *f = (http_file_t *)file_p;
    if (f) {
        if (f->buffer) lv_free(f->buffer);
        lv_free(f);
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br) {
    http_file_t *f = (http_file_t *)file_p;
    if (!f || !f->buffer) return LV_FS_RES_INV_PARAM;

    uint32_t remaining = f->size - f->pos;
    *br = (btr > remaining) ? remaining : btr;

    if (*br > 0) {
        memcpy(buf, f->buffer + f->pos, *br);
        f->pos += *br;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence) {
    http_file_t *f = (http_file_t *)file_p;
    if (!f) return LV_FS_RES_INV_PARAM;

    switch (whence) {
        case LV_FS_SEEK_SET:
            f->pos = pos;
            break;
        case LV_FS_SEEK_CUR:
            f->pos += pos;
            break;
        case LV_FS_SEEK_END:
            f->pos = f->size + pos; // In LVGL, pos is usually negative or 0 for END
            break;
    }

    if (f->pos > f->size) f->pos = f->size;
    return LV_FS_RES_OK;
}

static lv_fs_res_t fs_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p) {
    http_file_t *f = (http_file_t *)file_p;
    if (!f) return LV_FS_RES_INV_PARAM;
    *pos_p = f->pos;
    return LV_FS_RES_OK;
}

void lv_http_fs_init(void) {
    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);

    fs_drv.letter = 'H';
    fs_drv.open_cb = fs_open;
    fs_drv.close_cb = fs_close;
    fs_drv.read_cb = fs_read;
    fs_drv.seek_cb = fs_seek;
    fs_drv.tell_cb = fs_tell;

    lv_fs_drv_register(&fs_drv);
}
