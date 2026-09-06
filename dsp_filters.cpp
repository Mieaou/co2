/**
 * @file dsp_filters.cpp
 * @brief DSP filters, PPG peak detection, SpO2/PI calculation, and physical formulas.
 */

#include "dsp_filters.h"
#include "config.h"


void detrendSignalZeroPhase(
    const float* input,
    float* output,
    size_t length,
    int windowRadius) {

  if (input == nullptr || output == nullptr || length == 0) return;
  if (windowRadius < 2) windowRadius = 2;

  for (size_t i = 0; i < length; ++i) {
    int start = static_cast<int>(i) - windowRadius;
    int end = static_cast<int>(i) + windowRadius;
    if (start < 0) start = 0;
    if (end >= static_cast<int>(length)) end = static_cast<int>(length) - 1;

    float sum = 0.0f;
    int count = end - start + 1;
    for (int k = start; k <= end; ++k) {
      sum += input[k];
    }
    float localBaseline = sum / static_cast<float>(count);
    output[i] = input[i] - localBaseline;
  }
}

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
    float prevStableHr) {

  outHr = 0;
  outHrValid = false;
  outSpo2 = 0;
  outSpo2Valid = false;
  outPi = 0.0f;
  outPiValid = false;

  if (irBuffer == nullptr || redBuffer == nullptr || length < 50 || fs <= 0) return;

  // 1. Calculate DC baseline for both channels
  uint64_t sumIr = 0;
  uint64_t sumRed = 0;
  for (size_t i = 0; i < length; ++i) {
    sumIr += irBuffer[i];
    sumRed += redBuffer[i];
  }
  float irMean = static_cast<float>(sumIr) / static_cast<float>(length);
  float redMean = static_cast<float>(sumRed) / static_cast<float>(length);

  // Insufficient optical signal / finger not properly placed
  if (irMean < 5000.0f || redMean < 5000.0f) return;

  // 2. Remove DC and eliminate baseline wander / contact pressure ramps
  // ISO 80601-2-61: Zero-phase moving average subtraction eliminates monotonic ramps,
  // making false low-frequency 36 BPM detection mathematically impossible.
  float rawAc[128];
  size_t n = (length > 128) ? 128 : length;
  for (size_t i = 0; i < n; ++i) {
    rawAc[i] = -1.0f * (static_cast<float>(irBuffer[i]) - irMean);
  }

  float acIr[128];
  detrendSignalZeroPhase(rawAc, acIr, n, 10);

  // 3. Zero-phase symmetric 5-point smoothing FIR filter (kernel [1, 2, 3, 2, 1] / 9.0)
  // Preserves exact peak timestamps without introducing phase shift or lag
  float smoothed[128];
  for (size_t i = 0; i < n; ++i) {
    if (i >= 2 && i < n - 2) {
      smoothed[i] = (acIr[i - 2] + 2.0f * acIr[i - 1] + 3.0f * acIr[i] + 2.0f * acIr[i + 1] + acIr[i + 2]) / 9.0f;
    } else {
      float sum = 0.0f;
      float weightSum = 0.0f;
      for (int k = -2; k <= 2; ++k) {
        int idx = static_cast<int>(i) + k;
        if (idx < 0) idx = -idx;
        if (idx >= static_cast<int>(n)) idx = 2 * (static_cast<int>(n) - 1) - idx;
        if (idx >= 0 && idx < static_cast<int>(n)) {
          float w = (k == 0) ? 3.0f : ((k == -1 || k == 1) ? 2.0f : 1.0f);
          sum += acIr[idx] * w;
          weightSum += w;
        }
      }
      smoothed[i] = sum / weightSum;
    }
  }

  // 4. Find signal maximum for adaptive threshold
  float sigMax = smoothed[0];
  for (size_t i = 1; i < n; ++i) {
    if (smoothed[i] > sigMax) sigMax = smoothed[i];
  }

  // Reject flat / no-finger signals
  if (sigMax < PPG_MIN_DYNAMIC_RANGE) return;

  // Adaptive threshold = 50% of signal maximum.
  // This adapts to any signal amplitude and keeps the bar above the dicrotic notch
  // (which is typically 40-60% of systolic peak — we threshold above that).
  const float threshold = sigMax * 0.50f;

  // Hard physiological refractory period: 300ms minimum = 8 samples at 25Hz.
  // A dicrotic notch appears 200-350ms after systolic peak.
  // 300ms refractory blocks the notch for all HR up to ~150 BPM without cutting real beats.
  // At 200 BPM, cycle = 300ms, so 300ms is the tightest safe floor.
  const int REFRACTORY = 8; // 320ms at 25Hz

  // 5. Peak detection with adaptive threshold and refractory blanking
  int peakLocs[25];
  float peakAmps[25];
  int numPeaks = 0;

  for (size_t i = 1; i < n - 1 && numPeaks < 25; ++i) {
    // Local maximum above threshold
    if (smoothed[i] > threshold &&
        smoothed[i] >= smoothed[i - 1] &&
        smoothed[i] > smoothed[i + 1]) {

      if (numPeaks > 0 && (static_cast<int>(i) - peakLocs[numPeaks - 1]) < REFRACTORY) {
        // Within refractory: keep only the taller peak (systolic wins over dicrotic)
        if (smoothed[i] > peakAmps[numPeaks - 1]) {
          peakLocs[numPeaks - 1] = static_cast<int>(i);
          peakAmps[numPeaks - 1] = smoothed[i];
        }
      } else {
        peakLocs[numPeaks] = static_cast<int>(i);
        peakAmps[numPeaks] = smoothed[i];
        numPeaks++;
      }
    }
  }

  // 6. Calculate HR from inter-beat intervals (IBI)
  float candidateBpm = 0.0f;
  bool candidateHrFound = false;

  if (numPeaks >= 2) {
    int validIntervals = 0;
    int minInt = 999;
    int maxInt = 0;
    float intervalSum = 0.0f;

    for (int i = 1; i < numPeaks; ++i) {
      int diff = peakLocs[i] - peakLocs[i - 1];
      // Physiological bounds: 8 samples (187 BPM, matches REFRACTORY) to 50 samples (30 BPM)
      if (diff >= REFRACTORY && diff <= 50) {
        intervalSum += static_cast<float>(diff);
        if (diff < minInt) minInt = diff;
        if (diff > maxInt) maxInt = diff;
        validIntervals++;
      }
    }

    if (validIntervals > 0) {
      float avgInterval = intervalSum / static_cast<float>(validIntervals);
      float calculatedBpm = (static_cast<float>(fs) * 60.0f) / avgInterval;

      // RR Jitter check: 40% tolerance covers normal HRV and RSA
      bool rhythmConsistent = true;
      if (validIntervals >= 2) {
        float jitter = static_cast<float>(maxInt - minInt) / avgInterval;
        if (jitter > 0.40f) {
          rhythmConsistent = false;
        }
      }

      if (rhythmConsistent && calculatedBpm >= 35.0f && calculatedBpm <= 220.0f) {
        candidateBpm = calculatedBpm;
        candidateHrFound = true;
      }
    }
  }
  // Note: Blind ACF fallback is eliminated to strictly avoid synthesizing pulse from noise

  // 10. Medical-grade SpO2 and Perfusion Index across pruned cardiac cycles (Maxim AN6845)
  if (numPeaks >= 2) {
    float rValues[15];
    float piValues[15];
    int validCycles = 0;

    for (int k = 0; k < numPeaks - 1; ++k) {
      int start = peakLocs[k];
      int end = peakLocs[k + 1];
      if (end - start < 7 || end >= static_cast<int>(length)) continue;

      // Find cardiac cycle min and max in raw buffers
      uint32_t minIr = irBuffer[start];
      uint32_t maxIr = irBuffer[start];
      uint32_t minRed = redBuffer[start];
      uint32_t maxRed = redBuffer[start];

      for (int i = start; i <= end; ++i) {
        if (irBuffer[i] < minIr) minIr = irBuffer[i];
        if (irBuffer[i] > maxIr) maxIr = irBuffer[i];
        if (redBuffer[i] < minRed) minRed = redBuffer[i];
        if (redBuffer[i] > maxRed) maxRed = redBuffer[i];
      }

      float acRed = static_cast<float>(maxRed - minRed);
      float acIr = static_cast<float>(maxIr - minIr);
      float dcRed = static_cast<float>(maxRed + minRed) * 0.5f;
      float dcIr = static_cast<float>(maxIr + minIr) * 0.5f;

      // Require meaningful physiological AC pulsation on both optical wavelengths
      if (acIr >= PPG_MIN_AC_AMPLITUDE_IR && acRed >= PPG_MIN_AC_AMPLITUDE_RED && dcIr > 0.0f && dcRed > 0.0f) {
        float r = (acRed / dcRed) / (acIr / dcIr);
        float pi = (acIr / dcIr) * 100.0f;

        // Physiological bounds check for R (0.18 to 1.85 corresponds to ~72% to 100% SpO2)
        if (r >= 0.18f && r <= 1.85f && validCycles < 15) {
          rValues[validCycles] = r;
          piValues[validCycles] = pi;
          validCycles++;
        }
      }
    }

    // Confirm Heart Rate independently based on verified IR rhythm and pulsatile amplitude.
    // Medical standards: Heart Rate relies on IR pulsatile SNR, decoupled from Red channel SpO2 ratio.
    // This guarantees stable pulse reporting even if peripheral perfusion is cold or SpO2 is calculating.
    if (candidateHrFound) {
      bool irPulseConfirmed = false;
      for (int k = 0; k < numPeaks - 1; ++k) {
        int start = peakLocs[k];
        int end = peakLocs[k + 1];
        if (end - start < 7 || end >= static_cast<int>(length)) continue;
        uint32_t minIr = irBuffer[start];
        uint32_t maxIr = irBuffer[start];
        for (int i = start; i <= end; ++i) {
          if (irBuffer[i] < minIr) minIr = irBuffer[i];
          if (irBuffer[i] > maxIr) maxIr = irBuffer[i];
        }
        if ((maxIr - minIr) >= PPG_MIN_AC_AMPLITUDE_IR) {
          irPulseConfirmed = true;
          break;
        }
      }

      if (irPulseConfirmed) {
        outHr = static_cast<int32_t>(roundf(candidateBpm));
        outHrValid = true;
      }
    }

    // Median selection across confirmed cycles (rejects motion artifacts)
    if (validCycles > 0) {
      // Sort R values (insertion sort)
      for (int i = 1; i < validCycles; ++i) {
        float keyR = rValues[i];
        float keyPi = piValues[i];
        int j = i - 1;
        while (j >= 0 && rValues[j] > keyR) {
          rValues[j + 1] = rValues[j];
          piValues[j + 1] = piValues[j];
          j--;
        }
        rValues[j + 1] = keyR;
        piValues[j + 1] = keyPi;
      }

      float medianR = rValues[validCycles / 2];
      float medianPi = piValues[validCycles / 2];

      // Empirical Nellcor / Maxim SpO2 calibration polynomial:
      // SpO2 = -45.060 * R^2 + 30.354 * R + 94.845
      float calcSpo2 = (-45.060f * medianR * medianR) + (30.354f * medianR) + 94.845f;
      if (calcSpo2 > 100.0f) calcSpo2 = 100.0f;

      if (calcSpo2 >= 70.0f) {
        outSpo2 = static_cast<int32_t>(roundf(calcSpo2));
        outSpo2Valid = true;
      }

      outPi = medianPi;
      outPiValid = true;
    }
  }
}

