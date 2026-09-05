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

// =============================================================================
// GLOBAL MANAGERS
// =============================================================================

ClimateSensorManager climate;
PpgSensorManager ppg;

unsigned long lastTelemetryMs = 0;
bool outputJson = OUTPUT_JSON;

// =============================================================================
// TELEMETRY FORMATTERS
// =============================================================================

/**
 * @brief Outputs a single-line, RFC-compliant JSON object for automated loggers.
 */
void printJsonTelemetry() {
  const ClimateData& c = climate.getData();
  const BiometricData& b = ppg.getData();
  const SensorHealth& h = climate.getHealth();

  Serial.print(F("{\"time_ms\":")); Serial.print(millis());
  
  // Climate telemetry
  Serial.print(F(",\"co2_ppm\":")); Serial.print(c.co2_ppm);
  Serial.print(F(",\"co2_status\":\"")); Serial.print(c.co2_status); Serial.print(F("\""));
  Serial.print(F(",\"co2_warming_up\":")); Serial.print(c.is_warming_up ? F("true") : F("false"));
  Serial.print(F(",\"temp_c\":")); Serial.print(c.temp_c, 1);
  Serial.print(F(",\"temp_bmp_c\":")); Serial.print(c.temp_bmp_c, 1);
  Serial.print(F(",\"humidity_pct\":")); Serial.print(c.humidity_pct, 1);
  Serial.print(F(",\"dew_point_c\":")); Serial.print(c.dew_point_c, 1);
  Serial.print(F(",\"abs_humidity_gm3\":")); Serial.print(c.abs_humidity_gm3, 1);
  Serial.print(F(",\"pressure_hpa\":")); Serial.print(c.pressure_hpa, 1);
  Serial.print(F(",\"pressure_mmhg\":")); Serial.print(c.pressure_mmhg, 1);
  Serial.print(F(",\"altitude_m\":")); Serial.print(c.altitude_m, 1);
  Serial.print(F(",\"rel_altitude_m\":")); Serial.print(c.relative_altitude_m, 2);

  // Biometric telemetry
  Serial.print(F(",\"finger_detected\":")); Serial.print(b.finger_detected ? F("true") : F("false"));
  Serial.print(F(",\"ppg_state\":")); Serial.print(static_cast<int>(b.state));
  Serial.print(F(",\"buffer_pct\":")); Serial.print(b.buffer_progress_pct);
  Serial.print(F(",\"hr_bpm\":"));
  if (b.hr_valid) Serial.print(b.hr_bpm); else Serial.print(F("null"));
  Serial.print(F(",\"hr_bpm_raw\":"));
  if (b.hr_bpm_raw > 0) Serial.print(b.hr_bpm_raw); else Serial.print(F("null"));
  Serial.print(F(",\"hr_valid\":")); Serial.print(b.hr_valid ? F("true") : F("false"));
  Serial.print(F(",\"spo2_pct\":"));
  if (b.spo2_valid) Serial.print(b.spo2); else Serial.print(F("null"));
  Serial.print(F(",\"spo2_raw\":"));
  if (b.spo2_raw > 0) Serial.print(b.spo2_raw); else Serial.print(F("null"));
  Serial.print(F(",\"spo2_valid\":")); Serial.print(b.spo2_valid ? F("true") : F("false"));
  Serial.print(F(",\"perfusion_index\":")); Serial.print(b.perfusion_index, 2);
  Serial.print(F(",\"led_brightness\":")); Serial.print(b.led_brightness);

  // Hardware diagnostics
  Serial.print(F(",\"bmp_ok\":")); Serial.print(h.bmp388_online ? F("true") : F("false"));
  Serial.print(F(",\"scd_ok\":")); Serial.print(h.scd41_online ? F("true") : F("false"));
  Serial.print(F(",\"max_ok\":")); Serial.print(ppg.isOnline() ? F("true") : F("false"));
  Serial.println(F("}"));
}

/**
 * @brief Outputs a formatted visual text dashboard for the Arduino Serial Monitor.
 */
