# Project SPANDAN — Master Report Data & Technical Documentation

> **Title**: SPANDAN: AI-Enabled Wearable Health and Stress Monitoring System for Police Personnel  
> **Collaboration**: Sardar Vallabhbhai National Institute of Technology (SVNIT), Surat & Gujarat Police  
> **Departments Involved**:
> - Department of Electronics and Communication Engineering (ECE): Hardware, Sensors & Firmware
> - Department of Computer Science and Engineering (CSE): Ingestion Backend, Mobile App & Web Command Dashboard
> - Department of Artificial Intelligence (AI): Physiological Anomaly Detection & Focus Monitoring Models
> **Academic Year**: 2025–2026

---

## 1. Executive Summary & Problem Definition

### 1.1 Operational Need for Police Personnel
Police personnel operate under physically arduous, high-stress, and thermally taxing environments (crowd control, emergency response, extended shift rotations). These operational conditions lead to:
1. Cumulative physical and cognitive stress.
2. Fatigue buildup compromising alertness and operational readiness.
3. Heat strain, dehydration, and cardiovascular vulnerability.

### 1.2 Limitations of Commercial Solutions vs. SPANDAN
| Dimension | Commercial Fitness Wearables | Project SPANDAN |
| :--- | :--- | :--- |
| **System Purpose** | Individual consumer lifestyle & fitness | Mission-critical institutional health & force readiness |
| **Stress Monitoring** | Basic static HR variability | Multi-modal fusion: AI + HRV + IMU Motion + Skin Temperature |
| **Command Oversight** | Single-user mobile app | Centralized Command Dashboard with live fleet monitoring |
| **Risk Prediction** | None (reactive threshold alerts only) | Early warning predictive models (MIT-BIH + Real duty data) |
| **Emergency Positioning** | Manual/Limited SOS | Automated health triggers tied to GPS location coordinates |
| **Data Privacy & Security** | 3rd-party commercial cloud storage | End-to-end institutional ownership, zero 3rd-party leakage |
| **Cost at Scale** | ₹3,000 – ₹10,000 (device only, no fleet tools) | ₹6,000 – ₹8,000 at scale + dedicated fleet command stack |

---

## 2. Multi-Department System Architecture

```text
+-----------------------------------------------------------------------------------+
|                            PROJECT SPANDAN SYSTEM PIPELINE                         |
+-----------------------------------------------------------------------------------+
|  [ ECE Wearable Unit ]                                                            |
|  - ESP32-C6 SuperMini / ESP32-S3 AMOLED                                           |
|  - MAX30102 (PPG / SpO2), MPU-6050 (IMU), GY-906 / MLX90614 (IR Skin Temp)       |
|  - Embedded DSP: DC Removal, Kalman HR Filter, Motion Gating                      |
|                                       │                                           |
|                         BLE 5.3 GATT (1 Hz ASCII packets)                         |
|                                       ▼                                           |
|  [ Mobile Gateway App (Flutter) ]                                                 |
|  - Android Foreground Service ("Police Data Hub")                                 |
|  - GPS Location Tagging (Latitude, Longitude, Altitude)                           |
|  - Disk-Persistent Resilient Buffer (bg_data_queue.jsonl, max 10MB / 2000 pkts)   |
|                                       │                                           |
|                       HTTPS / MQTT (10-second batched JSON)                       |
|                                       ▼                                           |
|  [ Ingestion Backend (CSE: Bun/Hono & Node/Express) ]                             |
|  - REST Ingestion Endpoint (/api/health/ingest) with X-Security-Token             |
|  - Multi-tier Storage: PostgreSQL / MongoDB / SQLite                              |
|  - Real-time Broadcast: WebSockets / Socket.io                                    |
|                      │                                     │                      |
|                      ▼                                     ▼                      |
|  [ AI Department Analytics ]             [ Command Web Dashboard (CSE) ]          |
|  - Model 1C: Physical State Classifier   - Live Fleet Map & Officer GPS Tracking  |
|    (Stress / Physical / Normal)          - Real-Time Vitals & Dynamic Alerts      |
|  - Model 2C: Focus & Distraction Model   - AI Insights & Shift Cognitive Bar      |
|  - Dual-path supervised & unsupervised   - SQL / CSV Data Explorer & Recovery     |
+-----------------------------------------------------------------------------------+
```