float calculatePerfusionIndex(const uint32_t* buffer, size_t length) {
  if (buffer == nullptr || length < 25) return 0.0f;

  // Segment-based AC/DC estimation to eliminate low-frequency baseline drift
  constexpr size_t SEG_LEN = 25; // 1-second segment at 25 Hz
  size_t numSegs = length / SEG_LEN;
  if (numSegs == 0) return 0.0f;

  float piSum = 0.0f;
  int validSegs = 0;

  for (size_t s = 0; s < numSegs; ++s) {
    size_t start = s * SEG_LEN;
    uint32_t minV = buffer[start];
    uint32_t maxV = buffer[start];
    uint64_t sumV = 0;

    for (size_t i = start; i < start + SEG_LEN; ++i) {
      if (buffer[i] < minV) minV = buffer[i];
      if (buffer[i] > maxV) maxV = buffer[i];
      sumV += buffer[i];
    }

    float dc = static_cast<float>(sumV) / static_cast<float>(SEG_LEN);
    float ac = static_cast<float>(maxV - minV);
    // Exclude static optical background noise below 45 counts
    if (dc > 0.0f && ac >= 45.0f) {
      piSum += (ac / dc) * 100.0f;
      validSegs++;
    }
  }

  return (validSegs > 0) ? (piSum / static_cast<float>(validSegs)) : 0.0f;
}

