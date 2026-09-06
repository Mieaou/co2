/**
 * @file co2.ino
 * @brief Multi-Sensor Environmental & Biometric Monitor: Main Orchestrator.
 * 
 * Hardware:
 * - Microcontroller: Seeed XIAO nRF52840 Sense (ARM Cortex-M4F)
 * - Sensirion SCD41: NDIR CO2, Temperature, Relative Humidity
 * - Bosch BMP388: High-precision Barometric Pressure & Temperature
 * - Maxim MAX30101: Pulse Oximeter & Heart Rate Monitor
 * 
 * Architecture:
 * - Non-blocking cooperative multitasking scheduler (0 delay)
 * - Closed-loop AGC for optical PPG
 * - Dynamic barometric pressure compensation for SCD41 (BMP388 -> SCD41)
 * - Anti-doubling harmonic rejection & 5-point rolling median filtering
 */

#include <Wire.h>
#include "config.h"
#include "types.h"
#include "dsp_filters.h"
#include "sensor_climate.h"
#include "sensor_ppg.h"
#include "ble_manager.h"

// =============================================================================
// GLOBAL MANAGERS
// =============================================================================

ClimateSensorManager climate;
PpgSensorManager ppg;
BleManager ble;

unsigned long lastTelemetryMs = 0;
bool outputJson = OUTPUT_JSON;

// =============================================================================
// TELEMETRY FORMATTERS
// =============================================================================

/**
 * @brief Streams a single-line, RFC-compliant JSON object to any Arduino Print stream.
 */
void formatJsonTelemetry(Print& out) {
  const ClimateData& c = climate.getData();
  const BiometricData& b = ppg.getData();
  const SensorHealth& h = climate.getHealth();

  out.print(F("{\"time_ms\":")); out.print(millis());
  
  // Climate telemetry
  out.print(F(",\"co2_ppm\":")); out.print(c.co2_ppm);
  out.print(F(",\"co2_status\":\"")); out.print(c.co2_status); out.print(F("\""));
  out.print(F(",\"co2_warming_up\":")); out.print(c.is_warming_up ? F("true") : F("false"));
  out.print(F(",\"temp_c\":")); out.print(c.temp_c, 1);
  out.print(F(",\"temp_bmp_c\":")); out.print(c.temp_bmp_c, 1);
  out.print(F(",\"humidity_pct\":")); out.print(c.humidity_pct, 1);
  out.print(F(",\"dew_point_c\":")); out.print(c.dew_point_c, 1);
  out.print(F(",\"abs_humidity_gm3\":")); out.print(c.abs_humidity_gm3, 1);
  out.print(F(",\"pressure_hpa\":")); out.print(c.pressure_hpa, 1);
  out.print(F(",\"pressure_mmhg\":")); out.print(c.pressure_mmhg, 1);
  out.print(F(",\"altitude_m\":")); out.print(c.altitude_m, 1);
  out.print(F(",\"rel_altitude_m\":")); out.print(c.relative_altitude_m, 2);

  // Biometric telemetry
  out.print(F(",\"finger_detected\":")); out.print(b.finger_detected ? F("true") : F("false"));
  out.print(F(",\"ppg_state\":")); out.print(static_cast<int>(b.state));
  out.print(F(",\"buffer_pct\":")); out.print(b.buffer_progress_pct);
  out.print(F(",\"hr_bpm\":"));
  if (b.hr_valid) out.print(b.hr_bpm); else out.print(F("null"));
  out.print(F(",\"hr_bpm_raw\":"));
  if (b.hr_bpm_raw > 0) out.print(b.hr_bpm_raw); else out.print(F("null"));
  out.print(F(",\"hr_valid\":")); out.print(b.hr_valid ? F("true") : F("false"));
  out.print(F(",\"spo2_pct\":"));
  if (b.spo2_valid) out.print(b.spo2); else out.print(F("null"));
  out.print(F(",\"spo2_raw\":"));
  if (b.spo2_raw > 0) out.print(b.spo2_raw); else out.print(F("null"));
  out.print(F(",\"spo2_valid\":")); out.print(b.spo2_valid ? F("true") : F("false"));
  out.print(F(",\"perfusion_index\":")); out.print(b.perfusion_index, 2);
  out.print(F(",\"led_brightness\":")); out.print(b.led_brightness);

  // Hardware diagnostics
  out.print(F(",\"bmp_ok\":")); out.print(h.bmp388_online ? F("true") : F("false"));
  out.print(F(",\"scd_ok\":")); out.print(h.scd41_online ? F("true") : F("false"));
  out.print(F(",\"max_ok\":")); out.print(ppg.isOnline() ? F("true") : F("false"));
  out.println(F("}"));
}

/**
 * @brief Outputs a formatted visual text dashboard for Serial Monitor or BLE.
 */
