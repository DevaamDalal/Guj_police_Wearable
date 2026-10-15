# Spandan: Guj Police Wearable (ESP32-S3 AMOLED)

Health-monitoring wearable firmware: heart rate, SpO2, skin temperature and motion, shown on a local AMOLED
touchscreen, logged to an SD card, and (later) sent over BLE.

**Last updated:** 2026-10-13 · **Firmware stage:** 4c (local-only: Serial + display + touch + SD log + RTC + watch UI + FIFO diagnostic + new-sample gate) ·
**Next:** BLE bring-up (Step D). Sensors are still on a breadboard; moving them to a proper mount is planned.
**Focus for now:** whole-device functionality first. Sensor tuning and calibration come later, one topic per chat.

---

## 1. Status at a glance

| Area | State | Notes |
|---|---|---|
| MAX30102 HR/SpO2 algorithm | Working. **Stage 4c: runs once per new sensor sample** (was ~8 passes per sample, 90 % stale) | SpO2 still noisy (finger on breadboard, weak pulse), uncalibrated. See sections 6-8 |
| QMI8658 IMU (onboard) | Working | Used for motion rejection |
| MLX90614 skin/ambient temp | Working | I2C max 100 kHz |
| AMOLED display (SH8601 / CO5300, QSPI) | Working (confirmed on device) | LVGL 8, three-screen watch UI confirmed good on device |
| Touch (FT3168, I2C 0x38) | Working (confirmed on device) | Stage 4: swipe left/right between screens. Sensitivity tunable in `lcd_config.h` (`TOUCH_SCROLL_LIMIT`, `TOUCH_SCROLL_THROW`) |
| SD logging | **Working (confirmed):** files written, rows about 100 ms apart. One boot (2026-10-13) reported "SD not available" with no reason shown; later boots fine | Card: SanDisk SU01G, 968 MB, SDSC. Stage 4c prints the failure reason |
| RTC (PCF85063) | Chip works while the board is powered (log timestamps advance correctly). **Backup cell currently detached**, so time is lost at power-off | Set from build time or `T...` command. Fix the cell later (use a holder or a pre-tabbed cell, never solder onto a bare cell) |
| Battery level | Voltage-only estimate | Board has **no fuel gauge** |
| Memory fix (display + SD together) | **Confirmed:** no crash, display and SD run together | Previous build boot-looped, see section 7 |
| BLE | Disabled (`USE_BLE 0`), code kept | Next major stage |
| Real watch UI | **Working (confirmed on device)** | Watch face, vitals, system page, swipe navigation |
| FIFO diagnostic | **Done, found the root cause of the stale-data problem** (2026-10-13) | Serial key `F`, see section 5 |
| App compatibility (BLE) | **Requirements known** (CSE reply 2026-10-12) | See Step D |

---

## 2. Hardware

* **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.43 (ESP32-S3R8: dual core, 16 MB flash, 8 MB OPI PSRAM)
  [Docs](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/Resources-And-Documents) ·
  [Arduino page](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/Arduino)
* **Display:** 1.43" round AMOLED 466x466, controller SH8601 **or** CO5300 depending on hardware revision
  (auto-detected at boot by `read_lcd_id_bsp.c`), QSPI.
* **Touch:** FT3168 / FT6146, I2C.
* **Onboard:** QMI8658 IMU, PCF85063 RTC (own battery connector), TF/SD slot, battery connector with
  charger (ETA6098) and a voltage divider on GPIO4. **No fuel gauge.**
* **Current test rig:** MAX30102 and GY-906 are still on a breadboard (loose contact can look like motion in the log).
* **External sensors**, wired with 4 wires (GND, 3V3, SDA, SCL) to the board's 4-pin SH1.0 I2C connector, which
  is on the same SDA/SCL net as GPIO47/48:
  * MAX30102 (PPG: HR + SpO2)
  * MLX90614 / GY-906 (IR temperature)
* **No interrupt lines are used.** All sensors are polled (the old GPIO8 / GPIO15 interrupt setup was removed).

### Pin map

| Function | GPIO |
|---|---|
| I2C SDA / SCL (touch, IMU, RTC, MAX30102, MLX90614) | 47 / 48 |
| Display QSPI: CS, CLK, D0, D1, D2, D3 | 9, 10, 11, 12, 13, 14 |
| Display RESET | 21 |
| Display EN (AMOLED_EN) | 42 (**not driven**, see section 8) |
| SD card (SPI): CS, MOSI, MISO, CLK | 38, 39, 40, 41 |
| Battery ADC (through a 1/3 divider) | 4 |
| IMU INT (unused) | 8 |

