# 📑 Technical Reference: ESP32-S3-Touch-LCD-5

This document serves as a technical reference for developers working with the Waveshare ESP32-S3-Touch-LCD-5 using **ESP-IDF 6.1** and **LVGL 9.4** in C++.

## 1. Architectural Complexity: The "Missing" GPIOs

The ESP32-S3's 16-bit RGB interface consumes nearly all available GPIOs. To manage peripherals, Waveshare uses a **CH422G I/O Expander** on the I2C bus.

> [!IMPORTANT]
> **Critical Developer Note:** You cannot control the LCD Reset, Touch Reset, or SD Card Chip Select via standard `gpio_set_level()` commands. You must send I2C commands to the CH422G.

## 2. Bus Specifications & Speeds

### I2C Bus (Peripherals & Expansion)
* **Pins:** SDA: GPIO 8 | SCL: GPIO 9
* **Expected Speed:** 400 kHz (Fast Mode).
  * *Note:* While the CH422G supports 1 MHz, the GT911 and PCF85063 RTC are rated for 400 kHz. Running the bus at 400 kHz ensures stability across all shared devices.
* **Component Addresses:**
  * **CH422G (IO Expander):**
    * `0x24`: System Configuration (WR_SET)
    * `0x26`: Read IO Port Status (RD_IO)
    * `0x38`: IO Port Byte-Write (WR_IO) - Bidirectional I/O
    * `0x23`: OC (Open-Collector) Byte-Write (WR_OC) - Output-only
  * **GT911 (Touch Controller):** 0x5D (Default) or 0x14
  * **PCF85063 (RTC):** 0x51

### SPI Bus (SD Card)
* **Interface Type:** SPI Mode (not SDMMC).
* **Pins:** MOSI: GPIO 11 | SCK: GPIO 12 | MISO: GPIO 13
* **Chip Select (CS):** Managed by CH422G IO4 (Active Low).
* **Expected Speed:** 40 MHz.
  * *Note:* In ESP-IDF, initialize the bus with `spi_bus_initialize`. Since CS is not a real GPIO, set the `spics_io_num` to -1 and manually toggle the CH422G IO4 bit before/after SPI transactions.

### RGB Interface (Display)
* **PCLK (Clock):** GPIO 7.
* **Expected Frequency:** ~12-21 MHz (standard for 800x480 @ 40-50 FPS).
* **Sync Pins:** VSYNC: GPIO 3 | HSYNC: GPIO 46 | DE: GPIO 5.
* **Data Pins:**
    * R[3-7]: 1, 2, 42, 41, 40
    * G[2-7]: 39, 0, 45, 48, 47, 21
    * B[3-7]: 14, 38, 18, 17, 10

## 3. Peripheral Control via CH422G

The CH422G manages the board's "housekeeping" signals. The `CH422GController` component provides a clean C++ interface for these operations.

### IO Port Register (Address 0x38) - Bidirectional
Used for critical system signals and digital inputs.

| Bit | Pin | Function | Requirement |
|---|---|---|---|
| 0 | IO0 | DI0 | Digital Input 0. |
| 1 | IO1 | TP_RST | Touch Reset. Pulse LOW then HIGH to wake GT911. |
| 2 | IO2 | LCD_BL | Backlight. Set HIGH to enable screen backlight. |
| 3 | IO3 | LCD_RST | LCD Reset. Pulse LOW then HIGH to initialize RGB panel. |
| 4 | IO4 | SD_CS | SD Chip Select. Pull LOW for SD operations; HIGH to release. |
| 5 | IO5 | DI1 | Digital Input 1. |

### OC Register (Address 0x23) - Output Only
Used for isolated digital outputs.

| Bit | Pin | Function | Requirement |
|---|---|---|---|
| 0 | DO0 | DO0 | Digital Output 0 (Isolated). |
| 1 | DO1 | DO1 | Digital Output 1 (Isolated). |

## 4. Hardware Implementation Guide

### Isolated I/O (DI / DO)
The board features optically isolated inputs and open-drain outputs accessible via the terminal block.

* **Digital Inputs (DI0, DI1):** Mapped to **IO0** and **IO5** of the IO Port register (**0x38**). These support 5V–36V signals.
* **Digital Outputs (DO0, DO1):** Mapped to **DO0** and **DO1** of the OC register (**0x23**).
  * **Software Control:** Controlled via I2C address **0x23**. Writing a `1` to the corresponding bit enables the output (sinks current to GND).

### Screen Backlight
* **Control Type:** Binary (ON/OFF) via CH422G IO2.

### Touch Screen (GT911)
* **Interrupt (IRQ):** GPIO 4.
* **Implementation:** Use the `esp_lcd_touch` component. Ensure you initialize the CH422G and pull IO1 (TP_RST) high before attempting to probe the touch IC.

### SD Card Mounting
For IDF 6.1, use the `sdspi_host` driver:
1. Initialize SPI bus on GPIO 11, 12, 13.
2. **Wake-up Sequence:** Send 100 dummy clocks on SCK with CS High before mounting.
3. Command CH422G to pull IO4 (SD_CS) LOW.
4. Call `esp_vfs_fat_sdspi_mount()`.

### Optimized JPEG Decoding
The ESP32-S3 supports hardware-accelerated JPEG decoding via SIMD instructions. Use the `esp_new_jpeg` component for maximum performance when rendering tiles.

## 5. C++ Developer Reminders

* **Memory:** Always allocate LVGL frame buffers and large image buffers in PSRAM using `MALLOC_CAP_SPIRAM`.
* **I2C Conflicts:** SDA/SCL are on GPIO 8/9. Avoid using these for RGB data.
* **CH422G Communication:** The chip requires the system configuration command (address `0x24`) to be sent once with value `0x01` to enable IO operations.

## 6. Development & Coding Standards

### Testing
* **Build Verification:** All changes **must** be tested by compiling the project using the command `idf.py build`. Ensure the project compiles without errors before considering a task complete.

### Formatting & Style
* **No Trailing Spaces:** Modified or added lines must **never** end with a space.
* **Indentation:** Use **2 spaces** for tabs/indentation for all new or modified code, regardless of the existing indentation style in the file.
