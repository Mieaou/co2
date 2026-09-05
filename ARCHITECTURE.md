# Architecture & Developer Reference: Multi-Sensor CO2 & Health Monitor

Comprehensive technical reference manual for human engineers and AI coding assistants.

---

## 1. System Overview

This project is an industrial-grade environmental and biometric monitoring system engineered for the **Seeed Studio XIAO nRF52840 Sense** microcontroller (Nordic Semiconductor nRF52840, ARM Cortex-M4F @ 64 MHz with FPU, 1 MB Flash, 256 KB RAM).

### Hardware Bus Topology
All three sensors share a single hardware TwoWire I2C bus operating in Fast Mode (400 kHz):
* **Sensirion SCD41** (`0x62`): Photoacoustic NDIR CO2 sensor with integrated SHT4x relative humidity & temperature sensor.
* **Bosch Sensortec BMP388** (`0x77` primary, `0x76` fallback): Ultra-precision piezoresistive barometric pressure and temperature sensor.
* **Maxim Integrated MAX30101** (`0x57`): Optical photoplethysmogram (PPG) sensor with integrated Red + Infrared LEDs and photodetector.

---

## 2. Directory Structure & Module Breakdown

The codebase is partitioned into distinct single-responsibility modules:

```
c:/Users/Manu/Desktop/co2/
├── config.h             # Central configuration: constants, pins, thresholds, timings
├── types.h              # Unified data structures, enums, telemetry models
├── dsp_filters.h        # DSP algorithms: RollingMedian, EmaFilter, HarmonicRejection, PI
├── dsp_filters.cpp      # Implementation of DSP algorithms and meteorological formulas
├── sensor_climate.h     # Climate manager interface (SCD41 + BMP388 fusion)
├── sensor_climate.cpp   # Implementation of asynchronous climate sampling & compensation
├── sensor_ppg.h         # Biometrics manager interface (MAX30101 + AGC + FSM)
├── sensor_ppg.cpp       # Implementation of FIFO drain, AGC, and biometric filtering
├── co2.ino              # Main application entry point: cooperative scheduler & serial output
└── ARCHITECTURE.md      # This architectural and functional documentation
```

---

## 3. Data Flow & Sensor Fusion Architecture

```
                                  [ Hardware Sensors ]
                                           │
                        ┌──────────────────┼──────────────────┐
                        ▼                  ▼                  ▼
                   [ BMP388 ]          [ SCD41 ]         [ MAX30101 ]
                 (Pressure/Temp)      (CO2/RH/Temp)       (Raw Red/IR)
                        │                  │                  │
                        │ Pressure (Pa)    │                  │
                        ├─────────────────►│ (Boyle-Mariotte) │
                        │                  │                  ▼
                        │                  │        [ Dynamic AGC Loop ]
                        │                  │        (Regulate 45k-185k)
                        │                  │                  │
                        ▼                  ▼                  ▼
              [ ISA / Relative ]     [ Magnus / ]    [ Maxim Algorithm ]
                Altitude Calc        Bolton Math        (100-sample FIFO)
                        │                  │                  │
                        │                  │                  ▼
                        │                  │        [ Harmonic Rejection ]
                        │                  │        (Anti-Doubling /2)
                        │                  │                  │
                        │                  │                  ▼
                        │                  │        [ Rolling Median (5) ]
                        │                  │                  │
                        │                  │                  ▼
                        │                  │         [ EMA Smoothing ]
                        │                  │                  │
                        └──────────────────┼──────────────────┘
                                           ▼
                                 [ Unified Telemetry ]
                                 (JSON / Serial UI)
```

---

## 4. Subsystem Details

### 4.1 Biometrics Subsystem (`sensor_ppg.h` / `sensor_ppg.cpp`)

#### Finite State Machine (FSM)
The PPG manager operates under a strict four-state finite state machine (`PpgState`):
1. `PPG_STATE_NO_FINGER`: Infrared signal is below `PPG_FINGER_THRESHOLD` (20,000 counts). Uses temporal hysteresis: requires 8 consecutive low samples (< 12,000 counts, ~320 ms) to confirm finger release, eliminating single-sample micro-glitch resets.
2. `PPG_STATE_CALIBRATING`: A finger touch is detected. The Closed-Loop AGC auto-tunes the LED drive current with 120 ms rate-limiting until the DC baseline sits comfortably in the linear ADC target window ($45\,000 - 165\,000$). Decoupled from Red saturation: verifies Red $\ge 8\,000$ counts without forcing IR into clipping. Once stabilized for $\ge 2$ consecutive evaluations, gain is locked, buffers are cleared, and FSM transitions to Acquiring.
3. `PPG_STATE_ACQUIRING`: Buffer accumulates the initial 100 samples (4 seconds at 25 Hz) of clean, constant-gain signal with zero AGC step disturbances. Progress percentage ($0-100\%$) is reported in telemetry.
4. `PPG_STATE_TRACKING`: Continuous sliding-window operation (computes every 1 second, 25 samples shift). AGC remains quiescent with a wide hysteresis deadband ($[18\,000, 235\,000]$ with 2.5s debounce) to prevent baseline jumps. Incorporates a 3-second clinical display holdover across transient motion noise.

