/**
 * @file dsp_filters.h
 * @brief Digital Signal Processing (DSP) algorithms and physical formulas.
 * 
 * Provides rolling median filtering, harmonic artifact rejection (anti-doubling),
 * exponential moving average (EMA) smoothing, perfusion index (PI) estimation,
 * and standard meteorological formulas.
 */

#ifndef DSP_FILTERS_H
#define DSP_FILTERS_H

#include <Arduino.h>
#include <math.h>

/**
 * @brief Rolling Median Filter with a fixed window of N elements.
 * 
 * Extracts the true statistical median from a sliding window without
 * phase lag or impulse distortion. Ideal for eliminating isolated spikes.
 * 
 * @tparam T Numerical data type (float, int32_t, etc.)
 * @tparam N Window capacity (recommended to be an odd integer, e.g. 5)
 */
template<typename T, size_t N>
class RollingMedian {
public:
  RollingMedian() : head_(0), count_(0) {}

  /**
   * @brief Insert a new sample into the circular buffer.
   * @param val New sample value.
   */
  void add(T val) {
    buffer_[head_] = val;
    head_ = (head_ + 1) % N;
    if (count_ < N) {
      count_++;
    }
  }

  /**
   * @brief Compute the median of all samples currently in the buffer.
   * @return The median value. If empty, returns default T().
   */
  T getMedian() const {
    if (count_ == 0) return T();

    T sorted[N];
    for (size_t i = 0; i < count_; ++i) {
      sorted[i] = buffer_[i];
    }

    // Insertion sort: optimal for small N (N=5 takes <= 10 comparisons)
    for (size_t i = 1; i < count_; ++i) {
      T key = sorted[i];
      int j = static_cast<int>(i) - 1;
      while (j >= 0 && sorted[j] > key) {
        sorted[j + 1] = sorted[j];
        j--;
      }
      sorted[j + 1] = key;
    }

    return sorted[count_ / 2];
  }

  /// Reset the buffer state
  void reset() {
    head_ = 0;
    count_ = 0;
  }

  /// Returns true if the buffer has accumulated at least N samples
  bool isFull() const {
    return count_ >= N;
  }

  /// Number of valid samples currently stored
  size_t count() const {
    return count_;
  }

private:
  T buffer_[N];
  size_t head_;
  size_t count_;
};

/**
 * @brief Single-pole Exponential Moving Average (EMA) low-pass filter.
 * 
 * Equation: y[n] = alpha * x[n] + (1 - alpha) * y[n-1]
 */
class EmaFilter {
public:
  /**
   * @param alpha Smoothing factor between 0.0 (maximum lag/smoothing) and 1.0 (no filtering).
   */
  explicit EmaFilter(float alpha = 0.25f) : alpha_(alpha), value_(0.0f), initialized_(false) {}

  /**
   * @brief Process a new input sample.
   * @param input Raw input value.
   * @return Filtered output value.
   */
  float update(float input) {
    if (!initialized_) {
      value_ = input;
      initialized_ = true;
    } else {
      value_ = (alpha_ * input) + ((1.0f - alpha_) * value_);
    }
    return value_;
  }

  /// Reset filter state
  void reset() {
    value_ = 0.0f;
    initialized_ = false;
  }

  /// Get current filtered value
  float get() const { return value_; }

  /// Change alpha parameter dynamically
  void setAlpha(float alpha) { alpha_ = alpha; }

private:
  float alpha_;
  float value_;
  bool initialized_;
};

/**
 * @brief Rejects dicrotic notch doubling harmonics.
 * 
 * In photoplethysmography (PPG), secondary dicrotic waves often cause the peak 
 * detector to trigger twice per heartbeat, doubling the reported rate (e.g. 68 -> 136).
 * This function detects if the current value is an exact 2x harmonic of the previous
 * stable heart rate and corrects it by halving back to fundamental rate.
 * 
 * @param currentVal Current raw instantaneous value from the peak detector.
 * @param prevStableVal Last confirmed stable value.
 * @param tolerance Allowable deviation band around 2.0x (e.g. 0.15 = 15%).
 * @return Harmonic-corrected value.
 */
