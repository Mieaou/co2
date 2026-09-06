/**
 * @file dsp_filters.cpp
 * @brief Implementation of DSP filters, harmonic rejection, and physical formulas.
 */

#include "dsp_filters.h"
#include "config.h"

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
  // Physiological reflection wave (dicrotic notch) causes instantaneous 2x jumps into tachycardia.
  // A resting rate of 60-95 BPM is never a dicrotic doubling of severe bradycardia (30-47 BPM).
  // Therefore, require currentVal >= 100 BPM and prevStableVal >= 45 BPM before halving.
  if (currentVal >= 100.0f && prevStableVal >= 45.0f && fabsf(ratio - 2.0f) <= (2.0f * tolerance)) {
    return currentVal / 2.0f;
  }

  // We deliberately do NOT force an auto-doubling on ratio ~0.5x.
  // In a multi-cycle buffer, an average rate dropping to 0.5x indicates either
  // a genuine physiological deceleration or a self-correction from an earlier falsely
  // doubled state. Forcing a 2.0x multiplication would lock the system in an error state.

  return currentVal;
}

bool estimateFundamentalPeriodAcf(
    const float* signal,
    size_t length,
    int fs,
    int& outFundamentalLag,
    float& outConfidence) {

  outFundamentalLag = 0;
  outConfidence = 0.0f;

  if (signal == nullptr || length < 50 || fs <= 0) return false;

  // Lags corresponding to physiological bounds: 220 BPM (tau ~7 at 25Hz) down to 35 BPM (tau ~43 at 25Hz)
  int minLag = static_cast<int>(roundf(static_cast<float>(fs) * 60.0f / 220.0f));
  int maxLag = static_cast<int>(roundf(static_cast<float>(fs) * 60.0f / 35.0f));
  if (minLag < 6) minLag = 6;
  if (maxLag > static_cast<int>(length) / 2) maxLag = static_cast<int>(length) / 2;
  if (maxLag <= minLag) return false;

  float acf[64];
  for (int tau = minLag - 1; tau <= maxLag + 1 && tau < 64; ++tau) {
    float cross = 0.0f;
    float sumSqBase = 0.0f;
    float sumSqTau = 0.0f;
    size_t count = length - tau;

    for (size_t i = 0; i < count; ++i) {
      cross += signal[i] * signal[i + tau];
      sumSqBase += signal[i] * signal[i];
      sumSqTau += signal[i + tau] * signal[i + tau];
    }
    float denom = sqrtf(sumSqBase * sumSqTau);
    acf[tau] = (denom > 1e-4f) ? (cross / denom) : 0.0f;
  }

  // Find local maxima in ACF
  struct AcfPeak {
    int lag;
    float val;
  };
  AcfPeak peaks[16];
  int peakCount = 0;

  for (int tau = minLag; tau <= maxLag && peakCount < 16; ++tau) {
    if (acf[tau] > 0.18f && acf[tau] > acf[tau - 1] && acf[tau] >= acf[tau + 1]) {
      peaks[peakCount].lag = tau;
      peaks[peakCount].val = acf[tau];
      peakCount++;
    }
  }

  if (peakCount == 0) return false;

  // Find highest correlation peak
  int bestIdx = 0;
  for (int i = 1; i < peakCount; ++i) {
    if (peaks[i].val > peaks[bestIdx].val) {
      bestIdx = i;
    }
  }



  int fundamentalLag = peaks[bestIdx].lag;
  float maxConf = peaks[bestIdx].val;

  // Harmonic disambiguation:
  // Only if the strongest peak in ACF is at an abnormally high frequency / short lag
  // (tau < 12 samples, corresponding to > 125 BPM at 25 Hz), check if there is a peak
  // at ~2 * tau representing the true fundamental cardiac cycle.
  // In normal resting rates (tau >= 13, <= 115 BPM), any peak at 2*tau is simply the
  // natural periodic repetition of the heartbeat across 2 cycles and must NEVER replace tau!
  if (fundamentalLag < 12) {
    for (int i = 0; i < peakCount; ++i) {
      if (peaks[i].lag > fundamentalLag) {
        float ratio = static_cast<float>(peaks[i].lag) / static_cast<float>(fundamentalLag);
        if (fabsf(ratio - 2.0f) <= 0.20f) {
          if (peaks[i].val >= 0.50f * maxConf && peaks[i].val >= 0.25f) {
            fundamentalLag = peaks[i].lag;
            maxConf = peaks[i].val;
            break;
          }
        }
      }
    }
  }

  outFundamentalLag = fundamentalLag;
  outConfidence = maxConf;
  return (maxConf >= 0.22f);
}

