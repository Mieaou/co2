/**
 * @file sensor_climate.cpp
 * @brief Implementation of the SCD41 and BMP388 climate subsystem manager.
 */

#include "sensor_climate.h"

ClimateSensorManager::ClimateSensorManager()
  : lastScdPollMs_(0),
    lastBmpPollMs_(0),
    lastPressureCompMs_(0),
    lastPressureCompHpa_(0.0f),
    scdReadCount_(0),
    baselinePressureHpa_(0.0f),
    bmpStartupCount_(0),
    bmpStartupSum_(0.0f) {
}

bool ClimateSensorManager::begin(TwoWire &wirePort, uint8_t bmpAddress, bool scdPresent) {
  // 1. Initialize Bosch BMP388 (only if validated address was detected)
  if (bmpAddress != 0 && bmp388_.begin_I2C(bmpAddress, &wirePort)) {
    bmp388_.setTemperatureOversampling(BMP3_OVERSAMPLING_8X);
    bmp388_.setPressureOversampling(BMP3_OVERSAMPLING_8X); // 8x oversampling for ultra-low noise
    bmp388_.setIIRFilterCoeff(BMP3_IIR_FILTER_COEFF_7);    // COEFF_7 for sub-10cm desk-to-shelf altitude stability
    bmp388_.setOutputDataRate(BMP3_ODR_12_5_HZ);           // 12.5 Hz ODR: optimal for 1 Hz polling, eliminates self-heating
    health_.bmp388_online = true;
    Serial.print(F("[+] BMP388 initialized at 0x"));
    Serial.println(bmpAddress, HEX);
  } else {
    health_.bmp388_online = false;
    Serial.println(F("[-] BMP388 not detected (offline)"));
  }

  // 2. Initialize Sensirion SCD41 (only if address 0x62 responded)
  if (scdPresent) {
    scd4x_.begin(wirePort, SCD41_I2C_ADDR);
    scd4x_.stopPeriodicMeasurement();
    delay(500); // Sensirion datasheet Section 3.5.2 requires 500ms execution time

    // Configure thermal offset and Automatic Self-Calibration (ASC)
    scd4x_.setTemperatureOffset(SCD41_TEMPERATURE_OFFSET);
    scd4x_.setAutomaticSelfCalibrationEnabled(SCD41_ENABLE_ASC ? 1 : 0);

    if (scd4x_.startPeriodicMeasurement() == 0) {
      health_.scd41_online = true;
      Serial.println(F("[+] SCD41 initialized, periodic measurement started (5s period)"));
    } else {
      health_.scd41_online = false;
      Serial.println(F("[-] SCD41 failed to start periodic measurement!"));
    }
  } else {
    health_.scd41_online = false;
    Serial.println(F("[-] SCD41 not detected (offline)"));
  }

  return health_.bmp388_online || health_.scd41_online;
}

void ClimateSensorManager::update(unsigned long currentMillis) {
  // ---------------------------------------------------------------------------
  // 1. BMP388 Periodic Acquisition (Every 1000ms)
  // ---------------------------------------------------------------------------
  if (health_.bmp388_online && (currentMillis - lastBmpPollMs_ >= 1000)) {
    lastBmpPollMs_ = currentMillis;

    if (bmp388_.performReading()) {
      data_.temp_bmp_c = bmp388_.temperature;
      data_.pressure_hpa = bmp388_.pressure / 100.0f;
      data_.pressure_mmhg = pressureHpaToMmHg(data_.pressure_hpa);

      // Establish baseline pressure after filter settles (average first 4 readings)
      if (baselinePressureHpa_ <= 0.0f) {
        bmpStartupSum_ += data_.pressure_hpa;
        bmpStartupCount_++;
        if (bmpStartupCount_ >= 4) {
          baselinePressureHpa_ = bmpStartupSum_ / 4.0f;
        }
      }

      // Calculate absolute ISA altitude and relative height delta
      data_.altitude_m = calculateAltitude(data_.pressure_hpa, STANDARD_SEA_LEVEL_HPA);
      data_.relative_altitude_m = (baselinePressureHpa_ > 0.0f) 
                                    ? calculateAltitude(data_.pressure_hpa, baselinePressureHpa_) 
                                    : 0.0f;

      // Feed real-time barometric pressure to SCD41 for Boyle-Mariotte law compensation
      // Rate-limited per Sensirion datasheet (not more than once every 5 seconds; here >= 1 hPa delta or 30s)
      if (health_.scd41_online) {
        if (fabsf(data_.pressure_hpa - lastPressureCompHpa_) >= 1.0f || 
            (currentMillis - lastPressureCompMs_ >= 30000) || 
            lastPressureCompMs_ == 0) {
          lastPressureCompHpa_ = data_.pressure_hpa;
          lastPressureCompMs_ = currentMillis;

          // Sensirion SCD41 valid ambient pressure range: 70,000 to 120,000 Pa (700 to 1200 hPa)
          uint32_t pressPa = static_cast<uint32_t>(bmp388_.pressure);
          if (pressPa >= 70000 && pressPa <= 120000) {
            scd4x_.setAmbientPressure(pressPa);
          }
        }
      }
    }
  }

  // ---------------------------------------------------------------------------
  // 2. SCD41 Non-Blocking Data Ready Polling (Every 250ms)
  // ---------------------------------------------------------------------------
  if (health_.scd41_online && (currentMillis - lastScdPollMs_ >= SCD41_POLL_INTERVAL_MS)) {
    lastScdPollMs_ = currentMillis;

    bool scdReady = false;
    if (scd4x_.getDataReadyStatus(scdReady) == 0 && scdReady) {
      uint16_t rawCo2 = 0;
      float rawTemp = 0.0f;
      float rawHum = 0.0f;

      if (scd4x_.readMeasurement(rawCo2, rawTemp, rawHum) == 0) {
        data_.co2_ppm = rawCo2;
        data_.temp_c = rawTemp;
        data_.humidity_pct = rawHum;

        scdReadCount_++;
        if (scdReadCount_ >= SCD41_WARMUP_CYCLES) {
          data_.is_warming_up = false;
        }

        // Calculate derived psychrometric values
        data_.dew_point_c = calculateDewPoint(data_.temp_c, data_.humidity_pct);
        data_.abs_humidity_gm3 = calculateAbsoluteHumidity(data_.temp_c, data_.humidity_pct);
        data_.co2_status = getCO2Status(data_.is_warming_up ? 0 : data_.co2_ppm);
      }
    }
  }
}
