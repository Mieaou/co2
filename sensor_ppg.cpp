#include "sensor_ppg.h"

PpgSensorManager::PpgSensorManager()
  : online_(false),
    stateChanged_(false),
    touchStartMs_(0),
    sampleCount_(0),
    currentLedBrightness_(PPG_LED_BRIGHTNESS_DEFAULT),
    lastStableHr_(0.0f),
    lastAgcAdjustMs_(0),
    agcStableCount_(0),
    outOfRangeStartMs_(0),
    fingerReleaseCounter_(0),
    lastValidHrMs_(0),
    lastValidSpo2Ms_(0),
    hrEma_(HR_EMA_ALPHA),
    spo2Ema_(SPO2_EMA_ALPHA) {
  data_.state = PPG_STATE_NO_FINGER;
  data_.led_brightness = currentLedBrightness_;
  data_.buffer_progress_pct = 0;
}

bool PpgSensorManager::begin(TwoWire &wirePort, bool maxPresent) {
  if (!maxPresent) {
    online_ = false;
    Serial.println(F("[-] MAX30101 not detected (offline)"));
    return false;
  }

  if (!sensor_.begin(wirePort, I2C_SPEED_FAST)) {
    online_ = false;
    Serial.println(F("[-] MAX30101 communication failed!"));
    return false;
  }

  sensor_.setup(
    currentLedBrightness_,
    PPG_SAMPLE_AVERAGE,
    PPG_LED_MODE,
    PPG_SAMPLE_RATE_HZ,
    PPG_PULSE_WIDTH,
    PPG_ADC_RANGE
  );

  online_ = true;
  Serial.println(F("[+] MAX30101 initialized (Red+IR, 25 effective SPS, Hysteretic AGC enabled)"));
  return true;
}

bool PpgSensorManager::handleAgcCalibration(uint32_t currentIr, uint32_t currentRed, unsigned long nowMs) {
  // Rate-limit AGC adjustments to once per 120ms to prevent limit-cycle hunting
  if (nowMs - lastAgcAdjustMs_ < 120) {
    return false;
  }
  lastAgcAdjustMs_ = nowMs;

  // Primary AGC control based strictly on the IR channel target linear window [PPG_AGC_TARGET_MIN, PPG_AGC_TARGET_MAX]
  if (currentIr < PPG_AGC_TARGET_MIN && currentLedBrightness_ < PPG_LED_BRIGHTNESS_MAX) {
    uint32_t deficit = PPG_AGC_TARGET_MIN - currentIr;
    uint8_t step = (deficit > PPG_AGC_COARSE_THRESHOLD) ? PPG_AGC_FAST_STEP : PPG_AGC_FINE_STEP;
    uint16_t next = currentLedBrightness_ + step;
    currentLedBrightness_ = (next > PPG_LED_BRIGHTNESS_MAX) ? PPG_LED_BRIGHTNESS_MAX : static_cast<uint8_t>(next);
    sensor_.setPulseAmplitudeRed(currentLedBrightness_);
    sensor_.setPulseAmplitudeIR(currentLedBrightness_);
    data_.led_brightness = currentLedBrightness_;
    agcStableCount_ = 0;
  } else if (currentIr > PPG_AGC_TARGET_MAX && currentLedBrightness_ > PPG_LED_BRIGHTNESS_MIN) {
    uint32_t excess = currentIr - PPG_AGC_TARGET_MAX;
    uint8_t step = (excess > PPG_AGC_COARSE_THRESHOLD) ? PPG_AGC_FAST_STEP : PPG_AGC_FINE_STEP;
    int16_t next = currentLedBrightness_ - step;
    currentLedBrightness_ = (next < PPG_LED_BRIGHTNESS_MIN) ? PPG_LED_BRIGHTNESS_MIN : static_cast<uint8_t>(next);
    sensor_.setPulseAmplitudeRed(currentLedBrightness_);
    sensor_.setPulseAmplitudeIR(currentLedBrightness_);
    data_.led_brightness = currentLedBrightness_;
    agcStableCount_ = 0;
  } else {
    // IR is safely within linear window [45000, 165000].
    // Check if Red has minimum required amplitude for AN6845 SpO2 (>= 8000).
    // If Red is slightly low but IR has plenty of headroom (< 140000), fine-tune upwards once.
    if (currentRed < PPG_MIN_RED_SNR_THRESHOLD && currentIr < 140000 && currentLedBrightness_ < PPG_LED_BRIGHTNESS_MAX) {
      uint16_t next = currentLedBrightness_ + PPG_AGC_FINE_STEP;
      currentLedBrightness_ = (next > PPG_LED_BRIGHTNESS_MAX) ? PPG_LED_BRIGHTNESS_MAX : static_cast<uint8_t>(next);
      sensor_.setPulseAmplitudeRed(currentLedBrightness_);
      sensor_.setPulseAmplitudeIR(currentLedBrightness_);
      data_.led_brightness = currentLedBrightness_;
      agcStableCount_ = 0;
      return false;
    }

    // DC baseline is stabilized in the target linear window: lock gain after 2 confirmations
    agcStableCount_++;
    if (agcStableCount_ >= 2) {
      return true;
    }
  }
  return false;
}