---

## 3. ECE Subsystem: Hardware & Firmware Specifications

### 3.1 Hardware Bill of Materials (POC3 / POC4)
| Item | Component | Model / Part | Unit Cost (₹) | Function |
| :---: | :--- | :--- | :---: | :--- |
| 1 | Microcontroller | ESP32-C6 SuperMini / ESP32-S3 | 550 | Central processing, signal DSP & BLE communication |
| 2 | PPG Pulse Oximeter | MAX30102 | 250 | Dual-wavelength optical HR & SpO2 acquisition |
| 3 | Inertial Unit (IMU) | MPU-6050 / QMI8658 | 150 | 6-axis acceleration & angular velocity motion tracking |
| 4 | Infrared Temp Sensor | GY-906 (MLX90614) | 350 | Non-contact accurate skin surface temperature |
| 5 | Battery Subsystem | Li-Po 2S 7.4V (or 1S 3.7V) | 600 | System power supply |
| 6 | Voltage Regulator | LM7805CV / Buck Converter | 60 | Stable 5V/3.3V power rails |
| 7 | Battery Charger (BMS) | 2S 4A Type-C (or TP4056/IP2312) | 150 | CC/CV charging with protection circuitry |
| 8 | Custom PCB | THT Double-Layer (30×37mm) | 500 | Component integration, ground & power planes |
| 9 | Enclosure | 3D Printed PLA-Pro (5th Model) | 100 | Ergonomic wrist housing with acrylic top lid |
| 10 | Wrist Strap & Sundries| Silicone watch band, fasteners | 250 | Wearable mounting |
| **Total** | | | **~₹2,970** | *Hardware prototype production cost* |

### 3.2 Power Budget & Battery Life Analysis (POC3 Validation)
- Operating Voltage: $V = 3.3\text{ V}$
- Current consumption per module:
  - ESP32-C6 SuperMini: $100\text{ mA}$ ($330\text{ mW}$)
  - MAX30102 Optical Sensor: $25\text{ mA}$ ($82.5\text{ mW}$)
  - MPU-6050 Motion Sensor: $20\text{ mA}$ ($66\text{ mW}$)
  - GY-906 Temperature Sensor: $10\text{ mA}$ ($33\text{ mW}$)
  - BLE 5.3 Active Broadcast: $20\text{ mA}$ ($66\text{ mW}$)
  - **Total System Current**: $I_{\text{total}} = 175\text{ mA}$ ($577.5\text{ mW}$)

- **Theoretical Runtime** ($1600\text{ mAh}$ Li-Po battery):
  $$T_{\text{theoretical}} = \frac{1600\text{ mAh}}{175\text{ mA}} \approx 9.14\text{ hours}$$

- **Measured Practical Runtime**:
  $$T_{\text{practical}} = 6\text{ h } 34\text{ min} = 6.56\text{ hours}$$

- **System Discharge Efficiency**:
  $$\eta = \frac{T_{\text{practical}}}{T_{\text{theoretical}}} \times 100 = \frac{6.56}{9.14} \times 100 \approx 71.77\%$$

### 3.3 Mathematical Algorithms & Embedded DSP
1. **PPG DC Removal (1st-Order IIR High-Pass Filter)**:
   $$dc[n] = \alpha \cdot dc[n-1] + (1 - \alpha) \cdot ir[n], \quad \alpha = 0.985$$
   $$ac[n] = ir[n] - dc[n]$$
2. **Kalman Filter for Heart Rate Smoothing**:
   $$P_{n|n-1} = P_{n-1} + Q, \quad Q = 0.15$$
   $$K_n = \frac{P_{n|n-1}}{P_{n|n-1} + R}, \quad R = 3.0$$
   $$\text{HR}_n = \text{HR}_{n-1} + K_n \cdot (\text{BPM}_{\text{meas}} - \text{HR}_{n-1})$$
   $$P_n = (1 - K_n) \cdot P_{n|n-1}$$