#### Closed-Loop Adaptive AGC (Automatic Gain Control)
* Prevents ADC saturation on fair skin and noise degradation on dark or thick skin.
* Monitored DC level:
  $$\text{Target Window} = [45\,000, 165\,000] \subset [0, 262\,143]$$
* **Two-Speed Adaptive Stepping:**
  - If $|IR - \text{Target Boundary}| > 30\,000 \implies \text{Brightness} \mathrel{\pm}= 16$ (coarse convergence).
  - Near boundary $\implies \text{Brightness} \mathrel{\pm}= 4$ (fine settling, rate-limited to 120 ms).
  - Accelerates AGC convergence from 3.6 s to ~360 ms.
* **Decoupled Optical Safeguard:** Verifies $Red \ge 8\,000$ counts for valid AN6845 SpO2 without over-driving IR into clipping.
* Locks gain prior to buffer collection to guarantee zero-step signal integrity.

#### Dicrotic Notch Suppression & Harmonic Rejection
Normal human PPG waveforms have a **dicrotic notch** (aortic valve closure reflection). Simplistic peak detection algorithms often mistake this notch for an extra heartbeat, doubling the rate ($68 \to 136$ BPM).
* **Dynamic Refractory Period:** The detector dynamically scales refractory blocking based on the current stable heart rate:
  $$\tau_{\text{refractory}} = \min(14, \max(7, \text{round}(0.40 \cdot \overline{RR})))$$
  Initial default upon finger contact is 10 samples (400 ms at 25 Hz = 150 BPM max), completely blocking early dicrotic reflections (250–350 ms). Once a baseline is confirmed, it dynamically scales between 7 samples (280 ms / 214 BPM) and 14 samples (560 ms).
* **Adaptive Decaying Threshold:** Peak detection threshold is initialized to 42% of peak-to-peak amplitude above floor to exclude secondary wave reflections.
* **Relative Anti-Doubling Harmonic Check:**
  $$\text{Ratio} = \frac{\text{HR}_{\text{instantaneous}}}{\text{HR}_{\text{last\_stable}}}$$
  - Doubling harmonic check: $|\text{Ratio} - 2.0| \le 2.0 \cdot \text{tol} \implies [1.70, 2.30] \implies \text{Corrected HR} = \frac{\text{HR}_{\text{instantaneous}}}{2}$.
  - Recovery safety: Omits forced $2\times$ multiplication on $0.5\times$ drops to guarantee smooth self-recovery without harmonic lock-in.
  (Uses relative tolerance scaling to prevent false doubling during natural cardiac deceleration).

#### Rolling Median & EMA Filter Pipeline
* **`RollingMedian<float, 5>`**: An insertion-sorted circular window of the last 5 measurements. Completely cuts out single-measurement dropouts and transient spikes.
* **`EmaFilter` ($\alpha = 0.25$)**: Smooths physiological heart rate variability for human-readable display.

#### Medical-Grade SpO2 & Perfusion Index (PI %)
* **Cycle-by-Cycle Linear Baseline Detrending (Maxim AN6845):** For each cardiac cycle, $AC_{\text{Red}}$ and $AC_{\text{IR}}$ are extracted relative to the local beat baseline $(max + min)/2$.
* **Median $R$-Ratio Selection:** $R_k$ ratios are collected per beat and filtered via statistical median before calculating SpO2 ($R \in [0.15, 2.00]$ allows accurate 99-100% SpO2 detection).
* **Pulsatile Perfusion Index (PI %):**
  $$PI = \frac{\overline{AC_{\text{pulse}}}}{DC} \times 100\%$$
  Calculated strictly across confirmed cardiac cycles, preventing false inflation from respiratory baseline wander.

---

### 4.2 Climate Subsystem (`sensor_climate.h` / `sensor_climate.cpp`)