void PpgSensorManager::handleAgcTracking(uint32_t currentIr, unsigned long nowMs) {
  // Broad tracking deadband [18000, 235000] to prevent unnecessary gain disturbance
  constexpr uint32_t TRACKING_MIN = 18000;
  constexpr uint32_t TRACKING_MAX = 235000;

  if (currentIr < TRACKING_MIN || currentIr > TRACKING_MAX) {
    if (outOfRangeStartMs_ == 0) {
      outOfRangeStartMs_ = nowMs;
    } else if (nowMs - outOfRangeStartMs_ >= 2500) {
      // Sustained severe drift outside linear ADC bounds -> recalibrate
      data_.state = PPG_STATE_CALIBRATING;
      data_.buffer_progress_pct = 0;
      sampleCount_ = 0;
      agcStableCount_ = 0;
      outOfRangeStartMs_ = 0;
      touchStartMs_ = nowMs; // Enforce settling window upon recalibration
    }
  } else {
    outOfRangeStartMs_ = 0;
  }
}

void PpgSensorManager::resetBiometrics() {
  bool wasDetected = data_.finger_detected;
  data_.finger_detected = false;
  data_.state = PPG_STATE_NO_FINGER;
  data_.hr_valid = false;
  data_.spo2_valid = false;
  data_.hr_bpm = 0;
  data_.hr_bpm_raw = 0;
  data_.spo2 = 0;
  data_.spo2_raw = 0;
  data_.perfusion_index = 0.0f;
  data_.signal_quality_ok = false;
  data_.buffer_progress_pct = 0;

  sampleCount_ = 0;
  lastStableHr_ = 0.0f;
  lastAgcAdjustMs_ = 0;
  agcStableCount_ = 0;
  outOfRangeStartMs_ = 0;
  fingerReleaseCounter_ = 0;
  lastValidHrMs_ = 0; // Immediate wipe: NO HOLDOVER ON FINGER RELEASE!
  lastValidSpo2Ms_ = 0;
  touchStartMs_ = 0;

  hrMedian_.reset();
  spo2Median_.reset();
  hrEma_.reset();
  spo2Ema_.reset();

  if (currentLedBrightness_ != PPG_LED_BRIGHTNESS_DEFAULT) {
    currentLedBrightness_ = PPG_LED_BRIGHTNESS_DEFAULT;
    sensor_.setPulseAmplitudeRed(currentLedBrightness_);
    sensor_.setPulseAmplitudeIR(currentLedBrightness_);
    data_.led_brightness = currentLedBrightness_;
  }

  if (wasDetected) {
    stateChanged_ = true; // Trigger immediate event-driven broadcast!
  }
}

