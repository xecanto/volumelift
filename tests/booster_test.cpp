// Offline checks for the boost/limiter DSP. Exits non-zero on the first failure.
#include "dsp/Booster.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr float kRate = 48000.0f;
constexpr float kPi = 3.14159265358979f;
int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

std::vector<float> Sine(float amplitude, uint32_t frames, uint32_t channels) {
    std::vector<float> samples(static_cast<size_t>(frames) * channels);
    for (uint32_t f = 0; f < frames; ++f) {
        const float value = amplitude * std::sin(2.0f * kPi * 1000.0f * f / kRate);
        for (uint32_t c = 0; c < channels; ++c) {
            samples[static_cast<size_t>(f) * channels + c] = value;
        }
    }
    return samples;
}

float Peak(const std::vector<float>& samples, size_t from = 0) {
    float peak = 0.0f;
    for (size_t i = from; i < samples.size(); ++i) {
        peak = std::max(peak, std::fabs(samples[i]));
    }
    return peak;
}

bool AllFinite(const std::vector<float>& samples) {
    for (float s : samples) {
        if (!std::isfinite(s)) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    const uint32_t frames = 48000;

    {
        vl::Booster booster;
        booster.Prepare(kRate, 2);
        booster.SetTarget(12.0f, true);
        booster.Reset();
        auto quiet = Sine(0.05f, frames, 2);
        booster.Process(quiet.data(), frames);
        const float expected = 0.05f * vl::DbToLinear(12.0f);
        Check(std::fabs(Peak(quiet, quiet.size() / 2) - expected) < 0.002f, "quiet signal is boosted by +12 dB");
        Check(booster.LimiterGain() > 0.999f, "limiter is idle for a quiet signal");
    }

    {
        vl::Booster booster;
        booster.Prepare(kRate, 2);
        booster.SetTarget(24.0f, true);
        booster.Reset();
        auto loud = Sine(0.9f, frames, 2);
        booster.Process(loud.data(), frames);
        Check(Peak(loud) <= vl::Booster::kCeiling + 1e-6f, "limiter keeps peaks under the ceiling");
        Check(AllFinite(loud), "limited output is finite");
    }

    {
        vl::Booster booster;
        booster.Prepare(kRate, 6);
        booster.SetTarget(24.0f, false);
        booster.Reset();
        auto loud = Sine(0.9f, frames, 6);
        booster.Process(loud.data(), frames);
        Check(Peak(loud) <= 1.0f, "limiter off still clips to full scale (5.1 layout)");
    }

    {
        vl::Booster booster;
        booster.Prepare(kRate, 2);
        booster.SetTarget(18.0f, true);
        std::vector<float> silence(static_cast<size_t>(frames) * 2, 0.0f);
        booster.Process(silence.data(), frames);
        Check(Peak(silence) == 0.0f, "silence stays silent");
    }

    {
        vl::Booster booster;
        booster.Prepare(kRate, 2);
        booster.SetTarget(0.0f, true);
        booster.Reset();
        booster.SetTarget(20.0f, true);
        auto ramp = Sine(0.01f, 480, 2);  // first 10 ms after the change
        booster.Process(ramp.data(), 480);
        Check(booster.CurrentGain() > 1.0f && booster.CurrentGain() < vl::DbToLinear(20.0f),
              "gain glides instead of jumping");
    }

    std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
