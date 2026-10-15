# Firmware Architecture — Spandan Wearable

> This document is the single source of truth for how the firmware is structured.
> Any AI agent working on this project should read this file first.

## Runtime Task Model (FreeRTOS, dual-core)

```
┌─────────────────────────────────────────────────────────────┐
│                      ESP32-S3 (240 MHz)                     │
│                                                             │
│  ┌─────────────────────┐   ┌──────────────────────────────┐ │
│  │      CORE 0         │   │          CORE 1              │ │
│  │                     │   │                              │ │
│  │  LVGL Task          │   │  loop() — Sensor Task        │ │
│  │   prio 2, 8 KB      │   │   (Arduino main loop)       │ │
│  │   - lv_timer_handler│   │   - IMU poll (QMI8658)       │ │
│  │   - touch read      │   │   - MAX30102 FIFO read       │ │
│  │   - screen render   │   │   - HR/SpO2 algorithm        │ │
│  │                     │   │   - MLX90614 temp read       │ │
│  │  SD Writer Task     │   │   - BLE notify (1 Hz)        │ │
│  │   prio 1, 6 KB      │   │   - push data to UI/SD/BLE  │ │
│  │   - drain 40-item   │   │   - Serial output (1 Hz)     │ │
│  │     queue to card   │   │                              │ │
│  │   - flush every 2s  │   │                              │ │
│  └─────────────────────┘   └──────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## Mutex Map

| Mutex | Type | Protects | Held by | Critical Notes |
|-------|------|----------|---------|----------------|
| `g_i2cMux` | Binary, **non-recursive** | I2C bus (Wire) | Sensor loop, touch task, RTC calls | ⚠️ **DEADLOCK RISK**: Never call RTC or touch functions while already holding this mutex |
| `g_mux` | Binary | UI data (`g_uiXxx` variables) | Sensor loop (writer), LVGL task (reader) | Reader uses `xSemaphoreTake(..., 0)` (non-blocking try-lock) — sensor loop never waits for display |
| `lvgl_mux` | Binary | LVGL API calls | LVGL task, UI setter functions | All `lv_*` calls must be inside this lock |

## Memory Layout (measured on device, stage 6 v0)

| Phase | Free Internal RAM | Largest Free Block |
|-------|------------------|--------------------|
| After display init | ~153 KB | ~102 KB (DMA-capable) |
| After SD init | ~126 KB | ~78 KB |
| **After BLE init** | **~51 KB** | **~32 KB** |

> ⚠️ **RAM is tight with BLE.** Any new feature must keep allocations small and use PSRAM for large buffers (8 MB OPI available).

### LVGL Draw Buffers
- Reduced to `V_RES / 10` = 46 lines (~43 KB each, 2 buffers)
- Must be allocated **before** SD init (needs the largest contiguous DMA blocks)
- If boot-loops: check the two `heap ...` lines in Serial output

## Init Order (setup())

```
1. Serial (115200)
2. Wire.begin(SDA=47, SCL=48, 100kHz)
3. QMI8658 IMU init
4. MAX30102 init (LED current, FIFO config)
5. MLX90614 verify
6. touch_init()
7. lcd_lvgl_Init()        ← allocates LVGL draw buffers + creates UI
8. RTC init (PCF85063)    ← after display (smaller allocation)
9. sdlog_begin()          ← after display (smaller allocation)
10. BLE init (if USE_BLE) ← last, consumes ~75 KB internal RAM
11. Start LVGL task on Core 0
```

> **Order matters!** Display must init before SD and BLE to get the large DMA blocks.

## File Dependency Graph

```
Guj_police_Wearable.ino
 ├── lcd_bsp.c/h              ← display + LVGL init
 │    ├── esp_lcd_sh8601.c/h  ← QSPI panel driver
 │    ├── read_lcd_id_bsp.c/h ← SH8601 vs CO5300 detection
 │    ├── lcd_config.h        ← pin defines, buffer sizes
 │    └── ui_bsp.c/h          ← 3-screen LVGL UI
 │         └── ui_font_*.c    ← 4 generated Montserrat fonts (do not edit)
 ├── touch_bsp.cpp/h          ← FT3168 I2C touch driver
 ├── rtc_bsp.cpp/h            ← PCF85063 RTC driver
 └── sdlog_bsp.c/h            ← SD card logging (FreeRTOS queue writer)
```

## BLE Architecture (stage 6 v0)

- **Conditional:** `#define USE_BLE 1` (set 0 to disable, saves ~75 KB RAM)
- **Stack:** ESP-IDF NimBLE via Arduino BLE library
- **Name:** `BLE_DEVICE_NAME` define (currently `"SVNIT_CRL_60"`)
- **Service UUID:** `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- **Characteristic UUID:** `beb5483e-36e1-4688-b7f5-ea07361b26a8` (NOTIFY)
- **MTU:** 517 requested
- **Packet:** ~95–99 chars, 1 Hz, format: `HR=72.0,SpO2=98.0,Temp=36.5,Batt=85,accX=..,accY=..,accZ=..,gX=..,gY=..,gZ=..,ST=OK`
- **Missing values:** sent as `--` (app treats as 0 / calibrating)
- **Reconnect:** re-advertises after disconnect; auto-reconnect after reboot confirmed

## FIFO New-Sample Gate (stage 4c fix)

**Problem:** Main loop ran at ~200 Hz but MAX30102 produces only ~25 samples/s → 89–90% of FIFO reads returned stale data, each sample processed ~8× through the algorithm.

**Fix:** `#define MAX_GATE_NEW_SAMPLE 1` — algorithm only runs when FIFO write pointer has advanced. Set to 0 to revert to old behaviour.

**Side effect:** Window-based algorithm constants now count *real samples* (25/s), so time durations are ~8× longer. `SPO2_MIN_WINDOW_SAMPLES` raised from 18 → 50 to compensate.

## Key #defines (all in .ino unless noted)

| Define | Value | Purpose |
|--------|-------|---------|
| `BLE_DEVICE_NAME` | `"SVNIT_CRL_60"` | BLE advertised name (change per unit, keep `SVNIT_CRL_` prefix) |
| `FW_LABEL` | `"stage 6 v0 (BLE)"` | Boot banner label |
| `USE_BLE` | `1` | Enable/disable BLE stack |
| `MAX_GATE_NEW_SAMPLE` | `1` | Gate algorithm on new FIFO samples |
| `SPO2_MIN_WINDOW_SAMPLES` | `50` | Min samples before SpO2 calculation |
| `LOG_PERIOD_MS` | `100` | SD log rate (10 rows/s) |
| `SDA_PIN` / `SCL_PIN` | `47` / `48` | I2C bus pins |
| `TOUCH_SCROLL_LIMIT` | (in `lcd_config.h`) | Touch sensitivity |
