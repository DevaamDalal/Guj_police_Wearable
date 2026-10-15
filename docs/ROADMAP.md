# Roadmap & Status — Spandan Wearable

> **Current stage:** 6 v0 (BLE bring-up)
> **Current tag:** `stage6-v0-ble`
> **Last updated:** 2026-10-15

---

## Step Status

| Step | Name | Status | Tag | Notes |
|------|------|--------|-----|-------|
| **A** | Board bring-up, QSPI AMOLED, memory fix | ✅ **DONE** | `stage4c-local` | Display + SD run together without boot loop |
| **B** | FIFO diagnostic | ✅ **DONE** | `stage4c-local` | Root cause: 90% stale reads. `F` command stays in firmware |
| **C** | New-sample gate for algorithm | ✅ **DONE** | `stage4c-local` | `MAX_GATE_NEW_SAMPLE 1`, SpO2 spread improved |
| **D** | BLE with Spandan app | ✅ **DONE** (1 pending test) | `stage6-v0-ble` | Connects, reconnects, 95–99 char packets. **Pending: out-of-range test** |
| **E** | Offline buffering ("blackout buffer") | ⬜ **NOT STARTED** | — | Store data on SD while BLE disconnected, replay later |
| **F** | UI polish | 🟡 **PARTIAL** | — | BT label done. Remaining: screen timeout, SD/contact icons, brightness |
| **G** | Power management | ⬜ **NOT STARTED** | — | Display off/dim, lower CPU freq, sleep, verify GPIO42 polarity |

## Small Cleanups (do with next task)

- [ ] Touch init retry at boot (currently flaky, sometimes says "not found")
- [ ] Print detected display controller (SH8601/CO5300) at boot
- [ ] Fix diagnostic "value changes/s" counter (shows 0.0 with gate on — cosmetic)

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