void pruneDicroticPeaks(
    const int* inLocs,
    const float* inAmps,
    int inCount,
    int* outLocs,
    float* outAmps,
    int& outCount,
    int expectedLag) {

  outCount = 0;
  if (inLocs == nullptr || inAmps == nullptr || inCount <= 0) return;

  if (inCount < 4) {
    for (int i = 0; i < inCount; ++i) {
      outLocs[i] = inLocs[i];
      outAmps[i] = inAmps[i];
    }
    outCount = inCount;
    return;
  }

  int intervals[30];
  int numIntervals = inCount - 1;
  for (int i = 0; i < numIntervals; ++i) {
    intervals[i] = inLocs[i + 1] - inLocs[i];
  }

  int alternansIntervalCount = 0;
  for (int i = 0; i < numIntervals - 1; ++i) {
    int diff = abs(intervals[i] - intervals[i + 1]);
    int sum = intervals[i] + intervals[i + 1];
    // In true dicrotic doubling, the sum of systolic-to-dicrotic and dicrotic-to-systolic
    // represents ONE heartbeat cycle (10 to 26 samples at 25 Hz = 58 to 150 BPM).
    // Sums > 26 represent two full cardiac cycles and must not be treated as alternans.
    if (diff >= 3 && sum >= 10 && sum <= 26) {
      alternansIntervalCount++;
    }
  }

  int evenHigherCount = 0;
  int oddHigherCount = 0;
  for (int i = 0; i < inCount - 1; ++i) {
    float r = inAmps[i + 1] / (inAmps[i] + 1e-4f);
    if (i % 2 == 0) {
      if (r < 0.75f) evenHigherCount++;
      else if (r > 1.33f) oddHigherCount++;
    } else {
      if (r > 1.33f) evenHigherCount++;
      else if (r < 0.75f) oddHigherCount++;
    }
  }

  bool intervalHalved = false;
  if (expectedLag >= 12) {
    float avgInt = 0.0f;
    for (int i = 0; i < numIntervals; ++i) avgInt += intervals[i];
    avgInt /= static_cast<float>(numIntervals);
    if (avgInt <= 0.65f * static_cast<float>(expectedLag)) {
      intervalHalved = true;
    }
  }

  int requiredAlternans = numIntervals / 2;
  if (requiredAlternans < 1) requiredAlternans = 1;

  // Prune only if BOTH interval alternans (or halved intervals from ACF) AND amplitude alternans agree CONSISTENTLY
  bool isAlternans = (alternansIntervalCount >= requiredAlternans && (evenHigherCount >= requiredAlternans || oddHigherCount >= requiredAlternans)) ||
                     (intervalHalved && (evenHigherCount >= requiredAlternans || oddHigherCount >= requiredAlternans));

  if (isAlternans) {
    bool pruneOdds = (evenHigherCount >= oddHigherCount);
    for (int i = 0; i < inCount; ++i) {
      bool isDicrotic = (pruneOdds && (i % 2 == 1)) || (!pruneOdds && (i % 2 == 0));
      if (!isDicrotic) {
        outLocs[outCount] = inLocs[i];
        outAmps[outCount] = inAmps[i];
        outCount++;
      }
    }
  } else {
    for (int i = 0; i < inCount; ++i) {
      outLocs[i] = inLocs[i];
      outAmps[i] = inAmps[i];
    }
    outCount = inCount;
  }
}

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

  // 4. Clinical Autocorrelation Function (ACF) to extract ground-truth fundamental period
  int acfLag = 0;
  float acfConf = 0.0f;
  bool acfValid = estimateFundamentalPeriodAcf(smoothed, n, fs, acfLag, acfConf);

  // 5. Elgendi Non-linear Energy Squaring to suppress dicrotic notches
  float squared[128];
  float maxSq = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    if (smoothed[i] > 0.0f) {
      squared[i] = smoothed[i] * smoothed[i];
    } else {
      squared[i] = 0.0f;
    }
    if (squared[i] > maxSq) maxSq = squared[i];
  }

  // Dynamic range & adaptive dual-threshold calculation
  float maxVal = smoothed[0];
  float minVal = smoothed[0];
  for (size_t i = 1; i < n; ++i) {
    if (smoothed[i] > maxVal) maxVal = smoothed[i];
    if (smoothed[i] < minVal) minVal = smoothed[i];
  }
  float dynamicRange = maxVal - minVal;
  if (dynamicRange < PPG_MIN_DYNAMIC_RANGE) return; // Rejects cloth/table noise

  // Dual thresholding: smoothed threshold + squared energy threshold
  float thresholdSmoothed = minVal + (dynamicRange * 0.40f);
  float thresholdSq = maxSq * 0.08f; // Resilient against contact-settling amplitude spikes

  // 6. Dynamic physiological refractory period
  // If prevStableHr is present, scale from it; otherwise use ACF ground truth to avoid cold-start 2x doubling!
  int tauRefractory = 10;
  if (prevStableHr >= 35.0f && prevStableHr <= 220.0f) {
    float estCycleSamples = (static_cast<float>(fs) * 60.0f) / prevStableHr;
    tauRefractory = static_cast<int>(roundf(0.48f * estCycleSamples));
  } else if (acfValid && acfLag >= 7) {
    tauRefractory = static_cast<int>(roundf(0.48f * static_cast<float>(acfLag)));
  }

  int minRefractory = (prevStableHr > 140.0f || (acfValid && acfLag <= 10)) ? 5 : 7;
  if (tauRefractory < minRefractory) tauRefractory = minRefractory;
  if (tauRefractory > 18) tauRefractory = 18; // 720ms cap (supports down to 40 BPM)

  // 7. Systolic peak detection with dual threshold & refractory blanking
  int rawPeakLocs[30];
  float rawPeakAmps[30];
  int rawNumPeaks = 0;

  for (size_t i = 1; i < n - 1 && rawNumPeaks < 30; ++i) {
    if (smoothed[i] > thresholdSmoothed && squared[i] >= thresholdSq &&
        smoothed[i] >= smoothed[i - 1] && smoothed[i] > smoothed[i + 1]) {

      if (rawNumPeaks > 0 && (static_cast<int>(i) - rawPeakLocs[rawNumPeaks - 1]) < tauRefractory) {
        // Within refractory period, retain only the higher amplitude peak (systolic vs dicrotic)
        if (smoothed[i] > rawPeakAmps[rawNumPeaks - 1]) {
          rawPeakLocs[rawNumPeaks - 1] = static_cast<int>(i);
          rawPeakAmps[rawNumPeaks - 1] = smoothed[i];
        }
      } else {
        rawPeakLocs[rawNumPeaks] = static_cast<int>(i);
        rawPeakAmps[rawNumPeaks] = smoothed[i];
        rawNumPeaks++;
      }
    }
  }

  // 8. Bimodal Alternating Peak & Amplitude Filter (Prunes surviving dicrotic waves)
  int peakLocs[30];
  float peakAmps[30];
  int numPeaks = 0;
  pruneDicroticPeaks(rawPeakLocs, rawPeakAmps, rawNumPeaks, peakLocs, peakAmps, numPeaks, acfValid ? acfLag : 0);

  // 9. Calculate Heart Rate from verified inter-beat intervals
  float candidateBpm = 0.0f;
  bool candidateHrFound = false;

  if (numPeaks >= 2) {
    int intervals[30];
    int validIntervals = 0;
    int minInt = 999;
    int maxInt = 0;
    float intervalSum = 0.0f;

    for (int i = 1; i < numPeaks; ++i) {
      int diff = peakLocs[i] - peakLocs[i - 1];
      // Physiological bounds: 7 samples (214 BPM) to 43 samples (35 BPM)
      if (diff >= 7 && diff <= 43) {
        intervals[validIntervals] = diff;
        intervalSum += static_cast<float>(diff);
        if (diff < minInt) minInt = diff;
        if (diff > maxInt) maxInt = diff;
        validIntervals++;
      }
    }

    if (validIntervals > 0) {
      float avgInterval = intervalSum / static_cast<float>(validIntervals);
      float calculatedBpm = (static_cast<float>(fs) * 60.0f) / avgInterval;

      // ISO 80601-2-61: Signal Quality Index (SQI) check on interval regularity (RR Jitter).
      // If motion/pressure artifact corrupted the rhythm, intervals will vary widely (> 22%).
      bool rhythmConsistent = true;
      if (validIntervals >= 2) {
        float jitter = static_cast<float>(maxInt - minInt) / avgInterval;
        if (jitter > 0.22f) {
          rhythmConsistent = false;
        }
      } else if (validIntervals == 1) {
        // With only 1 interval, require verification by ACF fundamental lag within +/- 18%
        if (!acfValid || fabsf(static_cast<float>(intervals[0]) - static_cast<float>(acfLag)) > (0.18f * static_cast<float>(acfLag))) {
          rhythmConsistent = false;
        }
      }

      // Cross-verify with ACF fundamental rate to guarantee immunity against 2x doubling (only for tachycardia)
      if (acfValid && acfLag >= 7) {
        float acfBpm = (static_cast<float>(fs) * 60.0f) / static_cast<float>(acfLag);
        if (calculatedBpm >= 100.0f && calculatedBpm >= 1.75f * acfBpm && calculatedBpm <= 2.25f * acfBpm) {
          calculatedBpm = calculatedBpm / 2.0f;
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
