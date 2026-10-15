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

## Open Questions

- [ ] Should raw `FIFO_IR` / `FIFO_RED` values be included in the packet? (waiting for app team)

## Serial Debug Output

When BLE is active, Serial prints:
- `BLE advertising as SVNIT_CRL_60` — at boot
- `BLE: client connected` — on app connect
- `BLE: client disconnected` — on app disconnect
- `BLE TX (95 chars): HR=72.0,...` — each packet
- `heap after BLE: free XXXXX, largest XXXXX` — at boot
