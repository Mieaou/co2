/**
 * @file config.h
 * @brief System configuration, thresholds, and compile-time constants.
 * 
 * Central repository of all tunable parameters for the Multi-Sensor CO2 
 * and Health Monitor system. Designed for Seeed XIAO nRF52840 Sense.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// =============================================================================
// OUTPUT & TELEMETRY CONFIGURATION
// =============================================================================

/**
 * @brief Telemetry mode selection:
 * true  = Single-line RFC-compliant JSON (ideal for loggers, Node-RED, Python, Grafana)
 * false = Formatted human-readable visual dashboard for Serial Monitor
 */
#define OUTPUT_JSON false

/// Serial communication baud rate
constexpr unsigned long SERIAL_BAUD_RATE = 115200;

/// Interval between periodic telemetry output reports (milliseconds)
constexpr unsigned long TELEMETRY_INTERVAL_MS = 3000;

// =============================================================================
// BLE (BLUETOOTH LOW ENERGY) CONFIGURATION
// =============================================================================

/// BLE Advertised Device Name
#define BLE_DEVICE_NAME "CO2-Health-Monitor"

/// Manufacturer & Model strings for Device Information Service (DIS)
#define BLE_MANUFACTURER_STR "Seeed XIAO"
#define BLE_MODEL_STR        "nRF52840-CO2-Health"

/// Transmit power in dBm (+4 dBm gives max range while remaining low power)
constexpr int8_t BLE_TX_POWER_DBM = 4;

/// Advertising interval fast (in 0.625ms units: 32 = 20ms)
constexpr uint16_t BLE_ADV_FAST_INTERVAL = 32;

/// Advertising interval slow (in 0.625ms units: 244 = 152.5ms)
constexpr uint16_t BLE_ADV_SLOW_INTERVAL = 244;

/// Fast advertising timeout in seconds
constexpr uint16_t BLE_ADV_FAST_TIMEOUT_S = 30;

// =============================================================================
// SENSOR I2C ADDRESSES
// =============================================================================

constexpr uint8_t BMP388_I2C_ADDR_PRIMARY   = 0x77;
constexpr uint8_t BMP388_I2C_ADDR_SECONDARY = 0x76;
constexpr uint8_t SCD41_I2C_ADDR            = 0x62;
constexpr uint8_t MAX30101_I2C_ADDR         = 0x57;

// =============================================================================
// CLIMATE SUBSYSTEM (SCD41 & BMP388) CONFIGURATION
// =============================================================================

/// Polling interval for SCD41 data-ready register (milliseconds)
constexpr unsigned long SCD41_POLL_INTERVAL_MS = 250;

/// Number of initial measurement cycles considered warm-up period (5s each = 20s)
constexpr uint8_t SCD41_WARMUP_CYCLES = 4;

/// Internal sensor temperature offset in °C for enclosure self-heating compensation
constexpr float SCD41_TEMPERATURE_OFFSET = 2.0f;

/// Sensirion Automatic Self-Calibration (ASC): false = disabled (recommended for closed 24/7 spaces)
constexpr bool SCD41_ENABLE_ASC = false;

/// Standard sea-level reference pressure (hPa) for ISA barometric altitude
constexpr float STANDARD_SEA_LEVEL_HPA = 1013.25f;

// =============================================================================
// BIOMETRICS SUBSYSTEM (MAX30101) CONFIGURATION
// =============================================================================

/// Default LED brightness during active measurement (0-255, approx 12mA at 60)
constexpr uint8_t PPG_LED_BRIGHTNESS_DEFAULT = 60;

/// Minimum and maximum allowable LED brightness for the AGC loop
constexpr uint8_t PPG_LED_BRIGHTNESS_MIN = 12;
constexpr uint8_t PPG_LED_BRIGHTNESS_MAX = 240;

/// Step size for dynamic LED current AGC adjustments (fine vs coarse)
constexpr uint8_t PPG_AGC_STEP = 4;
constexpr uint8_t PPG_AGC_FAST_STEP = 16;
constexpr uint8_t PPG_AGC_FINE_STEP = 4;
constexpr uint32_t PPG_AGC_COARSE_THRESHOLD = 30000;

