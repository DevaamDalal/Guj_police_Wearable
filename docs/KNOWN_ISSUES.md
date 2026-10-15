# Known Issues & Bugs — Spandan Wearable

> Track all known issues here. Reference by number in commit messages and status reports.

---

## Open Issues

### #1 — Touch init flaky at boot
**Severity:** Low (cosmetic — touch works after boot)
**Since:** Stage 4c
**Symptom:** Occasionally prints `FT3168 touch NOT found (0x38)` at boot, but touch works fine afterwards.
**Root cause:** Likely I2C timing at startup, possibly breadboard wiring.
**Fix planned:** Add retry loop to `touch_init()` (small cleanup task).

### #2 — Diagnostic "value changes/s" counter shows 0.0
**Severity:** Low (cosmetic — only affects diagnostic output)
**Since:** Stage 4c (when gate was added)
**Symptom:** The FIFO diagnostic `F` command reports 0.0 for "value changes/s".
**Root cause:** Counter compares consecutive rows, but with the gate on, skipped rows show zero change.
**Fix planned:** Rewrite counter to only compare rows that had new data.

### #3 — SpO2 reads ~92% on healthy skin
**Severity:** Medium (sensor calibration, not a code bug)
**Since:** Always
**Symptom:** SpO2 consistently reads lower than actual. Formula `112.5 - 25*R` is uncalibrated.
**Fix planned:** Later chat — calibrate against reference oximeter after sensors are mounted properly.

### #4 — Battery reads high on USB
**Severity:** Low (known behaviour)
**Since:** Always
**Symptom:** Battery percentage shows inflated values while on USB power.
**Root cause:** Charger raises voltage above battery level. No fuel gauge on board.
**Fix planned:** Document as expected. Later: detect USB power and show "Charging" instead of %.

### #5 — RAM tight after BLE (~51 KB free)
**Severity:** High (blocks new features)
**Since:** Stage 6 v0
**Symptom:** After BLE init, only ~51 KB internal RAM free (largest block ~32 KB).
**Root cause:** BLE stack consumes ~75 KB internal RAM.
**Mitigation:** Use PSRAM for any new large allocations. Monitor heap at boot.

### #6 — GPIO42 (AMOLED_EN) polarity unknown
**Severity:** Low (display works without it)
**Since:** Always
**Symptom:** Cannot use GPIO42 for display power management.
**Fix planned:** Test with oscilloscope/multimeter before implementing display sleep.

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