3. **Oxygen Saturation ($\text{SpO}_2$) Ratio-of-Ratios**:
   $$R = \frac{\sigma_{\text{red}} / \mu_{\text{red}}}{\sigma_{\text{ir}} / \mu_{\text{ir}}}$$
   $$\text{SpO}_2 = 110 - 25 \cdot R$$
4. **Heart Rate Variability ($\text{RMSSD}$)**:
   $$\text{RMSSD} = \sqrt{\frac{1}{N-1} \sum_{i=2}^N (\text{RR}_i - \text{RR}_{i-1})^2}$$
5. **IMU Motion Indicator & Gating**:
   $$g_{\text{total}} = \frac{\sqrt{a_x^2 + a_y^2 + a_z^2}}{16384} \quad [\text{g}]$$
   $$\text{motion} = |g_{\text{total}} - g_{\text{LP}}|$$
   *Gating thresholds*: beat detection suppressed if $\text{motion} > 0.05\text{ g}$; $\text{SpO}_2$ update suppressed if $\text{motion} > 0.08\text{ g}$.

---

## 4. Mobile Gateway & CSE Backend Architecture

### 4.1 Mobile Gateway Application (`bletomp/mobile_app`)
- **Framework**: Flutter (Dart) with `flutter_blue_plus`, `geolocator`, `flutter_background_service`.
- **Always-On Daemon**: Configured with `AndroidForegroundType.location` & `dataSync`, `autoStartOnBoot`.
- **Packet Ingestion & Buffering**:
  - Subscribes to GATT Notify UUID: `beb5483e-36e1-4688-b7f5-ea07361b26a8`.
  - Parses ASCII key-value stream.
  - Adds real-time GPS coordinates (lat, lng, altitude).
  - Two-stage offline resilience: RAM queue + persistent `bg_data_queue.jsonl` (10 MB cap / ~2000 packets).
  - Flushes to server every 10 seconds via HTTPS/MQTT.

### 4.2 Web Command Platform (`guardian-health`)
- **Frontend Stack**: React 18, Vite, TypeScript, TailwindCSS, Lucide-React, Leaflet Maps.
- **Backend Stack**: Bun, Hono framework, Drizzle ORM, PostgreSQL.
- **Reverse Proxy & Deployment**: Caddy reverse proxy serving `http://localhost`, multi-stage Docker containerization.
- **Core Endpoints**:
  - `POST /api/health/ingest` — Primary secure vital ingestion.
  - `GET /api/health` — Live telemetry polling.
  - `GET /api/alerts` — Real-time officer alert stream.
  - `GET /api/personnel/officers` — Officer roster and active duty status.
  - `POST /api/auth/login` & `/api/auth/me` — Role-based access control.

---

## 5. AI Department: Stress & Health Prediction Models

### 5.1 Dataset & Training Regime
- **Training Cohort**: 29 active Gujarat Police officers during real shift duties.
- **Data Volume**: 100,658 recorded data points.
- **Validation Strategy**: Chronological per-officer test split of 15,113 samples.
- **Sim-to-Real Insight**: Purely synthetic dataset models yielded only 30.75% accuracy and 0.58% Stress F1, proving the necessity of field calibration.

### 5.2 Model Performance Metrics
| Model | Target Classification | Algorithm | Test Accuracy | Macro F1 | Key Metric |
| :--- | :--- | :--- | :---: | :---: | :--- |
| **Model 1C** | Physical State (Normal / Stress / Activity) | Tuned Random Forest / XGBoost | **78.59%** | **78.10%** | **88.51% Stress F1** |
| **Model 2C** | Focus State (Focused / Distracted) | Supervised Gradient Boosted Ensemble | **98.12%** | **97.28%** | **100% Distraction Recall** |

### 5.3 Live AI Endpoints
- **Hugging Face Space**: `https://huggingface.co/spaces/TheDeepDas/Spandan`
- **Code Repository**: `https://github.com/THE-DEEPDAS/Spandan`