void printDashboardTelemetry(Print& out = Serial) {
  const ClimateData& c = climate.getData();
  const BiometricData& b = ppg.getData();

  out.println(F("\n=================================================="));
  out.print(F("[UPTIME: ")); out.print(millis() / 1000); out.println(F(" s]"));
  
  out.println(F("--- [CLIMATE] Sensirion SCD41 + Bosch BMP388 ---"));
  out.print(F("  CO2:            ")); out.print(c.co2_ppm); out.print(F(" ppm [")); out.print(c.co2_status); out.println(F("]"));
  out.print(F("  Temperature:    ")); out.print(c.temp_c, 1); out.print(F(" °C  (BMP388 Ref: ")); out.print(c.temp_bmp_c, 1); out.println(F(" °C)"));
  out.print(F("  Humidity:       ")); out.print(c.humidity_pct, 1); out.println(F(" %"));
  out.print(F("  Dew Point:      ")); out.print(c.dew_point_c, 1); out.println(F(" °C"));
  out.print(F("  Abs. Humidity:  ")); out.print(c.abs_humidity_gm3, 1); out.println(F(" g/m³"));
  out.print(F("  Pressure:       ")); out.print(c.pressure_mmhg, 1); out.print(F(" mmHg (")); out.print(c.pressure_hpa, 1); out.println(F(" hPa)"));
  out.print(F("  Altitude (ISA): ")); out.print(c.altitude_m, 1); out.print(F(" m  (Delta: ")); out.print(c.relative_altitude_m, 2); out.println(F(" m)"));

  out.println(F("--- [BIOMETRICS] Maxim MAX30101 (AGC + DSP) ---"));
  out.print(F("  Finger Contact: ")); out.println(b.finger_detected ? F("YES") : F("NO"));
  out.print(F("  Subsystem State:"));
  switch (b.state) {
    case PPG_STATE_NO_FINGER:    out.println(F("No Finger Detected")); break;
    case PPG_STATE_CALIBRATING:  out.println(F("Calibrating AGC Gain...")); break;
    case PPG_STATE_ACQUIRING:    out.print(F("Acquiring 4s Buffer (")); 
                                 out.print(b.buffer_progress_pct); out.println(F("%)")); break;
    case PPG_STATE_TRACKING:     out.println(F("Tracking (Active)")); break;
  }

  out.print(F("  Heart Rate:     "));
  if (b.hr_valid) {
    out.print(b.hr_bpm); out.println(F(" BPM (Filtered)"));
  } else {
    if (b.state == PPG_STATE_CALIBRATING) {
      out.println(F("Calibrating AGC..."));
    } else if (b.state == PPG_STATE_ACQUIRING) {
      out.print(F("Acquiring (")); out.print(b.buffer_progress_pct); out.println(F("%)..."));
    } else if (b.state == PPG_STATE_TRACKING) {
      out.println(F("Analyzing..."));
    } else {
      out.println(F("--"));
    }
  }

  out.print(F("  SpO2:           "));
  if (b.spo2_valid) {
    out.print(b.spo2); out.println(F(" %"));
  } else {
    if (b.state == PPG_STATE_CALIBRATING) {
      out.println(F("Calibrating AGC..."));
    } else if (b.state == PPG_STATE_ACQUIRING) {
      out.print(F("Acquiring (")); out.print(b.buffer_progress_pct); out.println(F("%)..."));
    } else if (b.state == PPG_STATE_TRACKING) {
      out.println(F("Analyzing..."));
    } else {
      out.println(F("--"));
    }
  }

  out.print(F("  Perfusion Index:")); out.print(b.perfusion_index, 2); out.println(F(" %"));
  out.print(F("  AGC Drive:      ")); out.print(b.led_brightness); out.println(F(" / 255"));
  out.println(F("=================================================="));
}

// =============================================================================
// HARDWARE I2C BUS RECOVERY & SAFE PROBE
// =============================================================================

/**
 * @brief Performs an active I2C bus recovery sequence (16 clock pulses + STOP).
 * 
 * If a slave device was interrupted mid-transaction (e.g. during firmware upload
 * without power cycle), it may hold SDA low. Clocking SCL prompts the slave to release SDA.
 */
void recoverI2CBus(uint8_t pinSda = PIN_WIRE_SDA, uint8_t pinScl = PIN_WIRE_SCL) {
  pinMode(pinSda, INPUT_PULLUP);
  pinMode(pinScl, OUTPUT);

  // Send 16 clock cycles on SCL to clear any stuck byte transmission
  for (int i = 0; i < 16; i++) {
    digitalWrite(pinScl, LOW);
    delayMicroseconds(10);
    digitalWrite(pinScl, HIGH);
    delayMicroseconds(10);
  }

  // Generate hardware I2C STOP condition: SDA LOW -> SCL HIGH -> SDA HIGH
  pinMode(pinSda, OUTPUT);
  digitalWrite(pinSda, LOW);
  delayMicroseconds(10);
  digitalWrite(pinScl, HIGH);
  delayMicroseconds(10);
  digitalWrite(pinSda, HIGH);
  delayMicroseconds(10);

  // Return pins to high-impedance with pullup
  pinMode(pinSda, INPUT_PULLUP);
  pinMode(pinScl, INPUT_PULLUP);
  delayMicroseconds(50);
}