void printDashboardTelemetry() {
  const ClimateData& c = climate.getData();
  const BiometricData& b = ppg.getData();

  Serial.println(F("\n=================================================="));
  Serial.print(F("[UPTIME: ")); Serial.print(millis() / 1000); Serial.println(F(" s]"));
  
  Serial.println(F("--- [CLIMATE] Sensirion SCD41 + Bosch BMP388 ---"));
  Serial.print(F("  CO2:            ")); Serial.print(c.co2_ppm); Serial.print(F(" ppm [")); Serial.print(c.co2_status); Serial.println(F("]"));
  Serial.print(F("  Temperature:    ")); Serial.print(c.temp_c, 1); Serial.print(F(" °C  (BMP388 Ref: ")); Serial.print(c.temp_bmp_c, 1); Serial.println(F(" °C)"));
  Serial.print(F("  Humidity:       ")); Serial.print(c.humidity_pct, 1); Serial.println(F(" %"));
  Serial.print(F("  Dew Point:      ")); Serial.print(c.dew_point_c, 1); Serial.println(F(" °C"));
  Serial.print(F("  Abs. Humidity:  ")); Serial.print(c.abs_humidity_gm3, 1); Serial.println(F(" g/m³"));
  Serial.print(F("  Pressure:       ")); Serial.print(c.pressure_mmhg, 1); Serial.print(F(" mmHg (")); Serial.print(c.pressure_hpa, 1); Serial.println(F(" hPa)"));
  Serial.print(F("  Altitude (ISA): ")); Serial.print(c.altitude_m, 1); Serial.print(F(" m  (Delta: ")); Serial.print(c.relative_altitude_m, 2); Serial.println(F(" m)"));

  Serial.println(F("--- [BIOMETRICS] Maxim MAX30101 (AGC + DSP) ---"));
  Serial.print(F("  Finger Contact: ")); Serial.println(b.finger_detected ? F("YES") : F("NO"));
  Serial.print(F("  Subsystem State:"));
  switch (b.state) {
    case PPG_STATE_NO_FINGER:    Serial.println(F("No Finger Detected")); break;
    case PPG_STATE_CALIBRATING:  Serial.println(F("Calibrating AGC Gain...")); break;
    case PPG_STATE_ACQUIRING:    Serial.print(F("Acquiring 4s Buffer (")); 
                                 Serial.print(b.buffer_progress_pct); Serial.println(F("%)")); break;
    case PPG_STATE_TRACKING:     Serial.println(F("Tracking (Active)")); break;
  }

  Serial.print(F("  Heart Rate:     "));
  if (b.hr_valid) {
    Serial.print(b.hr_bpm); Serial.println(F(" BPM (Filtered)"));
  } else {
    if (b.state == PPG_STATE_CALIBRATING) {
      Serial.println(F("Calibrating AGC..."));
    } else if (b.state == PPG_STATE_ACQUIRING) {
      Serial.print(F("Acquiring (")); Serial.print(b.buffer_progress_pct); Serial.println(F("%)..."));
    } else if (b.state == PPG_STATE_TRACKING) {
      Serial.println(F("Analyzing..."));
    } else {
      Serial.println(F("--"));
    }
  }

  Serial.print(F("  SpO2:           "));
  if (b.spo2_valid) {
    Serial.print(b.spo2); Serial.println(F(" %"));
  } else {
    if (b.state == PPG_STATE_CALIBRATING) {
      Serial.println(F("Calibrating AGC..."));
    } else if (b.state == PPG_STATE_ACQUIRING) {
      Serial.print(F("Acquiring (")); Serial.print(b.buffer_progress_pct); Serial.println(F("%)..."));
    } else if (b.state == PPG_STATE_TRACKING) {
      Serial.println(F("Analyzing..."));
    } else {
      Serial.println(F("--"));
    }
  }

  Serial.print(F("  Perfusion Index:")); Serial.print(b.perfusion_index, 2); Serial.println(F(" %"));
  Serial.print(F("  AGC Drive:      ")); Serial.print(b.led_brightness); Serial.println(F(" / 255"));
  Serial.println(F("=================================================="));
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
    Serial.println(F("--------------------------------------------------"));
    lastTelemetryMs = 0; // Force immediate telemetry packet
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

  // 3. Serial command handler for dynamic mode switching ('j' = JSON, 'd' = Dashboard, 'r' = Refresh)
  while (Serial.available() > 0) {
    char cmd = static_cast<char>(Serial.read());
    if (cmd == 'j' || cmd == 'J') {
      outputJson = true;
      lastTelemetryMs = 0; // Trigger immediate JSON packet without plain text banner
    } else if (cmd == 'd' || cmd == 'D') {
      outputJson = false;
      Serial.println(F("[*] Switched to Visual Dashboard"));
      lastTelemetryMs = 0;
    } else if (cmd == 'r' || cmd == 'R') {
      lastTelemetryMs = 0;
    } else if (cmd == '?' || cmd == 'h' || cmd == 'H') {
      Serial.println(F("\n[Commands] 'j' = JSON | 'd' = Dashboard | 'r' = Send now"));
    }
  }

  // 4. Drain MAX30101 FIFO buffer and execute real-time PPG state machine
  ppg.update();

  // 5. Poll climate sensors and execute asynchronous data-ready tasks
  climate.update(now);

  // 6. Continuous periodic telemetry reporting
  if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryMs = now;
    if (outputJson) {
      printJsonTelemetry();
    } else {
      printDashboardTelemetry();
    }
  }
}
