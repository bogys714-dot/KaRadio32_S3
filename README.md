# KaRadio32_S3 (Custom Fork)

This is a heavily modified and optimized version of the KaRadio32 web radio, specifically adapted for the **ESP32-S3** microcontroller with **16MB Flash and 8MB PSRAM**. 

The project is fully configured for development and compilation inside **VS Code with PlatformIO**.

---

## 🚀 Key Improvements & Modifications

* **Enhanced Hardware Utilization:** Tailored specifically for ESP32-S3 (16MB/8MB) configuration to leverage maximum performance and memory availability.
* **Expanded Partition Table:** Increased the application partition size from **1.75MB to 5MB**, allowing for a more stable firmware structure and future feature expansions.
* **Massive Audio Buffer:** Increased the audio stream buffer size up to **2MB** (utilizing the external PSRAM) for flawless, stutter-free playback even on unstable network connections.
* **Web-Based Display Selection:** Moved the display configuration and selection directly into the Web Interface, eliminating the need to hardcode display types before compilation.
* **Streamlined Firmware:** Removed obsolete web-update dependencies and unnecessary remote calls to external sites for faster and more private operation.
* **Custom OTA Updates:** Implemented a robust Over-The-Air (OTA) update mechanism for seamless wireless firmware flashing.
* **Under the Hood:** Numerous code optimizations, bug fixes, and stability improvements for the S3 architecture.

---

## 🔌 Hardware Wiring & Peripheral Connections

All hardware pins are statically defined. Below is the reference table for connecting your peripherals to the ESP32-S3 development board.

> ⚠️ **Important Note on Octal PSRAM:** GPIO pins from **26 to 37** are strictly reserved for the internal Octal PSRAM/Flash communication bus and **cannot** be used for external hardware connections.

### Peripheral Pinout Table

| Peripheral | Signal Name | ESP32-S3 GPIO | Description |
| :--- | :--- | :---: | :--- |
| **SPI Bus (Common)** | SPI_MISO | **GPIO 13** | SPI Master Input, Slave Output |
| | SPI_MOSI | **GPIO 11** | SPI Master Output, Slave Input |
| | SPI_CLK | **GPIO 12** | SPI Clock |
| **TFT Display** | TFT_CS | **GPIO 10** | Chip Select |
| | TFT_DC | **GPIO 7** | Data / Command Selection |
| | TFT_RST | **GPIO 6** | Hardware Reset (or connect to EN) |
| | BACKLIGHT | **GPIO 5** | LCD Brightness Control PWM |
| **I2S Audio DAC** | I2S_LRCK | **GPIO 18** | Left / Right Clock (WS) |
| | I2S_BCLK | **GPIO 17** | Bit Clock (SCK) |
| | I2S_DOUT | **GPIO 16** | Data Output (SD) |
| **Rotary Encoder** | ENC_A (DT) | **GPIO 4** | Direction Pin A |
| | ENC_B (CLK) | **GPIO 3** | Direction Pin B (*Strapping Pin*) |
| | ENC_BTN (SW) | **GPIO 8** | Encoder Push Button |

---

## 🛠️ Configuration & File Editing

If you need to change the GPIO assignment or modify peripheral settings, all definitions are located in the following header file:
📂 `main/include/gpio.h`

<img width="3456" height="4608" alt="3" src="https://github.com/user-attachments/assets/b73fc86b-66a0-4600-9d94-452d37b1a480" />

<img width="684" height="1281" alt="192 168 1 135_" src="https://github.com/user-attachments/assets/eee5cd54-7270-4a1d-ba4d-7f2777426b00" />


<img width="495" height="679" alt="5" src="https://github.com/user-attachments/assets/36fda851-d568-480f-8b48-8dc73825b02f" />

The flashing bin. are located in the binaries folder.

```cpp
/******************************************************************************
 * * Copyright 2017 karawin ([http://www.karawin.fr](http://www.karawin.fr))
 *
*******************************************************************************/
#pragma once
#ifndef __GPIO_H__
#define __GPIO_H__
#include "nvs_flash.h"
#include "driver/spi_master.h"
#include "esp_adc_cal.h"
#include "app_main.h"
#include "driver/gpio.h"

#define GPIO_NONE 255

#define KSPI SPI2_HOST
#define PIN_NUM_MISO GPIO_NUM_13
#define PIN_NUM_MOSI GPIO_NUM_11
#define PIN_NUM_CLK  GPIO_NUM_12

#define GPIO_LED	GPIO_NONE

// VS1053 — NOT used in this build (Audio via I2S DAC)
#define PIN_NUM_XCS  GPIO_NONE
#define PIN_NUM_RST  GPIO_NONE
#define PIN_NUM_XDCS GPIO_NONE
#define PIN_NUM_DREQ GPIO_NONE

// Encoder knob
#define PIN_ENC0_A   GPIO_NUM_4	// DT / S1
#define PIN_ENC0_B   GPIO_NUM_3	// CLK / S2 (JTAG strapping pin)
#define PIN_ENC0_BTN GPIO_NUM_8	// SW / Key
#define PIN_ENC1_A   GPIO_NONE
#define PIN_ENC1_B   GPIO_NONE
#define PIN_ENC1_BTN GPIO_NONE

// SPI LCD (TFT)
#define PIN_LCD_CS	GPIO_NUM_10	// TFT_CS
#define PIN_LCD_A0	GPIO_NUM_7	// TFT_DC
#define PIN_LCD_RST GPIO_NUM_6  // TFT_RST
#define PIN_LCD_BACKLIGHT	GPIO_NUM_5

// I2S DAC Output
#define PIN_I2S_LRCK GPIO_NUM_18	// I2S_LRC
#define PIN_I2S_BCLK GPIO_NUM_17	// I2S_BCLK
#define PIN_I2S_DATA GPIO_NUM_16	// I2S_DOUT

#define PIN_ADC	GPIO_NONE
#define PIN_TOUCH_CS	GPIO_NONE
#define PIN_AUDIO_SHDN	GPIO_NONE
#define PIN_SLEEP   GPIO_NONE
#define LEVEL_SLEEP   1

// [Functions and prototypes continue below in the original source code...]
#endif
