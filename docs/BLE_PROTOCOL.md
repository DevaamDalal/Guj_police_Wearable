# BLE Protocol — Spandan Wearable

## Connection Parameters

| Parameter | Value |
|-----------|-------|
| Device Name | `SVNIT_CRL_60` (change per unit via `BLE_DEVICE_NAME` define; keep `SVNIT_CRL_` prefix) |
| Service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Characteristic UUID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |
| Characteristic Property | **NOTIFY, WRITE, WRITE WITHOUT RESPONSE** |
| MTU | 517 requested |
| Rate | 1 packet per second |
| Max Packet Size | ~140–175 characters (< 200 chars limit) |

> ⚠️ **UUIDs and name prefix are registered on the Spandan app server — do not change.**
> 📱 **Mobile App Repository:** [ExponentialGrowth/bletomp](https://github.com/ExponentialGrowth/bletomp) (Flutter app in `ble_to_mp/mobile_app`, Node.js backend in `ble_to_mp/server`)

## Packet Format (Stage 7 v1 / v2)

Plain text, comma-separated key=value pairs:

```
HR=72.0,SpO2=98.0,Temp=36.5,BatPct=85,BatV=4.19,CHG=0,accX=0.12,accY=-0.05,accZ=9.81,gyroX=1.2,gyroY=-0.8,gyroZ=0.3,ST=OK,TS=2026-10-18T12:12:00+05:30
```

### Fields

| Key | Type | Description | Missing / Special |
|-----|------|-------------|-------------------|
| `HR` | float | Heart rate (bpm) | `--` |
| `SpO2` | float | Blood oxygen (%) | `--` |
| `Temp` | float | Skin temperature (°C) | `--` |
| `BatPct` | int | Battery level (%). ⚠️ **Use `BatPct` (or `Battery` / `Bat`)**. Verified from `main.dart:2579`: app checks `batpct`, `battery`, `bat`; does NOT match `Batt`. Server (`input_receiver.js:156`) checks `BatPct` or `Battery`. | `--` |
| `BatV` | float | Battery voltage (V, e.g. `4.19`) | `--` |
| `CHG` | int | Charging state (`1` = charging estimate, `0` = discharging) | `0` |
| `accX`, `accY`, `accZ` | float | Accelerometer (g). Matches mobile app and server. | `--` |
| `gyroX`, `gyroY`, `gyroZ` | float | Gyroscope (dps). ⚠️ **Use `gyroX/Y/Z`**. Mobile app accepts `gyrox` or `gx`, but backend server specifically parses `gyroX/Y/Z`. | `--` |
| `ST` | string | Status code (`OK`, `WARMUP`, `NO_CONTACT`, `MOTION`, `IMU_MOTION`) | `--` |
| `TS` | string | ISO timestamp with timezone (e.g. `2026-10-18T12:12:00+05:30`). Sent on every packet (live & buffered) | `--` |
| `TSQ` | int | Clock quality flag: `0` indicates clock is untrusted/not synced from phone. Key is omitted once synced | (omitted when synced) |
| `BUF` | int | `1` if this is a replayed offline-buffered packet | (omitted on live packets) |
| `IR`, `Red` | float | (Optional) Raw PPG values; supported by app `unifiedParse` | (optional) |

### Status Codes (`ST`)

| Code | Meaning |
|------|---------|
| `OK` | Valid readings, finger on sensor |
| `WARMUP` | Sensor warming up after contact |
| `NO_CONTACT` | No finger detected |
| `MOTION` | Motion artifact rejection active |
| `IMU_MOTION` | IMU detected excessive movement |

### Notes

- Field order is free (app parses by key name)
- `--` is treated as 0 / "calibrating" by the app
- Extra fields are forwarded to the server (IMU values feed their AI models)
- Packet is always sent even without contact (missing values use `--`)

## Inbound BLE Commands (App → Watch via WRITE on Characteristic)

The app (or nRF Connect) can send commands as plain UTF-8 text strings to the characteristic:

### 1. `AUTH:<token>`
- Authentication token handshake.
- The watch compares the token against `BLE_AUTH_TOKEN` (stored in NVS).
- Can also be set/updated without recompiling via Serial command `A<token>`.
- Token is currently empty in firmware until the CSE team specifies the exact value.

### 2. `TIME:<unix_epoch_seconds_utc>,<timezone_offset_minutes>`
- Example: `TIME:1792305720,330` (+05:30 IST = 330 minutes).
- The watch updates its PCF85063 RTC and stores the timezone offset in NVS.
- **Trust condition:** Accepted if `AUTH` has matched, or if `BLE_AUTH_TOKEN` is currently empty and the clock was previously untrusted (`TSQ=0`, e.g. after battery cut or set from build-time default).
- After sync, `TSQ=0` is removed from packets, the `(not synced)` indicator disappears from the system screen, and `# TIME SYNC` is logged to `SYSLOG.TXT`.
- Testing: Can be tested via nRF Connect (write text `TIME:<epoch>,330`) or Serial commands (`U` marks clock untrusted for re-testing, `W` prints time info).

## App Requirements (CSE team replies: 2026-10-12 & 2026-10-18)

- **Name format:** `SVNIT_CRL_XXX` (registered on server, number changes per unit)
- **Characteristic:** NOTIFY for data packets, WRITE / WRITE WITHOUT RESPONSE for `AUTH` and `TIME`
- **Time sync:** App sends `AUTH:<token>` then `TIME:<epoch>,<offset min>`
- **TS format:** ISO 8601 with timezone (e.g. `2026-10-18T13:00:00+05:30`) on every packet
- **CHG & BatV:** App parses `CHG` (`0`/`1`) and `BatV` (voltage)
- **Replay pacing:** 50–100 ms between replayed buffered packets (watch sends at 100 ms / 10 pkts/s)
- **Server handling:** Server stores `BUF=1` packets under their `TS`; app hides `BUF=1` packets from the live screen
- 1 Hz rate, packet length < 200 characters
- No bonding required

## Offline Buffer (stage 7 v0)

When the BLE app is not connected:
1. Each 1 Hz packet is appended to `BLEBUF.TXT` on the SD card with `TS=<RTC>` and `BUF=1`
2. File survives reboot; 20 MB cap (oldest data dropped if exceeded)
3. All SD work happens in the writer task (no extra RAM allocation beyond a 260 B line buffer)

When the app reconnects:
1. Stored packets are replayed **oldest first** at 10 packets/second, interleaved with live packets
2. If the link drops during replay, replay pauses and resumes on next connect
3. After all buffered packets are sent, `BLEBUF.TXT` is deleted
4. Serial prints `BLE replay: sending stored packets` and `BLE replay done`
5. System page shows `buf N` (count of buffered packets waiting)

## Open Questions

- [ ] Should raw `FIFO_IR` / `FIFO_RED` values be included in the packet? (waiting for app team)

## Serial Debug Output

When BLE is active, Serial prints:
- `BLE advertising as SVNIT_CRL_60` — at boot
- `BLE: client connected` — on app connect
- `BLE: client disconnected` — on app disconnect
- `BLE TX (95 chars): HR=72.0,...` — each packet
- `BLE replay: sending stored packets` — when offline buffer replay starts
- `BLE replay done` — when replay finishes
- `heap after BLE: free XXXXX, largest XXXXX` — at boot
