/**
 * @file sensor_climate.h
 * @brief Subsystem manager for Sensirion SCD41 (CO2/Temp/Hum) and Bosch BMP388 (Pressure/Temp).
 * 
 * Implements non-blocking asynchronous sampling, dynamic barometric pressure 
 * compensation, thermal fusion, relative altitude tracking, and sensor health diagnostics.
 */

#ifndef SENSOR_CLIMATE_H
#define SENSOR_CLIMATE_H

#include <Wire.h>
#include <Adafruit_BMP3XX.h>
#include <SensirionI2cScd4x.h>
#include "config.h"
#include "types.h"
#include "dsp_filters.h"

class ClimateSensorManager {
public:
  ClimateSensorManager();

  /**
   * @brief Initializes BMP388 and SCD41 on the specified I2C bus.
   * @param wirePort Reference to the I2C TwoWire bus instance.
   * @param bmpAddress Detected I2C address for BMP388 (0x77, 0x76, or 0 if not detected).
   * @param scdPresent True if Sensirion SCD41 was verified on the I2C bus.
   * @return True if at least one sensor successfully initialized.
   */
  bool begin(TwoWire &wirePort = Wire, uint8_t bmpAddress = 0, bool scdPresent = false);

  /**
   * @brief Non-blocking periodic update handler. Must be called regularly in loop().
   * @param currentMillis Current timestamp from millis().
   */
  void update(unsigned long currentMillis);

  /// Get current aggregated climate data snapshot
  const ClimateData& getData() const { return data_; }

  /// Get diagnostic health flags for climate sensors
  const SensorHealth& getHealth() const { return health_; }

private:
  Adafruit_BMP3XX bmp388_;
  SensirionI2cScd4x scd4x_;

  ClimateData data_;
  SensorHealth health_;

  unsigned long lastScdPollMs_;
  unsigned long lastBmpPollMs_;
  unsigned long lastPressureCompMs_;
  float lastPressureCompHpa_;
  uint32_t scdReadCount_;
  float baselinePressureHpa_;
  uint8_t bmpStartupCount_;
  float bmpStartupSum_;
};

#endif // SENSOR_CLIMATE_H
