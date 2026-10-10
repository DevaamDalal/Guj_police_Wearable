# Spandan: Guj Police Wearable (ESP32-S3 AMOLED)

## Project Overview
This repository contains the firmware for the **Spandan Wearable**, a health-monitoring smartwatch device. The firmware was recently migrated from an ESP32-C6 Supermini to a Waveshare ESP32-S3-Touch-AMOLED-1.43 development board. 

The core objective is to monitor Heart Rate, SpO2, Temperature, and Motion Artifacts, transmit the data via BLE, and display it on the local AMOLED screen.

## Hardware Components
1. **Main Board:** Waveshare ESP32-S3 1.43-inch Touch AMOLED (466x466)
   * [Official Documentation & Resources](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/Resources-And-Documents)
   * [Purchase Link / Specs (Robu.in)](https://robu.in/product/waveshare-esp32-s3-1-43inch-amoled-display-development-board-466x466-qspi-interface-round-display-optional-for-cnc-metal-case-esp32-with-display/?gad_source=1&gad_campaignid=17413441824&gbraid=0AAAAADvLFWfnwXrkaphWaN_YkW8dE-VQp&gclid=CjwKCAjwrP3VBhBbEiwAnaqpQ8iaHN4iJFLKNYGsfjafHcsst47Wq6zySCmAVP8ynxEWmTcbqlVVqRoCRycQAvD_BwE)
2. **Onboard Sensors:**
   * **IMU:** QMI8658 (6-axis Accelerometer & Gyroscope)
   * **RTC:** PCF85063 (Real-Time Clock)
   * **Touch Controller:** FT3168 / FT6146 (Capacitive)
   * **Display Controller:** SH8601 / C05300 (QSPI)
3. **External Sensors (Wired to I2C):**
   * **MAX30102:** Pulse Oximeter & Heart-Rate Sensor
   * **MLX90614 (GY-906):** Non-contact Infrared Temperature Sensor

## Pin Mapping & Architecture
### I2C Bus (Shared)
* **SDA:** GPIO 47
* **SCL:** GPIO 48
*(Used for Touch, QMI8658, RTC, MAX30102, and MLX90614)*

### Interrupts
* **QMI8658 IMU INT:** GPIO 8
* **MAX30102 INT:** GPIO 15 (Configurable via expansion header)

### AMOLED Display (QSPI)
* **CS:** GPIO 9
* **CLK:** GPIO 10
* **D0:** GPIO 11
* **D1:** GPIO 12
* **D2:** GPIO 13
* **D3:** GPIO 14
* **RESET:** GPIO 21
* **ENABLE:** GPIO 42

## Current Firmware State (Step 1 Complete)
* **`Guj_police_Wearable.ino`**: Contains the baseline firmware. 
* **Sensors:** Advanced HR/SpO2 DSP algorithms, AC/DC tracking, motion artifact rejection, and BLE notification logic have been successfully ported over from the previous C6 architecture. 
* **IMU:** Logic updated to use the onboard QMI8658 instead of the old MPU-6050.

## Pending Tasks (Next Steps)
1. **Display Integration:** Initialize the QSPI AMOLED using `Arduino_GFX` or `TFT_eSPI`.
2. **Touch Integration:** Implement the FT3168/FT6146 touch driver.
3. **UI Development:** Build the Graphical User Interface using LVGL (v8 or v9) to visualize HR, SpO2, and Temperature locally on the watch.
