# Known Issues & Bugs — Spandan Wearable

> Track all known issues here. Reference by number in commit messages and status reports.

---

## Open Issues

### #3 — SpO2 reads ~92% on healthy skin
**Severity:** Medium (sensor calibration, not a code bug)
**Since:** Always
**Symptom:** SpO2 consistently reads lower than actual. Formula `112.5 - 25*R` is uncalibrated.
**Fix planned:** Later chat — calibrate against reference oximeter after sensors are mounted properly.

### #4 — Battery reads high on USB
**Severity:** Low (known behaviour, now understood from schematic)
**Since:** Always
**Symptom:** Battery percentage shows inflated values while on USB power.
**Root cause:** Schematic confirms the 200k/100k divider on IO4 samples the **VCC rail**, not the battery directly. On USB the voltage reads ~4.6–4.7 V (charger output). No fuel gauge on board. ETA6098 STAT pin only drives LED2 — no GPIO carries charge status or USB presence.
**Mitigation:** Charging bolt (⚡) is now shown on the watch face (stage 7 v0), estimated from the voltage trend. To get exact charge status, a wire from ETA6098 STAT (with series resistor) to a free GPIO (e.g. IO18) would be needed — not planned now.

### #8 — Charging estimate can lag up to 1 minute
**Severity:** Low (cosmetic)
**Since:** Stage 7 v0
**Symptom:** The lightning bolt (charging indicator) on the watch face may appear/disappear with up to ~1 minute delay.
**Root cause:** Charging detection is estimated from voltage trend, not from a hardware charge-status signal. The board has no GPIO connected to ETA6098 STAT.
**Fix planned:** Not urgent. Could be improved with a STAT→GPIO wire (hardware mod).

### #5 — RAM tight after BLE (~51 KB free, improved in 7v3)
**Severity:** Medium (monitored, improved by PSRAM queue)
**Since:** Stage 6 v0 (updated in Stage 7 v2 & Stage 7 v3)
**Symptom:** After BLE init, ~51 KB internal RAM free (largest block ~32 KB).
**Stage 7 v3 mitigation:** The 40-item SD writer queue was moved to PSRAM using `xQueueCreateStatic` (with internal RAM fallback). This saves ~16 KB of internal RAM. Monitor `heap after BLE` at boot and check minfree numbers in `SYSLOG.TXT`.

### #10 — AUTH token value open (waiting for CSE team)
**Severity:** Medium (blocks secured BLE write validation)
**Since:** Stage 7 v1 / v2
**Symptom:** The exact authentication token required for `AUTH:<token>` is not yet specified by the CSE app team.
**Mitigation:** Default token is empty (`""`). In this state, `TIME` sync is accepted whenever the watch clock is untrusted or unset. The token can be set without recompiling via Serial command `A<token>` and is saved in NVS.

### #6 — GPIO42 (AMOLED_EN) polarity unknown
**Severity:** Low (display works without it)
**Since:** Always
**Symptom:** Cannot use GPIO42 for display power management.
**Schematic finding (stage 7 v0):** IO42 goes through R9 which is marked **NC** (not connected). IO42 probably does nothing on this board revision.
**Fix planned:** Verify with multimeter/oscilloscope before implementing display sleep. May be a no-op.

### #9 — Schematic reference findings (stage 7 v0)
**Severity:** Info (recorded for reference)
**Since:** Stage 7 v0 analysis
**Findings:**
- Battery divider: 200k/100k on IO4, samples the VCC rail (not battery directly)
- IO42 (AMOLED_EN): goes through R9 marked NC — probably disconnected
- RTC backup header J2 sits behind diode D1 — check Waveshare wiki for correct cell type
- LED1 = power LED, LED2 = charge LED (driven by ETA6098 STAT)
- ETA6098 STAT only drives LED2 — no GPIO reads charge status

### #7 — RTC backup cell detached
**Severity:** Medium (time lost at power-off)
**Since:** Always
**Symptom:** PCF85063 RTC loses time when board power is removed.
**Fix planned:** Solder a pre-tabbed cell or use a holder. Never solder directly to bare coin cell.

---

## Resolved Issues

### ~#R1~ — Boot loop with SD + display (FIXED stage 3+)
**Fixed in:** `9f11709` (stage 4c)
**Root cause:** Second 108 KB LVGL draw buffer couldn't be allocated after SD driver took internal RAM.
**Fix:** Draw buffers reduced to 46 lines (~43 KB each), display init before SD.

### ~#R2~ — 90% stale FIFO reads (FIXED stage 4c)
**Fixed in:** `9f11709` (stage 4c)
**Root cause:** Main loop at ~200 Hz, MAX30102 at ~25 samples/s → 7.8 of 8 reads were stale.
**Fix:** `MAX_GATE_NEW_SAMPLE 1` — algorithm only runs on new FIFO data.

### ~#R3~ — Touch init flaky at boot (FIXED stage 7 v0)
**Fixed in:** `522c8e1` (stage 7 v0)
**Root cause:** I2C timing at startup.
**Fix:** `touch_init()` now retries up to 6 times with delay.

### ~#R4~ — Diagnostic "value changes/s" counter shows 0.0 (FIXED stage 7 v0)
**Fixed in:** `522c8e1` (stage 7 v0)
**Root cause:** Counter compared consecutive rows; with the gate on, skipped rows showed zero change.
**Fix:** Counter rewritten to only compare rows with new data.

### ~#R5~ — Settings buttons act on press (FIXED stage 7 v3)
**Fixed in:** `a7855fd` (stage 7 v3)
**Root cause:** Buttons triggered on `LV_EVENT_PRESSED`, which caught initial touch of a swipe gesture.
**Fix:** Changed trigger to `LV_EVENT_RELEASED` with a 20 px drag distance threshold. Swipes no longer alter setting values.

### ~#R6~ — Offline replay damaged line resilience (FIXED stage 7 v3)
**Fixed in:** `a7855fd` (stage 7 v3)
**Root cause:** Power loss or abrupt SD removal during logging could leave an incomplete/corrupted tail row in `BLEBUF.TXT`.
**Fix:** Added line validation during replay. Incomplete or malformed rows are skipped and counted; valid packets continue streaming at 80 ms ticks without stalling.
