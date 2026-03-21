#include "CH422GController.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

CH422GController::CH422GController(i2c_master_bus_handle_t bus_handle,
                                   uint8_t addr_config,
                                   uint8_t addr_rd_io,
                                   uint8_t addr_io,
                                   uint8_t addr_oc)
    : m_bus_handle(bus_handle),
      m_addr_config(addr_config),
      m_addr_rd_io(addr_rd_io),
      m_addr_io(addr_io),
      m_addr_oc(addr_oc) {
}

CH422GController::~CH422GController() {
    if (m_dev_config) i2c_master_bus_rm_device(m_dev_config);
    if (m_dev_rd_io)  i2c_master_bus_rm_device(m_dev_rd_io);
    if (m_dev_io)     i2c_master_bus_rm_device(m_dev_io);
    if (m_dev_oc)     i2c_master_bus_rm_device(m_dev_oc);
}

esp_err_t CH422GController::init() {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.scl_speed_hz = 400000;

    esp_err_t ret;
    if (!m_dev_config) {
        dev_cfg.device_address = m_addr_config;
        ret = i2c_master_bus_add_device(m_bus_handle, &dev_cfg, &m_dev_config);
        if (ret != ESP_OK) return ret;
    }

    if (!m_dev_rd_io) {
        dev_cfg.device_address = m_addr_rd_io;
        ret = i2c_master_bus_add_device(m_bus_handle, &dev_cfg, &m_dev_rd_io);
        if (ret != ESP_OK) return ret;
    }

    if (!m_dev_io) {
        dev_cfg.device_address = m_addr_io;
        ret = i2c_master_bus_add_device(m_bus_handle, &dev_cfg, &m_dev_io);
        if (ret != ESP_OK) return ret;
    }

    if (!m_dev_oc) {
        dev_cfg.device_address = m_addr_oc;
        ret = i2c_master_bus_add_device(m_bus_handle, &dev_cfg, &m_dev_oc);
        if (ret != ESP_OK) return ret;
    }

    // 1. Configure CH422G
    ret = writeConfig(m_cfg_cache);
    if (ret != ESP_OK) return ret;
    vTaskDelay(pdMS_TO_TICKS(10));

    // 2. Initial state: Backlight OFF, etc.
    ret = writeIO(m_io_cache);
    if (ret != ESP_OK) return ret;

    // 3. Initial OC state
    return writeOC(m_oc_cache);
}

// --- Configuration ---

esp_err_t CH422GController::setSleepMode(bool sleep) {
    if (sleep) m_cfg_cache |= BIT_CFG_SLEEP;
    else       m_cfg_cache &= ~BIT_CFG_SLEEP;
    return writeConfig(m_cfg_cache);
}

esp_err_t CH422GController::setOpenDrain(IOMode mode) {
    if (mode == IOMode::OpenDrain) m_cfg_cache |= BIT_CFG_OD_EN;
    else                          m_cfg_cache &= ~BIT_CFG_OD_EN;
    return writeConfig(m_cfg_cache);
}

esp_err_t CH422GController::setIOOutputEnable(IODirection direction) {
    if (direction == IODirection::Output) m_cfg_cache |= BIT_CFG_IO_OE;
    else                                  m_cfg_cache &= ~BIT_CFG_IO_OE;
    return writeConfig(m_cfg_cache);
}

// --- Setters (Output Pins) ---

esp_err_t CH422GController::setLCDReset(bool active) {
    if (active) m_io_cache &= ~BIT_IO_LCD_RST; 
    else        m_io_cache |= BIT_IO_LCD_RST;
    return writeIO(m_io_cache);
}

esp_err_t CH422GController::setTouchReset(bool active) {
    if (active) m_io_cache &= ~BIT_IO_TP_RST;
    else        m_io_cache |= BIT_IO_TP_RST;
    return writeIO(m_io_cache);
}

esp_err_t CH422GController::setBacklight(bool active) {
    if (active) m_io_cache |= BIT_IO_LCD_BL;
    else        m_io_cache &= ~BIT_IO_LCD_BL;
    return writeIO(m_io_cache);
}

esp_err_t CH422GController::setSDCardSelected(bool selected) {
    if (selected) m_io_cache &= ~BIT_IO_SD_CS;
    else          m_io_cache |= BIT_IO_SD_CS;
    return writeIO(m_io_cache);
}

esp_err_t CH422GController::setD0(bool level) {
    if (level) m_oc_cache |= BIT_OC_DO0;
    else       m_oc_cache &= ~BIT_OC_DO0;
    return writeOC(m_oc_cache);
}

esp_err_t CH422GController::setD1(bool level) {
    if (level) m_oc_cache |= BIT_OC_DO1;
    else       m_oc_cache &= ~BIT_OC_DO1;
    return writeOC(m_oc_cache);
}

esp_err_t CH422GController::setDI0(bool level) {
    if (level) m_io_cache |= BIT_IO_DI0;
    else       m_io_cache &= ~BIT_IO_DI0;
    return writeIO(m_io_cache);
}

esp_err_t CH422GController::setDI1(bool level) {
    if (level) m_io_cache |= BIT_IO_DI1;
    else       m_io_cache &= ~BIT_IO_DI1;
    return writeIO(m_io_cache);
}

// --- Getters ---

esp_err_t CH422GController::getLCDReset(bool *active) {
    if (!active) return ESP_ERR_INVALID_ARG;
    *active = !(m_io_cache & BIT_IO_LCD_RST);
    return ESP_OK;
}

esp_err_t CH422GController::getTouchReset(bool *active) {
    if (!active) return ESP_ERR_INVALID_ARG;
    *active = !(m_io_cache & BIT_IO_TP_RST);
    return ESP_OK;
}

esp_err_t CH422GController::getBacklight(bool *active) {
    if (!active) return ESP_ERR_INVALID_ARG;
    *active = (m_io_cache & BIT_IO_LCD_BL);
    return ESP_OK;
}

esp_err_t CH422GController::getSDCardSelected(bool *selected) {
    if (!selected) return ESP_ERR_INVALID_ARG;
    *selected = !(m_io_cache & BIT_IO_SD_CS);
    return ESP_OK;
}

esp_err_t CH422GController::getDI0(bool *level) {
    if (!level) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    esp_err_t ret = readIO(&val);
    if (ret == ESP_OK) {
        *level = (val & BIT_IO_DI0);
    }
    return ret;
}

esp_err_t CH422GController::getDI1(bool *level) {
    if (!level) return ESP_ERR_INVALID_ARG;
    uint8_t val;
    esp_err_t ret = readIO(&val);
    if (ret == ESP_OK) {
        *level = (val & BIT_IO_DI1);
    }
    return ret;
}

// --- Private Helpers ---

esp_err_t CH422GController::writeConfig(uint8_t val) {
    if (!m_dev_config) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit(m_dev_config, &val, 1, 100);
}

esp_err_t CH422GController::writeIO(uint8_t val) {
    if (!m_dev_io) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit(m_dev_io, &val, 1, 100);
}

esp_err_t CH422GController::writeOC(uint8_t val) {
    if (!m_dev_oc) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit(m_dev_oc, &val, 1, 100);
}

esp_err_t CH422GController::readIO(uint8_t *val) {
    if (!m_dev_rd_io) return ESP_ERR_INVALID_STATE;
    return i2c_master_receive(m_dev_rd_io, val, 1, 100);
}
