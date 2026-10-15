# Build & Flash Guide — Spandan Wearable

## Prerequisites

1. **Arduino IDE 2.3.10** (or later)
2. **ESP32 board package** (Espressif) **3.3.12** — install via Board Manager
3. **LVGL 8.4.0** — installed from the Waveshare package (ships with correct `lv_conf.h`)
   - ⚠️ Do **not** mix LVGL versions or install LVGL from the Arduino Library Manager separately

## Arduino IDE Settings (Tools Menu)

| Setting | Value |
|---------|-------|
| Board | **ESP32S3 Dev Module** |
| USB CDC On Boot | **Enabled** |
| CPU Frequency | 240MHz (WiFi) |
| Flash Mode | **QIO 120MHz** |
| Flash Size | **16MB (128Mb)** |
| Partition Scheme | **16M Flash (3MB APP/9.9MB FATFS)** |
| PSRAM | **OPI PSRAM** |
| Upload Speed | 921600 |
| Everything else | Leave at defaults |

> ⚠️ The partition scheme **must** be "16M Flash (3MB APP/9.9MB FATFS)" from stage 5 onward.
> BLE makes the binary too large for the old "Default 4MB with spiffs" scheme.

## How to Flash

1. Open `Guj_police_Wearable/Guj_police_Wearable.ino` in Arduino IDE
   - The folder name **must** match the `.ino` filename
2. Select the correct COM port
3. Click Upload
4. Open Serial Monitor at **115200 baud** **before** pressing reset to see the full boot banner

## Serial Monitor

- **Baud rate:** 115200
- **Line ending:** "Newline" (needed for the `T...` clock command)
- **Key commands:**
  - `F` — start FIFO diagnostic (30 seconds, no Enter needed)
  - `T2026-10-15 14:30:00` — set RTC time (with Newline line ending)

## SD Card

- Format as **FAT or FAT32** on a PC before first use
- The firmware never formats the card
- Each boot creates `LOG0001.TXT`, `LOG0002.TXT`, etc.
- ~10 rows/s, roughly 10 MB/hour
- Flush interval: every 2 seconds

## Troubleshooting

| Problem | Check |
|---------|-------|
| Boot loop | Look at `heap ...` lines. LVGL draw buffers need ~86 KB. Try reducing `V_RES/10` further |
| "SD not available" | Check card is FAT/FAT32, inserted properly, pins 38-41 |
| Touch not found at boot | Usually works after retry. Planned fix: add retry loop |
| BLE not advertising | Check `USE_BLE 1` and partition scheme is 16M |
| High battery % on USB | Normal — charger raises the voltage. Only trust battery reading on battery power |
| SpO2 reads ~92% healthy | Formula uncalibrated. Will be tuned in later chats |
