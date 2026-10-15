# Hardware Reference — Spandan Wearable

## Board

**Waveshare ESP32-S3-Touch-AMOLED-1.43**

| Spec | Value |
|------|-------|
| MCU | ESP32-S3R8 (dual-core Xtensa LX7, 240 MHz) |
| Flash | 16 MB |
| PSRAM | 8 MB OPI |
| Display | 1.43" round AMOLED, 466×466, QSPI |
| Display Controller | SH8601 **or** CO5300 (auto-detected at boot) |
| Touch | FT3168 / FT6146, I2C |
| IMU | QMI8658 (onboard), I2C |
| RTC | PCF85063 (onboard), I2C, backup battery connector |
| SD Card | TF/SD slot, SPI |
| Battery | LiPo connector, ETA6098 charger, voltage divider on GPIO4 |
| Fuel Gauge | **None** — battery level is voltage-estimate only |

### Links

- [Waveshare Product Page & Docs](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/Resources-And-Documents)
- [Waveshare Arduino Setup](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.43/Arduino)
- [Purchase (Robu.in)](https://robu.in/product/waveshare-esp32-s3-1-43inch-amoled-display-development-board-466x466-qspi-interface-round-display-optional-for-cnc-metal-case-esp32-with-display/)

## Pin Map

| Function | GPIO | Notes |
|----------|------|-------|
| I2C SDA | **47** | Shared: touch, IMU, RTC, MAX30102, MLX90614 |
| I2C SCL | **48** | Shared: same bus |
| Display QSPI CS | 9 | |
| Display QSPI CLK | 10 | |
| Display QSPI D0 | 11 | |
| Display QSPI D1 | 12 | |
| Display QSPI D2 | 13 | |
| Display QSPI D3 | 14 | |
| Display RESET | 21 | |
| Display EN (AMOLED_EN) | 42 | ⚠️ **Not driven** — polarity unverified, display works without it |
| SD CS | 38 | |
| SD MOSI | 39 | |
| SD MISO | 40 | |
| SD CLK | 41 | |
| Battery ADC | 4 | Through 1/3 voltage divider |
| IMU INT | 8 | Unused in firmware |

## I2C Bus (100 kHz)

| Device | Address | Notes |
|--------|---------|-------|
| MAX30102 (PPG) | `0x57` | External, on SH1.0 connector |
| QMI8658 (IMU) | `0x6B` | Onboard |
| MLX90614 (IR temp) | `0x5A` | External, on SH1.0 connector |
| FT3168 (touch) | `0x38` | Onboard |
| PCF85063 (RTC) | `0x51` | Onboard |

> **All 5 devices share one I2C bus**, protected by `g_i2cMux` mutex.
> MLX90614 limits the bus to 100 kHz (its max).

## External Sensors (on breadboard)

Connected to the 4-pin SH1.0 I2C connector (GND, 3V3, SDA, SCL):

1. **MAX30102** — PPG sensor for heart rate and SpO2
   - LED current: 0x19 (~5 mA each)
   - Sample rate: 200 sps, 8× averaging → 25 effective samples/s
   - 18-bit ADC, range 4096 nA
   - No interrupt line used (FIFO polled)

2. **MLX90614 / GY-906** — IR skin & ambient temperature
   - I2C only, max 100 kHz
   - Factory calibrated

## Known Hardware Quirks

1. **GPIO42 (AMOLED_EN)** — polarity unknown, left floating. Display works without it. Only revisit for display power-down.
2. **RTC backup cell** — physically detached on the dev board. Time lost at power-off. Fix: solder a pre-tabbed cell or use a holder. **Never solder directly onto a bare coin cell.**
3. **Battery ADC** — reads falsely high when on USB power (charger active). Assumes 1/3 divider from Waveshare demo. Verify with multimeter.
4. **Touch at boot** — occasionally reports "not found" then works fine. Needs retry loop (planned cleanup).
5. **Breadboard sensors** — loose contact causes signal drift that looks like motion artifact. Will improve when sensors are mounted properly.

## Previous Board (for reference)

The project originally ran on an **ESP32-C6 SuperMini** with an external MPU-6050 IMU. That board was replaced by the Waveshare ESP32-S3 AMOLED board, and the MPU-6050 was replaced by the onboard QMI8658.
