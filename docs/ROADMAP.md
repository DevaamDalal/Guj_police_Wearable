# Roadmap & Status — Spandan Wearable

> **Current stage:** 7 v3 (status icons + settings touch filter + replay validation)
> **Current tag:** `stage7-v3-status-icons`
> **Last updated:** 2026-10-19

---

## Step Status

| Step | Name | Status | Tag | Notes |
|------|------|--------|-----|-------|
| **A** | Board bring-up, QSPI AMOLED, memory fix | ✅ **DONE** | `stage4c-local` | Display + SD run together without boot loop |
| **B** | FIFO diagnostic | ✅ **DONE** | `stage4c-local` | Root cause: 90% stale reads. `F` command stays in firmware |
| **C** | New-sample gate for algorithm | ✅ **DONE** | `stage4c-local` | `MAX_GATE_NEW_SAMPLE 1`, SpO2 spread improved |
| **D** | BLE with Spandan app | ✅ **DONE** | `stage6-v0-ble` | Connects, reconnects, out-of-range test passed (2026-10-16) |
| **E** | Offline buffering ("blackout buffer") | ✅ **DONE** | `stage7-v0-offline-buffer` | BLEBUF.TXT on SD, replay after reconnect with TS= and BUF=1, verified on device (2026-10-17) |
| **F** | UI polish & settings | ✅ **COMPLETE** (pending on-device check) | `stage7-v3-status-icons` | 4th page (brightness, screen timeout, NVS persist), screen off & touch wake, BT label, charging bolt, SD dot (green/grey/red), contact line (CONTACT/NO CONTACT/etc.). Settings buttons act on release with 20px drag filter (#11 fixed). |
| **G** | Power management | ⬜ **NOT STARTED** | — | Display off/dim (panel DISPOFF in 7v2), lower CPU freq, sleep, verify GPIO42 polarity |

## Small Cleanups (do with next task)

- [x] Touch init retries 6× at boot (fixed in stage 7 v0)
- [x] Print detected display controller (SH8601/CO5300) at boot (fixed in stage 7 v0)
- [x] Fix diagnostic "value changes/s" counter (fixed in stage 7 v0)

## Later Chats (after sensors are off breadboard, one topic per chat)

1. **MAX30102 tuning:** LED current, amplitude limit, window sizes. Capture 3 min rest + 3 min movement, analyse in Python
2. **SpO2 calibration:** compare against reference oximeter. Current formula `112.5 - 25*R` reads ~92% on healthy skin
3. **MLX90614 tuning:** skin temperature for wrist use
4. **RTC backup cell:** solder a holder or pre-tabbed cell, verify with multimeter
5. **Battery voltage:** verify 1/3 divider assumption with multimeter

---

## Commit History

| Hash | Tag | Message |
|------|-----|---------|
| `1e598f7` | — | Step 1: Migrate firmware to ESP32-S3 and swap MPU6050 for QMI8658 |
| `7c62219` | — | docs: Add project context, hardware links, and pinouts to README |
| `9f11709` | `stage4c-local` | Port to Waveshare ESP32-S3-Touch-AMOLED-1.43: display, touch, watch UI, SD log, RTC, FIFO gate (stage 4c) |
| `5e5ab57` | `stage6-v0-ble` | BLE tested with the real Spandan app (stage 6 v0); unit name SVNIT_CRL_60 |
| `8a0b848` | — | repo: Add project docs, AI context, and repo structure |
| `522c8e1` | `stage7-v0-offline-buffer` | Stage 7 v0: offline BLE buffer on SD, charging bolt, boot cleanups |
| `577cac1` | `stage7-v2-settings-eventlog` | Stage 7 v2: screen timeout and brightness, SD event log, time sync test helpers |
| `21ee16d` | — | docs: Update for stage 7 v2 — settings page, SYSLOG.TXT, inbound TIME/AUTH protocol, issues #10 and #11 |
| `5120944` | — | docs: Update BLE_PROTOCOL.md with verified findings from ExponentialGrowth/bletomp app repo |
| `a7855fd` | `stage7-v3-status-icons` | Stage 7 v3: status icons (SD, finger contact), settings buttons on release (#11), replay validation, SD queue in PSRAM |

---

## Test History (on-device results)

| Date | Stage | Result |
|------|-------|--------|
| 2026-10-10 | Stage 1 | Display OK. HR 84–88, SpO2 82–99% (noisy, breadboard) |
| 2026-10-10 | Stage 2 | Touch OK, SpO2 90–100%, HR 88 |
| 2026-10-10 | Stage 3 | RTC valid, SD mounted. **Boot loop** (draw buffer too big) |
| 2026-10-10 | Stage 3 + fix | No crash. SD logs written, ~100 ms rows |
| 2026-10-13 | Stage 4b | FIFO diagnostic: 195 passes/s, 25 new samples/s, 90% empty |
| 2026-10-13 | Stage 4c | Gate works: 25 samples/s to algorithm, HR 63–81, SpO2 84–100% |
| 2026-10-15 | Stage 6 v0 | BLE connects to Spandan app, auto-reconnects. Heap: 51 KB free after BLE |
| 2026-10-16 | Stage 6 v0 | Out-of-range reconnect test passed — Step D fully closed |
| 2026-10-17 | Stage 7 v0 | Offline buffer replay verified (LOG0015/LOG0016). Charging bolt on watch face confirmed |
| 2026-10-18 | Stage 7 v2 | Syntax checked with stubs; SD event log & buffer logic host-tested. Settings page & screen sleep/wake built |
| 2026-10-19 | Stage 7 v3 | Host-tested with damaged BLEBUF.TXT (22 lines replayed, 3 skipped); watch face rendered headless. Pending on-device verification: SD dot, contact line, settings button drag filter, heap after BLE with PSRAM queue |

---

## Memory Budget (stage 6 v0)

| After | Free Internal | Largest Block | Warning |
|-------|--------------|---------------|---------|
| Display | 153 KB | 102 KB | |
| SD | 126 KB | 78 KB | |
| **BLE** | **51 KB** | **32 KB** | ⚠️ Tight — use PSRAM for new buffers |

---

## What Any AI Agent Needs to Know to Continue

1. **Read these docs first:** `docs/ARCHITECTURE.md`, `docs/HARDWARE.md`, this file
2. **The README.md** in the repo root is the detailed operational manual (Claude maintains it)
3. **Deliver code as:** complete files (not diffs) + `COMMIT_MESSAGE.txt` + `STATUS_REPORT_FOR_ANTIGRAVITY.md`
4. **Don't break:** init order (display before SD before BLE), mutex discipline, FreeRTOS task pinning
5. **RAM constraint:** only ~32 KB largest contiguous internal block available — use PSRAM for anything big
6. **Build settings:** Arduino IDE 2.3.10, ESP32 core 3.3.12, LVGL 8.4.0, partition "16M Flash (3MB APP/9.9MB FATFS)"
7. **BLE UUIDs and name prefix `SVNIT_CRL_`** are registered on the app server — do not change them
