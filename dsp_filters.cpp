/**
 * @file dsp_filters.cpp
 * @brief Implementation of DSP filters, harmonic rejection, and physical formulas.
 */

#include "dsp_filters.h"

float filterHarmonics(float currentVal, float prevStableVal, float tolerance) {
  if (currentVal <= 10.0f) {
    return currentVal;
  }

  // If no prior stable baseline has been confirmed yet, do not arbitrarily halve
  // the value, as true sinus tachycardia or active HR (130-180 BPM) would be corrupted.
  if (prevStableVal <= 10.0f) {
    return currentVal;
  }

  float ratio = currentVal / prevStableVal;

  // Check for doubling harmonic (e.g. 2.0x +/- tolerance*2.0x -> 1.70x to 2.30x for tol=0.15)
  // Physiological reflection wave (dicrotic notch) causes instantaneous 2x jumps.
  if (fabsf(ratio - 2.0f) <= (2.0f * tolerance)) {
    return currentVal / 2.0f;
  }

  // We deliberately do NOT force an auto-doubling on ratio ~0.5x.
  // In a multi-cycle buffer, an average rate dropping to 0.5x indicates either
  // a genuine physiological deceleration or a self-correction from an earlier falsely
  // doubled state. Forcing a 2.0x multiplication would lock the system in an error state.

  return currentVal;
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

  // 2. Remove DC and invert IR (optical absorption peaks become positive local maxima)
  float acIr[128];
  size_t n = (length > 128) ? 128 : length;
  for (size_t i = 0; i < n; ++i) {
    acIr[i] = -1.0f * (static_cast<float>(irBuffer[i]) - irMean);
  }

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

  // 4. Dynamic range & adaptive threshold calculation
  float maxVal = smoothed[0];
  float minVal = smoothed[0];
  for (size_t i = 1; i < n; ++i) {
    if (smoothed[i] > maxVal) maxVal = smoothed[i];
    if (smoothed[i] < minVal) minVal = smoothed[i];
  }
  float dynamicRange = maxVal - minVal;
  if (dynamicRange < 25.0f) return; // Signal too flat for pulsatile detection

  // Adaptive threshold: set to 42% of peak-to-peak amplitude above floor
  // This naturally suppresses low-amplitude dicrotic notches
  float threshold = minVal + (dynamicRange * 0.42f);

  // 5. Dynamic physiological refractory period
  // Based on current stable heart rate or standard resting default
  int tauRefractory = 10; // Default: 400ms at 25Hz (allows up to 150 BPM at first touch, guarantees blocking dicrotic notch <400ms)
  if (prevStableHr >= 35.0f && prevStableHr <= 220.0f) {
    float estCycleSamples = (static_cast<float>(fs) * 60.0f) / prevStableHr;
    tauRefractory = static_cast<int>(roundf(0.40f * estCycleSamples));
    if (tauRefractory < 7) tauRefractory = 7;   // Absolute minimum: 280ms (214 BPM)
    if (tauRefractory > 14) tauRefractory = 14; // Maximum cap: 560ms
  }

  // 6. Systolic peak detection with refractory filtering
  int peakLocs[30];
  float peakAmps[30];
  int numPeaks = 0;

  for (size_t i = 1; i < n - 1 && numPeaks < 30; ++i) {
    if (smoothed[i] > threshold && smoothed[i] >= smoothed[i - 1] && smoothed[i] > smoothed[i + 1]) {
      if (numPeaks > 0 && (static_cast<int>(i) - peakLocs[numPeaks - 1]) < tauRefractory) {
        // If within refractory period, retain only the higher amplitude peak (systolic vs dicrotic)
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

  // 7. Calculate Heart Rate from verified inter-beat intervals
  if (numPeaks >= 2) {
    float intervalSum = 0.0f;
    int validIntervals = 0;
    for (int i = 1; i < numPeaks; ++i) {
      int diff = peakLocs[i] - peakLocs[i - 1];
      // Physiological bounds: 7 samples (214 BPM) to 43 samples (35 BPM)
      if (diff >= 7 && diff <= 43) {
        intervalSum += static_cast<float>(diff);
        validIntervals++;
      }
    }
    if (validIntervals > 0) {
      float avgInterval = intervalSum / static_cast<float>(validIntervals);
      float calculatedBpm = (static_cast<float>(fs) * 60.0f) / avgInterval;
      if (calculatedBpm >= 35.0f && calculatedBpm <= 220.0f) {
        outHr = static_cast<int32_t>(roundf(calculatedBpm));
        outHrValid = true;
      }
    }
  }

  // 8. Medical-grade SpO2 and Perfusion Index across cardiac cycles (Maxim AN6845)
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

      if (acIr >= 12.0f && dcIr > 0.0f && dcRed > 0.0f) {
        float r = (acRed / dcRed) / (acIr / dcIr);
        float pi = (acIr / dcIr) * 100.0f;

        // Physiological bounds check for R (0.15 to 2.0 corresponds to ~70% to 100% SpO2)
        if (r >= 0.15f && r <= 2.0f && validCycles < 15) {
          rValues[validCycles] = r;
          piValues[validCycles] = pi;
          validCycles++;
        }
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
    if (dc > 0.0f && ac >= 10.0f) {
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
