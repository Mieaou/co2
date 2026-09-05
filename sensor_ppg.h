/**
 * @file sensor_ppg.h
 * @brief Subsystem manager for the Maxim MAX30101 pulse oximeter.
 * 
 * Features:
 * - Dynamic AGC (Automatic Gain Control) auto-exposure loop
 * - Finite State Machine (No Finger -> Calibrating -> Acquiring -> Tracking)
 * - Sequential FIFO queue extraction (getFIFORed / getFIFOIR)
 * - Anti-doubling dicrotic notch harmonic rejection
 * - 5-point rolling median filtering & EMA smoothing
 * - Perfusion Index (PI %) signal strength verification
 */

#ifndef SENSOR_PPG_H
#define SENSOR_PPG_H

#include <Wire.h>
#include "MAX30105.h"
#include "config.h"
#include "types.h"
#include "dsp_filters.h"

class PpgSensorManager {
public:
  PpgSensorManager();

  /**
   * @brief Initializes MAX30101 optical sensor and sets up baseline configuration.
   * @param wirePort Reference to the I2C bus.
   * @param maxPresent True if MAX30101 was verified on the I2C bus.
   * @return True if sensor was found and initialized.
   */
  bool begin(TwoWire &wirePort = Wire, bool maxPresent = false);

  /**
   * @brief Drains FIFO buffer and executes state machine. Call as frequently as possible in loop().
   */
  void update();

  /// Get current biometric data snapshot
  const BiometricData& getData() const { return data_; }

  /// Check if MAX30101 is online
  bool isOnline() const { return online_; }

private:
  MAX30105 sensor_;
  bool online_;

  uint32_t irBuffer_[BUFFER_SIZE];
  uint32_t redBuffer_[BUFFER_SIZE];
  size_t sampleCount_;

  BiometricData data_;
  uint8_t currentLedBrightness_;
  float lastStableHr_;

  // AGC timing, stability and hysteresis management
  unsigned long lastAgcAdjustMs_;
  uint8_t agcStableCount_;
  unsigned long outOfRangeStartMs_;
  uint8_t fingerReleaseCounter_;

  // Medical display holdover timestamps
  unsigned long lastValidHrMs_;
  unsigned long lastValidSpo2Ms_;

  RollingMedian<float, MEDIAN_WINDOW_SIZE> hrMedian_;
  RollingMedian<float, MEDIAN_WINDOW_SIZE> spo2Median_;
  EmaFilter hrEma_;
  EmaFilter spo2Ema_;

  bool handleAgcCalibration(uint32_t currentIr, uint32_t currentRed, unsigned long nowMs);
  void handleAgcTracking(uint32_t currentIr, unsigned long nowMs);
  void resetBiometrics();
};

#endif // SENSOR_PPG_H