void PpgSensorManager::update() {
  if (!online_) return;

  unsigned long now = millis();
  sensor_.check();

  while (sensor_.available()) {
    // Read samples strictly in queue order from FIFO tail
    uint32_t red = sensor_.getFIFORed();
    uint32_t ir = sensor_.getFIFOIR();
    sensor_.nextSample();

    // -------------------------------------------------------------------------
    // 1. Finger Presence Check with Hysteresis and Debounce
    // -------------------------------------------------------------------------
    if (ir >= PPG_FINGER_THRESHOLD) {
      // Signal is strong: clear finger release counter
      fingerReleaseCounter_ = 0;

      if (!data_.finger_detected) {
        // Finger newly placed: initialize calibration phase
        data_.finger_detected = true;
        data_.state = PPG_STATE_CALIBRATING;
        data_.buffer_progress_pct = 0;
        sampleCount_ = 0;
        agcStableCount_ = 0;
        lastAgcAdjustMs_ = 0;
        outOfRangeStartMs_ = 0;
        touchStartMs_ = now;
        stateChanged_ = true; // Trigger immediate event-driven broadcast!
      }
    } else if (ir < PPG_FINGER_RELEASE_THRESHOLD) {
      // Signal dropped below release threshold: debounce
      if (data_.finger_detected) {
        fingerReleaseCounter_++;
        if (fingerReleaseCounter_ >= PPG_FINGER_RELEASE_DEBOUNCE_SAMPLES) {
          // Sustained finger removal confirmed (>320ms): cleanly reset
          resetBiometrics();
        }
      }
    } else {
      // In hysteresis band [PPG_FINGER_RELEASE_THRESHOLD, PPG_FINGER_THRESHOLD):
      // Retain current finger presence state, reset release counter
      fingerReleaseCounter_ = 0;
    }

    if (data_.finger_detected) {
      // -----------------------------------------------------------------------
      // 2. FSM Execution
      // -----------------------------------------------------------------------
      if (data_.state == PPG_STATE_CALIBRATING) {
        bool locked = handleAgcCalibration(ir, red, now);
        // ISO 80601-2-61: Require BOTH AGC lock AND contact settling window (>= 1000ms)
        // This eliminates transient contact-squeeze artifacts (e.g. spurious 95 BPM)
        if (locked && (now - touchStartMs_ >= 1000)) {
          // AGC gain locked and contact settled: switch cleanly to acquiring state with empty buffer
          data_.state = PPG_STATE_ACQUIRING;
          data_.buffer_progress_pct = 0;
          sampleCount_ = 0;
          stateChanged_ = true;
        }
      } else if (data_.state == PPG_STATE_ACQUIRING || data_.state == PPG_STATE_TRACKING) {
        if (data_.state == PPG_STATE_TRACKING) {
          handleAgcTracking(ir, now);
        }

        // Only store samples if still in acquiring/tracking
        if (data_.state != PPG_STATE_CALIBRATING) {
          if (sampleCount_ < BUFFER_SIZE) {
            redBuffer_[sampleCount_] = red;
            irBuffer_[sampleCount_] = ir;
            sampleCount_++;
            if (data_.state == PPG_STATE_ACQUIRING) {
              data_.buffer_progress_pct = static_cast<uint8_t>((sampleCount_ * 100) / BUFFER_SIZE);
            } else {
              data_.buffer_progress_pct = 100;
            }
          }

          // When buffer capacity is reached (100 samples = 4 seconds)
          if (sampleCount_ >= BUFFER_SIZE) {
            bool wasAcquiring = (data_.state == PPG_STATE_ACQUIRING);
            data_.state = PPG_STATE_TRACKING;
            data_.buffer_progress_pct = 100;
            if (wasAcquiring) {
              stateChanged_ = true; // Trigger immediate event-driven broadcast on initial lock!
            }

            // -----------------------------------------------------------------
            // 3. State-of-the-Art Biometric Extraction
            // -----------------------------------------------------------------
            int32_t rawSpo2 = 0;
            bool validSpo2 = false;
            int32_t rawHr = 0;
            bool validHr = false;
            float rawPi = 0.0f;
            bool validPi = false;

            processPpgSignal(
              irBuffer_, redBuffer_, BUFFER_SIZE,
              PPG_EFFECTIVE_FS,
              rawHr, validHr,
              rawSpo2, validSpo2,
              rawPi, validPi,
              lastStableHr_
            );

            data_.hr_bpm_raw = rawHr;
            data_.spo2_raw = rawSpo2;
            data_.perfusion_index = validPi ? rawPi : calculatePerfusionIndex(irBuffer_, BUFFER_SIZE);
            data_.signal_quality_ok = (data_.perfusion_index >= MIN_PERFUSION_INDEX);

            // -----------------------------------------------------------------
            // 4. Heart Rate Post-Processing (Anti-Harmonic + Median + EMA + Holdover)
            // -----------------------------------------------------------------
            if (validHr && rawHr >= HR_MIN_BPM && rawHr <= HR_MAX_BPM && data_.signal_quality_ok) {
              float correctedHr = filterHarmonics(static_cast<float>(rawHr), lastStableHr_, HARMONIC_TOLERANCE);

              hrMedian_.add(correctedHr);
              float medianHr = hrMedian_.getMedian();

              float smoothHr = hrEma_.update(medianHr);
              data_.hr_bpm = static_cast<int32_t>(roundf(smoothHr));
              data_.hr_valid = true;
              lastStableHr_ = smoothHr;
              lastValidHrMs_ = now;
            } else {
              // Clinical holdover: retain last confirmed valid rate for up to 3 seconds during momentary noise
              if (lastValidHrMs_ > 0 && (now - lastValidHrMs_ <= BIOMETRIC_HOLDOVER_MS)) {
                data_.hr_valid = true;
              } else {
                data_.hr_valid = false;
              }
            }

            // -----------------------------------------------------------------
            // 5. SpO2 Post-Processing (Median + EMA + Holdover)
            // -----------------------------------------------------------------
            if (validSpo2 && rawSpo2 >= SPO2_MIN_PCT && rawSpo2 <= SPO2_MAX_PCT && data_.signal_quality_ok) {
              spo2Median_.add(static_cast<float>(rawSpo2));
              float medianSpo2 = spo2Median_.getMedian();
              float smoothSpo2 = spo2Ema_.update(medianSpo2);

              data_.spo2 = static_cast<int32_t>(roundf(smoothSpo2));
              data_.spo2_valid = true;
              lastValidSpo2Ms_ = now;
            } else {
              // Clinical holdover: retain last confirmed valid SpO2 for up to 3 seconds during momentary noise
              if (lastValidSpo2Ms_ > 0 && (now - lastValidSpo2Ms_ <= BIOMETRIC_HOLDOVER_MS)) {
                data_.spo2_valid = true;
              } else {
                data_.spo2_valid = false;
              }
            }

            // -----------------------------------------------------------------
            // 6. Sliding Window Step (25 samples shift = 1 second)
            // -----------------------------------------------------------------
            for (size_t i = PPG_SHIFT_SAMPLES; i < BUFFER_SIZE; ++i) {
              redBuffer_[i - PPG_SHIFT_SAMPLES] = redBuffer_[i];
              irBuffer_[i - PPG_SHIFT_SAMPLES] = irBuffer_[i];
            }
            sampleCount_ = BUFFER_SIZE - PPG_SHIFT_SAMPLES; // 75
          }
        }
      }
    }
  }
}
