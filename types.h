/**
 * @file types.h
 * @brief Data structures, enumerations, and telemetry models.
 * 
 * Defines standard contracts between hardware managers, DSP algorithms,
 * and communication outputs.
 */

#ifndef TYPES_H
#define TYPES_H

#include <Arduino.h>

/**
 * @brief Finite State Machine states for the PPG pulse oximeter.
 */
enum PpgState : uint8_t {
  PPG_STATE_NO_FINGER = 0,    ///< No finger present on sensor (low-power proximity check)
  PPG_STATE_CALIBRATING = 1,  ///< Finger detected, AGC adjusting LED current to target ADC band
  PPG_STATE_ACQUIRING = 2,    ///< LED current locked, acquiring initial clean 100-sample buffer
  PPG_STATE_TRACKING = 3      ///< Active biometric tracking with harmonic & median filtering
};

/**
 * @brief Biometric metrics from MAX30101 optical subsystem.
 */
struct BiometricData {
  bool finger_detected = false;       ///< True when a finger is firmly placed on the sensor
  PpgState state = PPG_STATE_NO_FINGER; ///< Current FSM state of the PPG manager
  uint8_t led_brightness = 60;        ///< Current dynamically tuned LED brightness (0-255)

  int32_t hr_bpm_raw = 0;             ///< Unfiltered instantaneous HR from Maxim algorithm
  int32_t hr_bpm = 0;                 ///< Cleaned HR after anti-doubling, median, and EMA filtering
  bool hr_valid = false;              ///< True if HR is within physiological bounds and validated

  int32_t spo2_raw = 0;               ///< Unfiltered instantaneous SpO2 from Maxim algorithm
  int32_t spo2 = 0;                   ///< Cleaned SpO2 after rolling median and EMA filtering
  bool spo2_valid = false;            ///< True if SpO2 is within physiological bounds and validated

  float perfusion_index = 0.0f;       ///< Perfusion Index (PI %) = (AC / DC) * 100%
  bool signal_quality_ok = false;     ///< True if Perfusion Index is above MIN_PERFUSION_INDEX
  uint8_t buffer_progress_pct = 0;    ///< Initial buffer filling progress percentage (0-100%)
};

/**
 * @brief Climate and atmospheric metrics from SCD41 and BMP388.
 */
struct ClimateData {
  uint16_t co2_ppm = 0;               ///< Carbon dioxide concentration in parts-per-million
  const char* co2_status = "Warming Up"; ///< Air quality qualitative assessment category
  bool is_warming_up = true;          ///< True during the initial 4 measurement cycles

  float temp_c = 0.0f;                ///< Ambient temperature (°C) from SCD41 with thermal offset
  float temp_bmp_c = 0.0f;            ///< Reference temperature (°C) from cold BMP388
  float humidity_pct = 0.0f;          ///< Relative humidity (%) from SCD41
  float dew_point_c = 0.0f;           ///< Calculated dew point temperature (°C)
  float abs_humidity_gm3 = 0.0f;      ///< Calculated absolute humidity (g/m³)

  float pressure_hpa = 0.0f;          ///< Barometric pressure in hectopascals (hPa)
  float pressure_mmhg = 0.0f;         ///< Barometric pressure in millimeters of mercury (mmHg)
  float altitude_m = 0.0f;            ///< Absolute altitude above sea level (ISA model)
  float relative_altitude_m = 0.0f;   ///< Relative height delta (m) from power-on baseline
};

/**
 * @brief Diagnostic health status of hardware sensors on the I2C bus.
 */
struct SensorHealth {
  bool bmp388_online = false;         ///< True if Bosch BMP388 responded and initialized
  bool scd41_online = false;          ///< True if Sensirion SCD41 responded and initialized
  bool max30101_online = false;       ///< True if Maxim MAX30101 responded and initialized

  /// Returns true only if all 3 hardware sensors are operational
  bool allOk() const {
    return bmp388_online && scd41_online && max30101_online;
  }
};

#endif // TYPES_H