### I2C addresses (100 kHz bus)

MAX30102 `0x57` · QMI8658 `0x6B` · MLX90614 `0x5A` · FT3168 touch `0x38` · PCF85063 RTC `0x51`

---

## 3. Build setup (what works)

### Arduino IDE settings (Tools menu)

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** |
| USB CDC On Boot | **Enabled** |
| CPU Frequency | 240MHz (WiFi) |
| Flash Mode | **QIO 120MHz** |
| Flash Size | **16MB (128Mb)** |
| Partition Scheme | Currently **Default 4MB with spiffs** (works now). **Switch to "16M Flash (3MB APP/9.9MB FATFS)" before enabling BLE.** |
| PSRAM | **OPI PSRAM** |
| Upload Speed | 921600 |
| Other items (USB Mode, Core Debug Level, Events/Arduino core, etc.) | Leave at their defaults |
| Serial Monitor | **115200 baud**. For the clock command `T...` select "Newline". The `F` key needs no Enter setting. |

Software: Arduino IDE **2.3.10**, ESP32 core (Espressif) **3.3.12**, LVGL **8.4.0** installed from the Waveshare package
(it ships the `lv_conf.h` the demos use). Do not mix LVGL versions.

Fonts: the UI uses its own generated fonts (`ui_font_*.c`), so it does not depend on the `lv_conf.h` font settings.
No Arduino SD library is used (SD goes through ESP-IDF FATFS).

**Flashing:** open `Guj_police_Wearable/Guj_police_Wearable.ino` (keep the folder name), select the COM port, upload.
Open the Serial Monitor **before** pressing reset to see the whole boot text. The boot banner says which firmware stage runs.

---

## 4. Code map (`Guj_police_Wearable/`)