#### Dynamic Barometric Compensation
The SCD41 photoacoustic NDIR cell measures the density of CO2 molecules. At lower atmospheric pressures (e.g. higher altitude or cyclonic weather), fewer gas molecules occupy the chamber, artificially reducing the raw absorption.
* Every 1 second, `sensor_climate` reads Bosch BMP388's pressure in Pascals.
* Validates atmospheric range ($P \in [70\,000, 120\,000]\text{ Pa}$) per Sensirion datasheet.
* Transmits value directly to SCD41 via `scd4x.setAmbientPressure((uint32_t)bmp388.pressure)`.
* Automatic Self-Calibration (ASC) is explicitly controlled via `SCD41_ENABLE_ASC` in `config.h` (disabled by default for 24/7 unventilated spaces).
* Full 500 ms delay enforced on `stopPeriodicMeasurement()` during boot per Sensirion Section 3.5.2.

#### Relative Altitude & Baseline Settling
* Digital filter configured with `BMP3_IIR_FILTER_COEFF_7` and 8x pressure oversampling at 12.5 Hz ODR for sub-10cm desk-to-shelf resolution with minimal self-heating.
* Initial barometric baseline `baselinePressureHpa_` is latched after averaging the first 4 steady-state readings.
* **Absolute Altitude**: Computed against international standard atmosphere ($P_0 = 1013.25\text{ hPa}$).
* **Relative Altitude ($\Delta h$)**: Computed against settled `baselinePressureHpa_`.

---

### 4.3 Mathematical Models (`dsp_filters.h` / `dsp_filters.cpp`)

#### Dew Point (August-Roche-Magnus Approximation)
$$\alpha(T, RH) = \frac{17.27 \cdot T}{237.7 + T} + \ln\left(\frac{RH}{100}\right)$$
$$T_{\text{dew}} = \frac{237.7 \cdot \alpha(T, RH)}{17.27 - \alpha(T, RH)}$$

#### Absolute Humidity (Bolton Equation)
$$e_s(T) = 6.112 \cdot \exp\left(\frac{17.67 \cdot T}{T + 243.5}\right) \quad [\text{hPa}]$$
$$e(T, RH) = \frac{RH}{100} \cdot e_s(T) \quad [\text{hPa}]$$
$$AH = \frac{216.7 \cdot e(T, RH)}{273.15 + T} \quad [\text{g/m}^3]$$

#### Hypsometric Altitude Formula
$$h = 44330 \cdot \left(1 - \left(\frac{P}{P_{\text{ref}}}\right)^{0.190295}\right) \quad [\text{m}]$$

---

## 5. Telemetry & Protocol Specifications

### JSON Telemetry Contract (`OUTPUT_JSON = true`)
```json
{
  "time_ms": 12450,
  "co2_ppm": 684,
  "co2_status": "Good",
  "co2_warming_up": false,
  "temp_c": 24.2,
  "temp_bmp_c": 23.8,
  "humidity_pct": 46.5,
  "dew_point_c": 12.1,
  "abs_humidity_gm3": 10.2,
  "pressure_hpa": 1012.4,
  "pressure_mmhg": 759.4,
  "altitude_m": 7.1,
  "rel_altitude_m": 0.05,
  "finger_detected": true,
  "ppg_state": 3,
  "buffer_pct": 100,
  "hr_bpm": 68,
  "hr_bpm_raw": 67,
  "hr_valid": true,
  "spo2_pct": 98,
  "spo2_raw": 98,
  "spo2_valid": true,
  "perfusion_index": 1.42,
  "led_brightness": 64,
  "bmp_ok": true,
  "scd_ok": true,
  "max_ok": true
}
```

### Interactive Serial Command Interface
The device listens to single-byte commands via USB Serial (115200 baud) in real-time:
* `j` / `J`: Switch telemetry stream to single-line RFC JSON format.
* `d` / `D`: Switch telemetry stream to human-readable Visual Dashboard.
* `r` / `R`: Request immediate telemetry burst without waiting for the 3-second cycle.
* `?` / `h`: Print interactive command help banner.

---

## 6. How AI Coding Assistants Should Modify This Project

When implementing new features or maintenance fixes:
1. **Never add hardware drivers directly into `co2.ino`**: Keep `co2.ino` strictly as an orchestrator and telemetry formatter.
2. **Add configuration constants into `config.h`**: Avoid magic numbers in implementation files.
3. **Keep DSP algorithms pure and decoupled in `dsp_filters.h / .cpp`**: These must remain free of hardware library dependencies so they can be unit-tested.
4. **Compile verification**: Always verify changes using `arduino-cli compile --fqbn Seeeduino:nrf52:xiaonRF52840Sense c:\Users\Manu\Desktop\co2`.