float filterHarmonics(float currentVal, float prevStableVal, float tolerance = 0.15f);

/**
 * @brief State-of-the-art PPG peak detection, heart rate calculation, and SpO2 extraction.
 * 
 * Implements:
 * - Zero-phase symmetric smoothing FIR filter
 * - Dynamic physiological refractory period tied to instantaneous heart rate
 * - Adaptive peak thresholding for dicrotic notch suppression
 * - Cardiac cycle linear baseline detrending (Maxim AN6845)
 * - Cycle-by-cycle R-ratio extraction with median selection for SpO2
 * - True pulsatile Perfusion Index (PI %) across confirmed beats
 * 
 * @param irBuffer Pointer to raw IR ADC samples.
 * @param redBuffer Pointer to raw Red ADC samples.
 * @param length Number of samples in the buffer (e.g. 100).
 * @param fs Effective sampling frequency (e.g. 25 Hz).
 * @param outHr Output heart rate in BPM.
 * @param outHrValid Output flag: true if heart rate is physiologically valid.
 * @param outSpo2 Output SpO2 percentage.
 * @param outSpo2Valid Output flag: true if SpO2 is physiologically valid.
 * @param outPi Output Perfusion Index (PI %).
 * @param outPiValid Output flag: true if PI is valid.
 * @param prevStableHr Prior confirmed heart rate for adaptive refractory scaling.
 */
void processPpgSignal(
    const uint32_t* irBuffer,
    const uint32_t* redBuffer,
    size_t length,
    int fs,
    int32_t& outHr,
    bool& outHrValid,
    int32_t& outSpo2,
    bool& outSpo2Valid,
    float& outPi,
    bool& outPiValid,
    float prevStableHr = 0.0f);

/**
 * @brief Computes the Perfusion Index (PI %) from an optical PPG buffer.
 * 
 * PI = (AC / DC) * 100%
 * Medical standard for evaluating peripheral pulsatile signal strength.
 * 
 * @param buffer Pointer to raw infrared or red ADC samples.
 * @param length Number of samples in the buffer.
 * @return Perfusion index as a percentage.
 */
float calculatePerfusionIndex(const uint32_t* buffer, size_t length);

/**
 * @brief Calculates Dew Point temperature (°C) using the August-Roche-Magnus approximation.
 * 
 * Valid for environmental temperatures between -40°C and 60°C.
 * 
 * @param tempC Ambient temperature in degrees Celsius.
 * @param humidityPct Relative humidity in percent (0.0 to 100.0).
 * @return Dew point temperature in degrees Celsius.
 */
float calculateDewPoint(float tempC, float humidityPct);

/**
 * @brief Calculates Absolute Humidity (g/m³) from temperature and relative humidity.
 * 
 * Based on the ideal gas law for water vapor and Bolton's saturation vapor pressure formula.
 * 
 * @param tempC Ambient temperature in degrees Celsius.
 * @param humidityPct Relative humidity in percent (0.0 to 100.0).
 * @return Water vapor mass concentration in grams per cubic meter (g/m³).
 */
float calculateAbsoluteHumidity(float tempC, float humidityPct);

/**
 * @brief Converts pressure from hectopascals (hPa) to millimeters of mercury (mmHg).
 * 
 * @param pressureHpa Barometric pressure in hPa.
 * @return Pressure in mmHg.
 */
float pressureHpaToMmHg(float pressureHpa);

/**
 * @brief Calculates barometric altitude (meters) using the International Standard Atmosphere (ISA) formula.
 * 
 * @param pressureHpa Measured barometric pressure in hPa.
 * @param seaLevelHpa Reference pressure at sea level (default: 1013.25 hPa).
 * @return Altitude above the reference pressure level in meters.
 */
float calculateAltitude(float pressureHpa, float seaLevelHpa = 1013.25f);

/**
 * @brief Maps CO2 concentration (ppm) to standardized indoor air quality status string.
 * 
 * @param co2 CO2 concentration in ppm.
 * @return Human-readable air quality category descriptor.
 */
const char* getCO2Status(uint16_t co2);

#endif // DSP_FILTERS_H