/// Linear target range for IR DC baseline (18-bit ADC full scale is 262,143)
constexpr uint32_t PPG_AGC_TARGET_MIN = 45000;
constexpr uint32_t PPG_AGC_TARGET_MAX = 165000;

/// Minimum acceptable Red optical amplitude for SpO2 AN6845 extraction
constexpr uint32_t PPG_MIN_RED_SNR_THRESHOLD = 8000;

/// Infrared raw threshold for finger touch detection (entry threshold)
constexpr uint32_t PPG_FINGER_THRESHOLD = 50000;

/// Infrared raw threshold for finger release (hysteresis exit threshold)
constexpr uint32_t PPG_FINGER_RELEASE_THRESHOLD = 40000;

/// Number of consecutive low samples required to confirm finger removal (debounce ~320ms at 25 Hz)
constexpr uint8_t PPG_FINGER_RELEASE_DEBOUNCE_SAMPLES = 8;

/// Holdover duration in ms for retaining valid biometrics across momentary 1-second noise
constexpr unsigned long BIOMETRIC_HOLDOVER_MS = 3000;

/// Hardware sample averaging factor (1, 2, 4, 8, 16, 32)
constexpr uint8_t PPG_SAMPLE_AVERAGE = 4;

/// LED Mode (2 = Red + IR for SpO2 and HR)
constexpr uint8_t PPG_LED_MODE = 2;

/// ADC sample rate in Hz (100 Hz / sampleAverage 4 = 25 effective Hz)
constexpr int PPG_SAMPLE_RATE_HZ = 100;

/// ADC pulse width in microseconds (411 us = 18-bit resolution)
constexpr int PPG_PULSE_WIDTH = 411;

/// ADC full-scale range in counts
constexpr int PPG_ADC_RANGE = 4096;

/// Effective sampling frequency of the FIFO buffer in Hz
constexpr int PPG_EFFECTIVE_FS = 25;

/// Total buffer capacity for biometric analysis (100 samples = 4 seconds at 25 Hz)
constexpr size_t PPG_BUFFER_SIZE = 100;
#ifndef BUFFER_SIZE
#define BUFFER_SIZE PPG_BUFFER_SIZE
#endif

/// Sliding step in samples for continuous recalculation (25 samples = 1 second)
constexpr size_t PPG_SHIFT_SAMPLES = 25;

// =============================================================================
// DSP FILTERING CONFIGURATION
// =============================================================================

/// Size of the rolling median filter window (must be odd for true median)
constexpr size_t MEDIAN_WINDOW_SIZE = 5;

/// Exponential Moving Average (EMA) smoothing coefficient for Heart Rate (0.0 - 1.0)
/// Higher alpha = faster response, less drift accumulation from bad readings
constexpr float HR_EMA_ALPHA = 0.45f;

/// Exponential Moving Average (EMA) smoothing coefficient for SpO2 (0.0 - 1.0)
constexpr float SPO2_EMA_ALPHA = 0.20f;

/// Tolerance band for anti-doubling / dicrotic notch harmonic rejection (e.g. 15%)
constexpr float HARMONIC_TOLERANCE = 0.15f;

/// Physiological bounds for heart rate (beats per minute)
constexpr int32_t HR_MIN_BPM = 30;
constexpr int32_t HR_MAX_BPM = 220;

/// Physiological bounds for blood oxygen saturation (percentage)
constexpr int32_t SPO2_MIN_PCT = 70;
constexpr int32_t SPO2_MAX_PCT = 100;

/// Minimum allowable Perfusion Index (PI %) for reliable biometric estimation
constexpr float MIN_PERFUSION_INDEX = 0.15f;

/// Minimum peak-to-peak AC amplitude (counts) to differentiate living pulsatile tissue from static cloth/bed noise
constexpr float PPG_MIN_DYNAMIC_RANGE = 400.0f;

/// Minimum cardiac cycle AC amplitude for IR and Red channels
constexpr float PPG_MIN_AC_AMPLITUDE_IR = 200.0f;
constexpr float PPG_MIN_AC_AMPLITUDE_RED = 100.0f;

#endif // CONFIG_H
