# Flutter Mobile App Plan: Multi-Sensor CO2 & Health Monitor

Detailed technical blueprint, BLE protocol contract, and prerequisites guide for building the native mobile application.

---

## 1. Prerequisites: What to Install on PC

To build and run a Flutter app on an Android phone, you need 3 components:

### A. Flutter SDK
1. Download the official Flutter Windows bundle:  
   👉 [https://docs.flutter.dev/get-started/install/windows/mobile](https://docs.flutter.dev/get-started/install/windows/mobile)
2. Extract the archive to a clean path (e.g. `C:\src\flutter` — **do NOT** put it in `Program Files`).
3. Add `C:\src\flutter\bin` to your Windows **User Environment Variables (`PATH`)**.
4. Open a new PowerShell terminal and verify:
   ```powershell
   flutter --version
   ```

### B. Android Studio (Provides Android SDK + Java JDK)
1. Download and install **Android Studio**:  
   👉 [https://developer.android.com/studio](https://developer.android.com/studio)
2. Open Android Studio $\to$ **More Actions** $\to$ **SDK Manager** $\to$ **SDK Tools** tab:
   - Check **Android SDK Command-line Tools (latest)**
   - Check **Android SDK Build-Tools**
   - Click **Apply** and let it install.
3. Open PowerShell and accept Android licenses:
   ```powershell
   flutter doctor --android-licenses
   ```
   *(press `y` to accept all prompts)*.
4. Verify complete setup:
   ```powershell
   flutter doctor
   ```

### C. Phone Setup (Physical Device)
1. On your Android phone: **Settings** $\to$ **About Phone** $\to$ tap **Build Number** 7 times to enable Developer Mode.
2. Go to **Settings** $\to$ **System / Additional** $\to$ **Developer Options** $\to$ enable **USB Debugging**.
3. Plug the phone into your PC with a USB cable (allow USB debugging prompt on phone screen).
4. Run `flutter devices` — your phone will appear in the list.

---

## 2. BLE Data Contract (Firmware Interface)

* **Device Name**: `CO2-Health-Monitor`
* **Service**: Nordic UART Service (NUS)
  - **Service UUID**: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - **TX Characteristic (Notify)**: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
  - **RX Characteristic (Write)**: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
* **Telemetry Payload**: Single-line JSON packet received every 3 seconds:
```json
{
  "time_ms": 45120,
  "co2_ppm": 780,
  "co2_status": "Good",
  "co2_warming_up": false,
  "temp_c": 23.5,
  "temp_bmp_c": 23.2,
  "humidity_pct": 46.1,
  "dew_point_c": 11.2,
  "abs_humidity_gm3": 9.7,
  "pressure_hpa": 1013.2,
  "pressure_mmhg": 759.9,
  "altitude_m": 15.4,
  "rel_altitude_m": 0.00,
  "finger_detected": true,
  "ppg_state": 3,
  "buffer_pct": 100,
  "hr_bpm": 72,
  "hr_bpm_raw": 71,
  "hr_valid": true,
  "spo2_pct": 98,
  "spo2_raw": 98,
  "spo2_valid": true,
  "perfusion_index": 2.15,
  "led_brightness": 60,
  "bmp_ok": true,
  "scd_ok": true,
  "max_ok": true
}
```

---

## 3. Recommended Flutter Dependencies (`pubspec.yaml`)

```yaml
dependencies:
  flutter:
    sdk: flutter
  
  # Bluetooth Low Energy
  flutter_blue_plus: ^1.34.0
  permission_handler: ^11.3.0
  
  # State Management & Utilities
  provider: ^6.1.2
  intl: ^0.19.0
  
  # Beautiful Charts
  fl_chart: ^0.69.0
  
  # Local Database (Daily Logging)
  sqflite: ^2.3.3
  path_provider: ^2.1.2
  path: ^1.9.0
  
  # Background Service & Alarms
  flutter_foreground_task: ^6.1.0
  flutter_local_notifications: ^17.1.0
  audioplayers: ^6.0.0
```

---

## 4. Phased Implementation Roadmap

### Phase 1: Real-Time Live Dashboard (Starting Point)
1. **BLE Service (`BleService`)**:
   - Handles Bluetooth permissions (Bluetooth Scan, Connect, Fine Location on Android 12+).
   - Scans and automatically connects to `CO2-Health-Monitor`.
   - Reassembly buffer for incoming chunked BLE UART packets, parses JSON line-by-line.
2. **Dashboard UI**:
   - Modern dark glassmorphism card design.
   - Large Circular CO2 Gauge:
     - 🟢 `< 800 ppm`: "Отлично / Свежий воздух"
     - 🟡 `800 - 1200 ppm`: "Умеренно / Норма"
     - 🟠 `1200 - 1600 ppm`: "Повышен / Проветрите"
     - 🔴 `> 1600 ppm`: "Опасно / Высокий уровень"
   - Climate Grid: Температура, Влажность, Точка росы, Давление (мм рт. ст. и гПа).
   - Biometric Card:
     - Статус пальца (Приложите палец / Калибровка / Измерение).
     - Пульс (BPM) с анимированным пульсирующим сердечком.
     - SpO2 (%) и индекс перфузии (PI).

### Phase 2: Historical Logging & Interactive Charts
1. **SQLite Database (`DatabaseService`)**:
   - Stores aggregated records (every 10s or 1 minute).
   - Auto-cleanup for records older than 30 days.
2. **Charts View**:
   - Filter by: 1 час / 24 часа / 7 дней.
   - Smooth Bezier curves (via `fl_chart`) with min/max/average markers.
   - Export history to CSV.

### Phase 3: Smart Ventilation Alarm & Background Service
1. **Foreground Service (`flutter_foreground_task`)**:
   - Keeps BLE connection active 24/7 even when phone screen is turned off.
2. **Ventilation Alarm Logic**:
   - User-settable threshold (e.g., default: 1200 ppm).
   - If CO2 remains above threshold for $> 2$ minutes:
     - Triggers persistent high-priority notification.
     - Plays audio chime / speech synthesis ("Пора проветрить помещение!").
     - Snooze button (10 min / 30 min).