| File | What it is | Origin |
|---|---|---|
| `Guj_police_Wearable.ino` | Sensors, HR/SpO2 algorithm, logging glue, RTC/battery helpers, `setup()`/`loop()` | Your firmware v14 + changes (BLE under `USE_BLE`) |
| `lcd_bsp.c/.h`, `lcd_config.h` | Display + LVGL + touch input glue. Draw buffers are now 46 lines (`V_RES/10`) | Waveshare `07_LVGL_Test`, modified |
| `esp_lcd_sh8601.c/.h`, `read_lcd_id_bsp.c/.h` | Panel driver, controller ID read | Waveshare, **unmodified** |
| `ui_bsp.c/.h` | Three swipeable screens (watch face, vitals, system), thread-safe value hand-off | New (rewritten in stage 4) |
| `ui_font_xl.c`, `ui_font_l.c`, `ui_font_m.c`, `ui_font_s.c` | Generated Montserrat fonts (110/64/28/20 px) so the UI does not depend on `lv_conf.h` font settings | Generated with `lv_font_conv`, do not edit |
| `touch_bsp.cpp/.h` | FT3168 over the shared `Wire` bus (not Waveshare's legacy I2C driver) | New |
| `rtc_bsp.cpp/.h` | PCF85063 get/set | New |
| `sdlog_bsp.c/.h` | SD mount, numbered log files, background writer task, secondary file for the FIFO diagnostic | New (pattern from Waveshare SD demo) |

### Runtime architecture

* **`loop()` task (core 1):** polls IMU, MLX90614 and MAX30102, runs the HR/SpO2 algorithm, pushes values to the
  screen (vitals 4 Hz, clock and battery 2 Hz) and the SD logger (10 Hz). This is the validated algorithm path; its timing must not be disturbed.
* **LVGL task (core 0, priority 2, 8 KB stack):** renders, reads touch.
* **SD writer task (core 0, priority 1, 6 KB stack):** drains a 40-line queue to the card. Never blocks the loop; drops
  lines if the queue is full (counted and shown on screen).
* **Locks:** one mutex (`g_i2cMux`) for the shared I2C bus (sensors vs touch vs RTC; non-recursive, never call RTC/touch
  functions while holding it). The screen data uses a try-lock, so the sensor loop never waits for the display.
* **Setup order matters:** I2C and sensor init, touch init, **display**, then RTC and SD (the display needs the biggest
  free RAM blocks).

---

## 5. Using it

### Screens (swipe left / right; three dots at the bottom show the page)
1. **Watch face:** time (RTC), date, seconds ring, battery icon and %, small HR / SpO2 line.
2. **Vitals:** large HR, SpO2, skin temperature, status (OK / WARMUP / NO_CONTACT / MOTION / IMU_MOTION). The ring is
   green when OK, grey for no contact / starting, amber otherwise.
3. **System:** accel, gyro, IR/RED DC, last touch, battery (V and ~%), SD file name and row count, RTC time.
Only the visible screen is refreshed. The screen stays black where nothing is drawn (AMOLED pixels off).
The battery % is a voltage estimate and reads high on USB / while charging.

### FIFO diagnostic (Serial command `F`)
Tests the hypothesis that `loop()` reads the MAX30102 FIFO far more often than the sensor produces samples.
1. Put a finger on the sensor and keep still. Open the Serial Monitor (115200), type `F` and press Send (Enter not required).
2. It records every loop pass for 30 s (FIFO write pointer, overflow counter, read pointer, IR, RED), prints a
   summary with a one-line verdict, and saves `DIAGnnnn.CSV` to the SD card (if present).
3. Summary fields: loop passes/s, new samples/s (sensor), samples consumed/s, value changes/s, % empty passes, max backlog,
   overflow counter.
4. Upload the Serial text and the `DIAG000x.CSV` for analysis. Normal HR/SpO2 processing continues during the capture;
   the extra pointer read costs about 0.3 ms per pass.
Rows are written to the card in small chunks after the capture, so the sensor loop is not stalled.
A `[alive Ns] ST=... diag=... sd=...` line prints every 5 s so the Serial Monitor shows the sketch is running even without a finger.
Known cosmetic bug: "value changes/s" prints 0.0 with the gate on (it compares consecutive rows, skipped rows are zero). Ignore it.

### Serial output
Same `key=value` line as before, once per second when the signal is valid, plus status lines.
`TOUCH x y` is printed on each press (set `TOUCH_SERIAL_DEBUG 0` in `touch_bsp.h` to silence).
At boot you also get two heap lines (free memory after display, after SD).

### Setting the clock
On first run (or if the RTC lost power / is behind the build time) it is set to the firmware build time.
For an exact time, send in the Serial Monitor (with "Newline" selected): `T2026-10-10 13:20:11`

### SD log
* Card: **FAT/FAT32**, formatted on a PC. The firmware never formats the card.
* Each boot creates the next `LOG0001.TXT`, `LOG0002.TXT`, ... (short 8.3 names).
* 10 rows/s (`LOG_PERIOD_MS` = 100), roughly 10 MB/hour. Flushed to the card every 2 s.
* First line is a `#` header (start RTC, build time, period, battery voltage).
* Each row uses the same fields as the Serial line, prefixed with `MS`, `RTC`, `VBAT`:

```
MS=,RTC=,VBAT=,FIFO_IR=,FIFO_RED=,S1_IR=,S1_RED=,IR_DC=,IR_AC=,RED_DC=,RED_AC=,HR=,SpO2=,ObjT=,AmbT=,
accX=,accY=,accZ=,accMag=,accMagF=,gMagF=,gX=,gY=,gZ=,ST=,Q=
```

---

## 6. Test history (real results)

| Date | Build | Result |
|---|---|---|
| 2026-10-10 | Stage 1 (display + vitals) | Display OK. HR 84-88 bpm. SpO2 82.2-99.1 % over 21 readings (SD 5.5) while HR stayed within 4 bpm. |
| 2026-10-10 | Stage 2 (touch + pages) | Touch OK (`TOUCH 127 250` ... `266 247`). SpO2 90.4-100 %, HR 88.1-88.2. |
| 2026-10-10 | Stage 3 (SD + RTC + battery) | RTC valid. SD mounted (SU01G, SDSC, 968 MB, 10 MHz), `LOG0001.TXT` created. **Boot loop:** `assert failed ... lcd_bsp.c:102 (buf2)`. |
| 2026-10-10 | Stage 3 + memory fix | **No crash.** SD log files written; rows are about 100 ms apart. Contact transitions (NO_CONTACT, WARMUP, OK) and SpO2 95.1-95.7 % at rest appear in the log. VBAT read 3.68-3.69 V (see section 7). |
| 2026-10-11 | Stage 4 (watch UI + FIFO diagnostic) | Built; UI layout verified in a PC simulation (found and fixed a startup null-pointer crash there). **On-device test pending.** |
| 2026-10-13 | Stage 4b: FIFO diagnostic (2 runs, finger on sensor) | **loop passes/s 195, new sensor samples/s 25, empty-FIFO passes 90 %, no overflow, max backlog 1.** So about 7.8 of every 8 passes fed stale data into the HR/SpO2 algorithm. The v14 firmware did the same (`maxDataReady` was set by the interrupt but never used to gate), so the algorithm constants were tuned on this behaviour. SD did not mount on this boot (no error text was available). |
| 2026-10-13 | Stage 4c: new-sample gate | Algorithm now runs only when the FIFO has a new sample (`MAX_GATE_NEW_SAMPLE`, set 0 to revert). `SPO2_MIN_WINDOW_SAMPLES` 18 to 50 under the gate. **Needs on-device comparison.** |
| 2026-10-13 | Stage 4c on device | Gate works: loop 224 passes/s, **25.0 samples/s fed to the algorithm**, 25 new/s, SD saved `DIAG0001/2.CSV`. HR 63-81, SpO2 84-100 % (spread 3.7). Pulse about 0.26-1 % of DC with strong slow drift (breadboard/finger). Touch OK after boot (check at boot reported not found once). |

---

## 7. Known issues and fixes

1. **Boot loop with SD enabled (fixed, confirmed 2026-10-10).** The second 108 KB LVGL draw buffer could not be
   allocated after the SD driver, log queue and tasks had used internal RAM. Fix: draw buffers reduced to 46 lines
   (about 43 KB each) and the display is initialised before SD. If it still fails, send the two `heap ...` lines.
2. **SpO2 noise: root cause found (2026-10-13), partly fixed in 4c.** Diagnostic (2 runs, 30 s each): loop 195-224 passes/s,
   25 new samples/s, **89-90 % of passes read an empty FIFO**, no overflow, max backlog 1. The v14 firmware did the same
   (`maxDataReady` was set by the interrupt but never used), so the algorithm constants were tuned on this behaviour.
   Stage 4c runs the algorithm only when a sample is waiting (`MAX_GATE_NEW_SAMPLE`, 0 reverts) and raises
   `SPO2_MIN_WINDOW_SAMPLES` 18 to 50. After the gate: HR 63-81 bpm, SpO2 84-100 % (spread 3.7, was 4.9).
   Offline analysis of the raw 25 sps data: pulse amplitude only 0.26-1 % of DC and slow drift 3.5-4x the pulse (finger
   pressure, breadboard), so remaining noise is mostly signal quality. SpO2 also reads about 92 % on a healthy finger because the
   formula `112.5 - 25*R` is uncalibrated.
3. **Battery reading looked low (3.68-3.69 V, about 10 % on the curve).** Check with a multimeter that the
   1/3 divider assumption is right, and whether a battery is actually connected while on USB.
   **Battery percentage is an estimate** from a resting-LiPo voltage curve, and reads high while charging or on USB.
   Assumes the 1/3 divider from Waveshare's ADC demo.
4. **GPIO42 (display EN) is left alone.** Waveshare's demos never drive it and the display works; its polarity could not
   be confirmed from the schematic. Only revisit it for display power-down, after checking polarity.
5. **RTC from build time is only approximate** (and an unchanged sketch may reuse an old build time). Use the `T...` command.
6. **Not compiled against the real ESP-IDF here.** The SD/display C files were checked only against stubs; the first
   real compile happens on your PC. Report any compiler error and it gets fixed.

7. **Touch check at boot is flaky:** one boot printed `FT3168 touch NOT found (0x38)` although touch worked afterwards. Probably
   breadboard I2C wiring or a start-up delay. Make the check retry (planned).
8. **Hard-coded values to remember** (all candidates for the later tuning chats):
   * MAX30102: LED current 0x19 (about 5 mA each LED), 25 samples/s (200 sps, averaging 8), 18-bit range 4096 nA.
   * Algorithm: SpO2 formula `112.5 - 25*R`; `WRIST_R_MIN/MAX` 0.4-1.3; `MIN_SIGNAL_AMPLITUDE` 45 counts (close to the real pulse of about 80 counts);
     `DC_ALPHA` 0.985; `MAX_RAW_CHANGE`; `MIN_SKIN_IR` 6000; warm-up and window sample counts (now meaning real samples, so about 8x longer in time);
     gyro motion limit 45 dps and flush counts; HR timeouts (8 s, hold 5 s).
   * Battery: assumed divider 1/3 and a generic LiPo percentage table. RTC set from build time. Log rate 10 rows/s.
   * Board: I2C pins and 100 kHz, SD pins and 10 MHz, display buffer 46 lines, GPIO42 not driven.
   * BLE (when enabled): name `SVNIT_CRL_16` and both UUIDs must not change (the app depends on them).

---

## 8. Plan: what to do next (in order)

### Step A: Confirm the memory fix (DONE 2026-10-10)
Flash the current folder. Pass when: boots without a reboot loop, display and touch work, Serial shows
`SD: logging to /sd_card/LOG000x.TXT` and two heap lines, page 2 shows an increasing row count.
Result: boots cleanly, SD files contain rows at about 10 per second.

### Step B: FIFO diagnostic (DONE 2026-10-13)
Result in section 7, item 2. The `F` diagnostic stays in the firmware for later checks.

### Step C: Gate the algorithm on new samples (DONE in stage 4c, needs ongoing comparison)
Implemented (`MAX_GATE_NEW_SAMPLE`). Detailed sensor tuning is deliberately postponed (see "Later chats" below).

### Step D: BLE back on
1. Partition scheme to "16M Flash (3MB APP/9.9MB FATFS)".
2. `#define USE_BLE 1`; confirm the BLE code builds on core 3.3.12 (the BLE API differs from 2.x).
3. Re-check free memory with BLE running (draw buffers can shrink further if needed).
4. **App requirements (CSE team reply, 2026-10-12):** keep name `SVNIT_CRL_XXX` (registered on the server); keep the service/characteristic UUIDs
   (`4fafc201-...914b` / `beb5483e-...26a8`, already in the sketch), NOTIFY mandatory, WRITE optional (`AUTH:<token>` handshake, not needed yet).
   The app parses `HR`, `SpO2`, **`Temp`** (not `ObjT`) and **`Batt`**; field order is free (split on `[,;|]` and `[:=]`); `--` is treated as 0 /
   calibrating; extra fields are forwarded to the server (IMU values feed their AI models). Rate 1 Hz; keep the BLE packet **under ~200 characters**
   (the old line is about 380). No bonding. `DISCONNECT` / `SHUTDOWN` in a packet = clean shutdown.
5. **BLE payload plan:** a separate compact string for BLE (SD log and Serial keep the full line), e.g.
   `HR=72.0,SpO2=98.0,Temp=36.5,Batt=85,accX=..,accY=..,accZ=..,gX=..,gY=..,gZ=..,ST=OK` (about 110-130 chars). Rename `ObjT` to `Temp`, add `Batt`
   (percent estimate, no fuel gauge). Decide with the team whether raw `FIFO_IR/FIFO_RED` should also be sent. JSON/binary is welcome later, added alongside the old format.
6. Test: connect, receive notifications, disconnect/reconnect, with display and SD running.

### Step E: Offline buffering ("blackout buffer")
Use the SD card instead of the LittleFS ring buffer planned earlier (no flash-wear concern): store data while BLE is
disconnected, send or upload later. Needs a design decision on format and replay.

### Step F: UI polish
Done: watch face, vitals, system page, swipe navigation. Remaining: Bluetooth status icon, brightness / screen timeout
(AMOLED burn-in and battery), SD and contact icons.

### Step G: Power
Display off/dim, lower CPU frequency, log rate options, sleep behaviour; verify GPIO42 polarity before using it.

### Small cleanups (do together with Step D)
* Make the touch check at boot retry; fix the diagnostic "value changes/s" counter; print the detected display controller at boot.

### Later chats (one topic per chat, after the sensors are off the breadboard)
1. MAX30102 tuning: LED current, amplitude limit, window sizes, on-wrist captures (3 min rest, 3 min movement) with Python analysis.
2. SpO2 calibration against a reference oximeter; HR validation.
3. MLX90614 skin temperature tuning for wrist use.
4. Fix the RTC backup cell (holder or pre-tabbed cell), multimeter check of the battery voltage and divider.

### Housekeeping
* Commit this folder and README to the repo (suggested tag: `stage4c-local`).
* Print the detected display controller (SH8601 / CO5300) at boot for the record.
* Keep this README updated after each step.

---

## 9. Working rules (project conventions)

1. Flash, capture serial/SD log, upload `.ino` + log, analyse, **then** change code.
2. Prefer working reference code to integrate over writing algorithms from scratch.
3. Changes are delivered as isolated diffs where possible; the sketch folder in this repo is always the full current state.
4. Algorithm constants in the `.ino` are tuned from field data; do not change them without analysis.

## 10. Reference documents

Waveshare board datasheet and schematic, SH8601 datasheet, FT3168, PCF85063, QMI8658C and ESP32-S3 datasheets, and the
Waveshare Arduino examples (`07_LVGL_Test`, `04_SD_Card`, `02_I2C_PCF85063`, `01_ADC_Test`) were used as the basis for the
drivers in this folder.