/**
 * @brief Non-blocking bit-banged I2C address ping.
 * 
 * Tests whether a sensor responds with ACK at the specified 7-bit address
 * WITHOUT touching the hardware TWIM peripheral. Prevents the nRF52 TWIM
 * from entering an unrecoverable while(!EVENTS_STOPPED) lockup on NACK.
 */
bool safeI2CPing(uint8_t address, uint8_t pinSda = PIN_WIRE_SDA, uint8_t pinScl = PIN_WIRE_SCL) {
  pinMode(pinSda, INPUT_PULLUP);
  pinMode(pinScl, INPUT_PULLUP);
  delayMicroseconds(5);

  // START condition: SDA LOW while SCL is HIGH
  pinMode(pinSda, OUTPUT);
  digitalWrite(pinSda, LOW);
  delayMicroseconds(5);
  pinMode(pinScl, OUTPUT);
  digitalWrite(pinScl, LOW);
  delayMicroseconds(5);

  // Shift 7-bit address + Write bit (0)
  uint8_t txByte = (address << 1);
  for (int i = 7; i >= 0; i--) {
    if (txByte & (1 << i)) {
      pinMode(pinSda, INPUT_PULLUP);
    } else {
      pinMode(pinSda, OUTPUT);
      digitalWrite(pinSda, LOW);
    }
    delayMicroseconds(5);
    digitalWrite(pinScl, HIGH);
    delayMicroseconds(5);
    digitalWrite(pinScl, LOW);
    delayMicroseconds(5);
  }

  // Read ACK from slave on 9th clock pulse
  pinMode(pinSda, INPUT_PULLUP);
  delayMicroseconds(5);
  digitalWrite(pinScl, HIGH);
  delayMicroseconds(5);
  bool ack = (digitalRead(pinSda) == LOW);
  digitalWrite(pinScl, LOW);
  delayMicroseconds(5);

  // STOP condition
  pinMode(pinSda, OUTPUT);
  digitalWrite(pinSda, LOW);
  delayMicroseconds(5);
  digitalWrite(pinScl, HIGH);
  delayMicroseconds(5);
  pinMode(pinSda, INPUT_PULLUP);
  delayMicroseconds(10);

  return ack;
}

// Connection & visual diagnostics state
bool wasSerialConnected = false;
unsigned long lastHeartbeatMs = 0;
bool heartbeatState = false;

// =============================================================================
// SETUP
// =============================================================================

void setup() {
  // Initialize onboard RGB LEDs (active LOW on Seeed XIAO nRF52840)
  pinMode(LED_RED, OUTPUT);
  digitalWrite(LED_RED, HIGH);
  pinMode(LED_GREEN, OUTPUT);
  digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE, OUTPUT);
  digitalWrite(LED_BLUE, LOW); // Turn on Blue LED during initialization

  Serial.begin(SERIAL_BAUD_RATE);

  // 1. Recover I2C bus from any residual slave lockup
  recoverI2CBus(PIN_WIRE_SDA, PIN_WIRE_SCL);

  // 2. Safe probe of all sensor addresses before starting hardware TWIM
  uint8_t bmpAddress = 0;
  if (safeI2CPing(BMP388_I2C_ADDR_PRIMARY)) {
    bmpAddress = BMP388_I2C_ADDR_PRIMARY; // 0x77
  } else if (safeI2CPing(BMP388_I2C_ADDR_SECONDARY)) {
    bmpAddress = BMP388_I2C_ADDR_SECONDARY; // 0x76
  }

  bool scdPresent = safeI2CPing(SCD41_I2C_ADDR);     // 0x62
  bool maxPresent = safeI2CPing(MAX30101_I2C_ADDR);   // 0x57

  // 3. Initialize hardware Wire safely and enforce Fast Mode (400 kHz) deterministically
  Wire.begin();
  Wire.setClock(400000);

  // 4. Initialize climate subsystem
  climate.begin(Wire, bmpAddress, scdPresent);

  // 5. Initialize biometric subsystem
  ppg.begin(Wire, maxPresent);

  // 6. Initialize Bluetooth Low Energy subsystem
  ble.begin(BLE_DEVICE_NAME);

  // Turn off Blue LED
  digitalWrite(LED_BLUE, HIGH);

  // If Serial Monitor is already connected at boot, print welcome banner
  if (Serial) {
    wasSerialConnected = true;
    Serial.println(F("\n=================================================="));
    Serial.println(F("  CO2 + Health Monitor: System Initialized"));
    Serial.println(F("=================================================="));
    Serial.print(F("  BMP388:   "));
    if (bmpAddress != 0) { Serial.print(F("ONLINE (0x")); Serial.print(bmpAddress, HEX); Serial.println(F(")")); }
    else { Serial.println(F("OFFLINE / NOT FOUND")); }
    Serial.print(F("  SCD41:    "));
    Serial.println(scdPresent ? F("ONLINE (0x62)") : F("OFFLINE / NOT FOUND"));
    Serial.print(F("  MAX30101: "));
    Serial.println(maxPresent ? F("ONLINE (0x57)") : F("OFFLINE / NOT FOUND"));
    Serial.print(F("  BLE:      ONLINE (")); Serial.print(F(BLE_DEVICE_NAME)); Serial.println(F(")"));
    Serial.println(F("--------------------------------------------------"));
    lastTelemetryMs = 0; // Force immediate telemetry packet
  }
}

