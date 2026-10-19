# AI Agent Context — Spandan Wearable

> **Read this file first if you are an AI agent picking up this project.**

## What This Project Is

A **health-monitoring police wearable** built on the **Waveshare ESP32-S3-Touch-AMOLED-1.43** board.
It measures heart rate, SpO2, skin temperature, and motion (IMU), displays them on a round AMOLED
touchscreen, logs to SD card, and transmits over BLE to the "Spandan" mobile app.

## Current State

- **Firmware stage:** 7 v3 (status icons + settings touch filter + replay validation + PSRAM queue)
- **Git tag:** `stage7-v3-status-icons`
- **What works:** Display, touch, UI (4 screens: watch face, vitals, system, settings), screen timeout / touch wake, status icons (SD dot, contact line), settings buttons on release with 20px drag filter (#11 fixed), SD queue in PSRAM (~16 KB internal heap saved), SD logging + SYSLOG.TXT, RTC, HR/SpO2 algorithm with FIFO gate, BLE to app with offline buffer replay & corruption validation, inbound TIME sync & AUTH handling
- **What's next:** On-device testing of stage 7 v3 → Power management (Step G) & hardware verification (AUTH token value, CSE time sync test, RTC backup cell)

## Repo Structure

```
Guj_police_Wearable/
├── README.md                           # Detailed operational manual (Claude maintains this)
├── CONTRIBUTING.md                     # Team roles, workflow, git rules
├── AI_CONTEXT.md                       # ← YOU ARE HERE
├── .gitignore
├── Guj_police_Wearable/               # Arduino sketch folder
│   ├── Guj_police_Wearable.ino        # Main firmware (sensors, algorithm, BLE, glue)
│   ├── lcd_bsp.c/h                    # Display + LVGL init
│   ├── lcd_config.h                   # Display/touch pin defines
│   ├── esp_lcd_sh8601.c/h             # QSPI panel driver (unmodified Waveshare)
│   ├── read_lcd_id_bsp.c/h            # SH8601 vs CO5300 auto-detection
│   ├── ui_bsp.c/h                     # 3-screen LVGL watch UI
│   ├── ui_font_xl/l/m/s.c            # Generated fonts (DO NOT EDIT)
│   ├── touch_bsp.cpp/h               # FT3168 touch driver
│   ├── rtc_bsp.cpp/h                 # PCF85063 RTC driver
│   └── sdlog_bsp.c/h                 # SD card logging with FreeRTOS queue
└── docs/
    ├── ARCHITECTURE.md                # FreeRTOS tasks, mutexes, memory, init order
    ├── HARDWARE.md                    # Board specs, pin map, I2C addresses, quirks
    ├── ROADMAP.md                     # Step status, commit history, test results
    ├── BUILD.md                       # Arduino IDE settings, flash guide, troubleshooting
    ├── BLE_PROTOCOL.md                # UUIDs, packet format, app requirements
    ├── KNOWN_ISSUES.md                # Open and resolved bugs with tracking numbers
    └── logs/                          # Place for captured serial/SD logs
```

## Critical Rules

1. **Init order matters:** Display → SD → BLE (display needs largest RAM blocks)
2. **RAM is tight:** Only ~32 KB largest contiguous block after BLE — use PSRAM for big allocations
3. **Mutex `g_i2cMux` is non-recursive:** Never call RTC/touch while holding it → deadlock
4. **BLE UUIDs and name prefix `SVNIT_CRL_`** are registered on the app server — never change
5. **Font files (`ui_font_*.c`)** are generated — never hand-edit
6. **Partition scheme must be "16M Flash (3MB APP/9.9MB FATFS)"** for BLE builds

## How to Continue

1. Read `docs/ROADMAP.md` for what's done and what's next
2. Read `docs/ARCHITECTURE.md` for how the firmware is structured
3. Read `docs/KNOWN_ISSUES.md` for open bugs
4. Make your changes to the sketch files in `Guj_police_Wearable/`
5. Deliver: complete files + `COMMIT_MESSAGE.txt` + `STATUS_REPORT_FOR_ANTIGRAVITY.md`

## GitHub

- **Repo:** https://github.com/DevaamDalal/Guj_police_Wearable
- **Branch:** `main` (single branch, linear history)
- **Owner:** Devaam Dalal ([@DevaamDalal](https://github.com/DevaamDalal))
