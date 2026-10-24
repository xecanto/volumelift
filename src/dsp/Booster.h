// Gain stage followed by a peak limiter, operating on interleaved float samples.
// Header-only and free of allocation or locking so it is safe on the audio thread.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vl {

inline float DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

class Booster {
public:
    // Output never exceeds this absolute value when the limiter is on (-0.3 dBFS).
    static constexpr float kCeiling = 0.9661f;

    void Prepare(float sampleRate, uint32_t channels) {
        sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
        channels_ = channels;
        gainGlide_ = Coefficient(0.020f);  // 20 ms glide when the gain setting changes
        release_ = Coefficient(0.150f);    // 150 ms limiter recovery
        Reset();
    }

    void Reset() {
        currentGain_ = targetGain_;
        limiterGain_ = 1.0f;
    }

    void SetTarget(float gainDb, bool limiter) {
        targetGain_ = DbToLinear(gainDb);
        limiter_ = limiter;
    }

    // Applies gain (and limiting) in place to `frames` frames of `channels` samples each.
    void Process(float* samples, uint32_t frames) {
        for (uint32_t f = 0; f < frames; ++f) {
            currentGain_ = targetGain_ + (currentGain_ - targetGain_) * gainGlide_;
            float* frame = samples + static_cast<size_t>(f) * channels_;

            float peak = 0.0f;
            for (uint32_t c = 0; c < channels_; ++c) {
                frame[c] *= currentGain_;
                peak = std::max(peak, std::fabs(frame[c]));
            }

            if (!limiter_) {
                for (uint32_t c = 0; c < channels_; ++c) {
                    frame[c] = std::clamp(frame[c], -1.0f, 1.0f);
                }
                continue;
            }

            // Instant attack keeps peaks under the ceiling; release glides back to unity.
            const float needed = peak > kCeiling ? kCeiling / peak : 1.0f;
            limiterGain_ = needed < limiterGain_ ? needed : needed + (limiterGain_ - needed) * release_;
            for (uint32_t c = 0; c < channels_; ++c) {
                frame[c] = std::clamp(frame[c] * limiterGain_, -kCeiling, kCeiling);
            }
        }
    }

    float CurrentGain() const { return currentGain_; }
    float LimiterGain() const { return limiterGain_; }

private:
    float Coefficient(float seconds) const { return std::exp(-1.0f / (seconds * sampleRate_)); }

    float sampleRate_ = 48000.0f;
    uint32_t channels_ = 2;
    float gainGlide_ = 0.0f;
    float release_ = 0.0f;
    float targetGain_ = 1.0f;
    float currentGain_ = 1.0f;
    float limiterGain_ = 1.0f;
    bool limiter_ = true;
};

}  // namespace vl