// =============================================================================
// COMMAND PROCESSOR
// =============================================================================

/**
 * @brief Unified command handler for Serial and BLE UART inputs.
 */
void handleCommand(char cmd, Print& out) {
  if (cmd == 'j' || cmd == 'J') {
    outputJson = true;
    out.println(F("[*] Switched to JSON output"));
    lastTelemetryMs = 0;
  } else if (cmd == 'd' || cmd == 'D') {
    outputJson = false;
    out.println(F("[*] Switched to Visual Dashboard"));
    lastTelemetryMs = 0;
  } else if (cmd == 'r' || cmd == 'R') {
    lastTelemetryMs = 0;
  } else if (cmd == 'z' || cmd == 'Z') {
    climate.resetRelativeAltitude();
    out.println(F("[*] Relative altitude zeroed (tare 0.0 m)"));
    lastTelemetryMs = 0;
  } else if (cmd == 'b' || cmd == 'B') {
    out.println(F("[*] Remote reboot commanded. Restarting MCU..."));
    delay(150);
    NVIC_SystemReset();
  } else if (cmd == '?' || cmd == 'h' || cmd == 'H') {
    out.println(F("\n[Commands] 'j'=JSON | 'd'=Dashboard | 'r'=Refresh | 'z'=Zero Alt | 'b'=Reboot"));
  }
}

// =============================================================================
// LOOP
// =============================================================================

void loop() {
  unsigned long now = millis();

  // 1. Heartbeat indicator: toggle Blue LED every 500ms (visible through enclosure)
  if (now - lastHeartbeatMs >= 500) {
    lastHeartbeatMs = now;
    heartbeatState = !heartbeatState;
    digitalWrite(LED_BLUE, heartbeatState ? LOW : HIGH);
  }

  // 2. Immediate reconnection detection
  bool isSerial = (bool)Serial;
  if (isSerial && !wasSerialConnected) {
    wasSerialConnected = true;
    Serial.println(F("\n=================================================="));
    Serial.println(F("  CO2 + Health Monitor: Real-Time Stream Active"));
    Serial.println(F("=================================================="));
    lastTelemetryMs = 0; // Output fresh telemetry immediately!
  } else if (!isSerial) {
    wasSerialConnected = false;
  }

  // 3. Command handlers for dynamic mode switching ('j' = JSON, 'd' = Dashboard, 'r' = Refresh)
  while (Serial.available() > 0) {
    char cmd = static_cast<char>(Serial.read());
    handleCommand(cmd, Serial);
  }

  while (ble.available() > 0) {
    char cmd = static_cast<char>(ble.read());
    handleCommand(cmd, ble.getUart());
  }

  // 4. Drain MAX30101 FIFO buffer and execute real-time PPG state machine
  ppg.update();

  // ISO 80601-2-61: Event-driven immediate push (<100ms) on finger contact or state change
  // Eliminates all 6-second UI display lag on finger release!
  if (ppg.consumeStateChanged()) {
    lastTelemetryMs = 0;
  }

  // 5. Poll climate sensors and execute asynchronous data-ready tasks
  climate.update(now);

  // 6. Continuous periodic telemetry reporting
  if (lastTelemetryMs == 0 || (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS)) {
    lastTelemetryMs = now;
    
    // Output to USB Serial
    if (outputJson) {
      formatJsonTelemetry(Serial);
    } else {
      printDashboardTelemetry(Serial);
    }

    // Output to BLE UART: stream structured JSON to connected mobile app / central
    if (ble.isSubscribed()) {
      formatJsonTelemetry(ble.getUart());
    }
  }
}

