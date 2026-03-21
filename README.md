# 🗺️ ESP32-S3 Map Tile Viewer (Waveshare 5" LCD)

This project implements a high-performance map tile engine for the **Waveshare ESP32-S3-Touch-LCD-5** using **ESP-IDF 6.1** and **LVGL 9.4**. It is designed to display 256x256 pixel map tiles from an SD card with smooth scrolling and pinch-to-zoom support.

## 🚀 Key Features

*   **Optimized Tile Engine:** Efficient 5x3 grid (15 tiles) to cover the 800x480 screen.
*   **Touch Interactivity:**
    *   **Single-touch Drag:** Smoothly pan across the map.
    *   **Pinch-to-Zoom:** Multi-touch support for zooming between map levels.
*   **SIMD Accelerated Decoding:** Custom JPEG decoder leveraging the ESP32-S3's SIMD instructions (via `esp_new_jpeg`) for fast on-the-fly tile decompression.
*   **Storage Support:** Custom SPI SD Card driver that integrates with the **CH422G I/O Expander** for manual Chip Select (CS) management.
*   **PSRAM Double Buffering:** Full 800x480 framebuffers in octal PSRAM ensure tear-free animations.
*   **Dynamic Cache:** 4MB LVGL image cache in PSRAM to store recently decoded tiles, reducing SD card I/O and CPU load.

## 🛠️ Hardware Stack

*   **Display:** 5-inch 800x480 RGB LCD (16-bit color).
*   **Controller:** ESP32-S3 (with Octal PSRAM).
*   **Touch:** GT911 Capacitive Touch Controller (I2C).
*   **I/O Expander:** CH422G (managed via the `CH422GController` component for LCD reset, touch reset, backlight, and SD CS).
*   **Storage:** microSD card slot connected via SPI.

## 📂 Software Stack

*   **Framework:** ESP-IDF v6.1 (using the new `i2c_master` and `spi_master` drivers).
*   **UI Library:** LVGL v9.4.
*   **I/O Control:** Custom `CH422GController` C++ component.
*   **JPEG Decoder:** `esp_new_jpeg` for hardware-accelerated decompression.

## 📁 SD Card Structure

Tiles should be stored on a FAT32-formatted SD card in the following directory structure:

```text
/sdcard/tiles-jpg/
└── {zoom}/
    └── {x}/
        └── {y}.jpg
```

*   **Zoom levels:** 1 to 18 (automatically detected from directory structure).
*   **Tile size:** 256x256 pixels.
*   **Format:** JPEG (Standard or Progressive).

## 🔨 Setup & Build

1.  **Environment:** Ensure you have ESP-IDF v6.1 installed and configured.
2.  **Clone:** Clone this repository with its submodules.
3.  **Configure:** The project uses `sdkconfig.defaults` for optimal PSRAM and LVGL settings.
4.  **Build & Flash:**
    ```bash
    idf.py build
    idf.py -p {YOUR_PORT} flash monitor
    ```

## 🧠 Technical Implementation Details

### CH422G Complexity
The ESP32-S3-Touch-LCD-5 uses nearly all GPIOs for the 16-bit RGB interface. Most peripheral control lines (LCD Reset, TP Reset, SD CS) are managed via a **CH422G I/O Expander** on the I2C bus. This requires a specialized driver to toggle these pins before initializing the display or mounting the SD card.

### Multi-touch Logic
The `TileEngine` uses the multi-touch data from the GT911 to calculate the distance between two points during a "pressing" event, triggering a zoom level change when the pinch distance exceeds a threshold.

### Optimized JPEG Decoding
Standard LVGL decoders can be slow on microcontrollers. This project implements a custom LVGL image decoder that uses the ESP32-S3's vector instructions. Compressed JPEG data is read into internal RAM (when possible) for maximum throughput before being decoded into PSRAM.

## 📈 Performance & Future Roadmap

Current measurements show `TileEngine::updateTiles` takes ~280ms for logic and source updates. Visual latency is primarily bound by SD card read speed and decoding time.

**Planned Improvements:**
- [ ] **Asynchronous Loading:** Move tile decoding to a background task to prevent UI stutter during rapid scrolling.
- [ ] **MBTiles Support:** Implement a single-file storage format to reduce FATFS overhead and file-open latency.
- [ ] **Pre-fetching:** Load adjacent tiles into cache before they enter the viewport.
- [ ] **Vector Tiles:** Future support for MVT (Mapbox Vector Tiles) for smaller storage footprint and infinite zoom levels.