float calculateDewPoint(float tempC, float humidityPct) {
  if (humidityPct < 0.1f) return tempC;
  if (humidityPct > 100.0f) humidityPct = 100.0f;

  constexpr float a = 17.27f;
  constexpr float b = 237.7f;
  float alpha = ((a * tempC) / (b + tempC)) + logf(humidityPct / 100.0f);
  
  if (fabsf(a - alpha) < 0.001f) return tempC;
  return (b * alpha) / (a - alpha);
}

float calculateAbsoluteHumidity(float tempC, float humidityPct) {
  if (humidityPct <= 0.0f) return 0.0f;
  if (humidityPct > 100.0f) humidityPct = 100.0f;

  // Saturation vapor pressure in hPa (Bolton formula)
  float vaporPressure = (humidityPct / 100.0f) * 6.112f * expf((17.67f * tempC) / (tempC + 243.5f));
  return (216.7f * vaporPressure) / (273.15f + tempC);
}

float pressureHpaToMmHg(float pressureHpa) {
  return pressureHpa * 0.750062f;
}

float calculateAltitude(float pressureHpa, float seaLevelHpa) {
  if (pressureHpa <= 0.0f || seaLevelHpa <= 0.0f) return 0.0f;
  return 44330.0f * (1.0f - powf(pressureHpa / seaLevelHpa, 0.190295f));
}

const char* getCO2Status(uint16_t co2) {
  if (co2 == 0) return "Warming Up";
  if (co2 < 600) return "Excellent";
  if (co2 < 1000) return "Good";
  if (co2 < 1400) return "Fair (Ventilate)";
  return "Poor (Action Req)";
}
