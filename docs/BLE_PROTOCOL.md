# BLE Protocol — Spandan Wearable

## Connection Parameters

| Parameter | Value |
|-----------|-------|
| Device Name | `SVNIT_CRL_60` (change per unit via `BLE_DEVICE_NAME` define; keep `SVNIT_CRL_` prefix) |
| Service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Characteristic UUID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` |
| Characteristic Property | **NOTIFY** (read-only from app side) |
| MTU | 517 requested |
| Rate | 1 packet per second |
| Max Packet Size | < 200 characters |

> ⚠️ **UUIDs and name prefix are registered on the Spandan app server — do not change.**

## Packet Format

Plain text, comma-separated key=value pairs:

```
HR=72.0,SpO2=98.0,Temp=36.5,Batt=85,accX=0.12,accY=-0.05,accZ=9.81,gX=1.2,gY=-0.8,gZ=0.3,ST=OK
```

### Fields

| Key | Type | Description | Missing Value |
|-----|------|-------------|---------------|
| `HR` | float | Heart rate (bpm) | `--` |
| `SpO2` | float | Blood oxygen (%) | `--` |
| `Temp` | float | Skin temperature (°C) | `--` |
| `Batt` | int | Battery level (%) | `--` |
| `accX`, `accY`, `accZ` | float | Accelerometer (g) | `--` |
| `gX`, `gY`, `gZ` | float | Gyroscope (dps) | `--` |
| `ST` | string | Status code | `--` |
| `TS` | string | RTC timestamp (added stage 7 v0). Present on buffered packets; planned for live too | (absent if live, pre-7v1) |
| `BUF` | int | `1` if this is a replayed offline-buffered packet; absent on live packets | (absent) |

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

## App Requirements (CSE team reply, 2026-10-12)

- Name format: `SVNIT_CRL_XXX` (registered on server, number changes per unit)
- NOTIFY mandatory, WRITE optional (for future `AUTH:<token>` handshake)
- App parses: `HR`, `SpO2`, `Temp`, `Batt`
- 1 Hz rate, keep under ~200 characters
- No bonding required
- `DISCONNECT` / `SHUTDOWN` packet = clean shutdown (not implemented yet, no shutdown path exists)
- JSON or binary welcome later (added alongside old format)

## CSE Team Reply (2026-10-18) — Stage 7 v1 Requirements

- **Time sync:** App writes `TIME:<epoch>,<offset min>` to the existing characteristic after `AUTH:<token>`
- **TS format:** ISO with timezone (e.g. `2026-10-18T13:00:00+05:30`)
- **TS on live packets too** (not just buffered)
- **Send `CHG` and `BatV`** fields in the packet
- **Replay pacing:** 50–100 ms between replayed buffered packets
- **Server handling:** Server stores `BUF=1` packets under their `TS`; app hides them from the live screen

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
