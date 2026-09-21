#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <complex>

namespace sineLab
{
constexpr int partials = 192;
constexpr int steps = 8;
using Mask = std::array<float, partials>;

inline float smoothstep (float x)
{
    x = juce::jlimit (0.0f, 1.0f, x);
    return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
}

// Fade before the OUTPUT Nyquist limit, including stereo detuning.
inline float bandGain (float frequency, double outputRate)
{
    return 1.0f - smoothstep ((frequency / (float) outputRate - 0.40f) / 0.05f);
}

// A reflected playhead has no wrap discontinuity at the model boundaries.
inline double reflectedFrame (double position, double lastFrame)
{
    if (lastFrame <= 0.0) return 0.0;
    double phase = std::fmod (position, lastFrame * 2.0);
    if (phase < 0.0) phase += lastFrame * 2.0;
    return phase <= lastFrame ? phase : lastFrame * 2.0 - phase;
}

class Sequence
{
public:
    Sequence() { reset(); }
    void reset()
    {
        for (auto& step : values)
            for (auto& value : step) value.store (1.0f);
    }
    float get (int step, int partial) const { return values[(size_t) step][(size_t) partial].load(); }
    void set (int step, int partial, float gain)
    {
        values[(size_t) step][(size_t) partial].store (
            std::isfinite (gain) ? juce::jlimit (0.0f, 1.0f, gain) : 1.0f);
    }
    void evaluate (double phase, float glide, Mask& result) const
    {
        const double wrapped = phase - std::floor (phase / steps) * steps;
        const int step = (int) wrapped;
        const float fraction = (float) (wrapped - step);
        // Glide is the fraction of a step used to arrive from the previous mask.
        const float mix = glide <= 0.001f ? 1.0f : smoothstep (fraction / glide);
        const int previous = (step + steps - 1) % steps;
        for (int k = 0; k < partials; ++k)
            result[(size_t) k] = get (previous, k) * (1.0f - mix) + get (step, k) * mix;
    }
private:
    std::array<std::array<std::atomic<float>, partials>, steps> values;
};

// Four frozen sine spectra survive released notes. Replacing/clearing a layer
// fades it to zero before touching its phases; a pending snapshot is inaudible.
class Memory
{
public:
    struct Frame
    {
        int count = 0;
        std::array<std::complex<float>, partials> zL {}, zR {}, wL {}, wR {};
        Mask amp {}, panL {}, panR {};
    };
    void prepare (double rate)
    {
        sr = rate;
        layers = {};
        next = 0;
        wet.reset (sr, 0.02);
        wet.setCurrentAndTargetValue (0.0f);
        for (auto& gain : gains)
        {
            gain.reset (sr, 0.01);
            gain.setCurrentAndTargetValue (1.0f);
        }
    }
    void remember (const Frame& frame)
    {
        if (frame.count <= 0) return;
        auto& layer = layers[(size_t) next];
        next = (next + 1) % (int) layers.size();
        if (layer.current.count == 0)
        {
            layer = {};
            layer.current = frame;
            layer.life = 1.0f;
            layer.fade = 0.0f;
        }
        else
        {
            layer.pending = frame;
            layer.fading = true;
        }
    }
    void clear()
    {
        for (auto& layer : layers)
        {
            layer.pending.count = 0;
            layer.fading = true;
        }
    }
    void render (juce::AudioBuffer<float>& buffer, int start, int count,
                 float amount, float seconds, const Mask& mask)
    {
        wet.setTargetValue (juce::jlimit (0.0f, 1.0f, amount));
        for (int k = 0; k < partials; ++k) gains[(size_t) k].setTargetValue (mask[(size_t) k]);
        bool active = false;
        for (const auto& layer : layers) active = active || layer.current.count > 0;
        if (! active)
        {
            wet.skip (count);
            for (auto& gain : gains) gain.skip (count);
            return;
        }
        const float decay = std::exp (-6.907755f / ((float) sr * juce::jmax (0.1f, seconds)));
        const float fadeStep = 1.0f / ((float) sr * 0.02f);
        auto* left = buffer.getWritePointer (0, start);
        auto* right = buffer.getWritePointer (1, start);
        for (int i = 0; i < count; ++i)
        {
            const float mix = wet.getNextValue() * 0.5f;
            Mask maskNow;
            for (int k = 0; k < partials; ++k) maskNow[(size_t) k] = gains[(size_t) k].getNextValue();
            for (auto& layer : layers)
            {
                auto& frame = layer.current;
                if (frame.count == 0) continue;
                layer.fade = juce::jlimit (0.0f, 1.0f,
                    layer.fade + (layer.fading ? -fadeStep : fadeStep));
                layer.life *= decay;
                float l = 0.0f, r = 0.0f;
                for (int k = 0; k < frame.count; ++k)
                {
                    auto n = (size_t) k;
                    frame.zL[n] *= frame.wL[n];
                    frame.zR[n] *= frame.wR[n];
                    const float a = frame.amp[n] * maskNow[n];
                    l += a * frame.panL[n] * frame.zL[n].imag();
                    r += a * frame.panR[n] * frame.zR[n].imag();
                }
                const float level = layer.life * smoothstep (layer.fade) * mix;
                left[i] += l * level;
                right[i] += r * level;
                if ((layer.fading && layer.fade <= 0.0f) || layer.life < 1e-5f)
                {
                    frame = layer.pending;
                    layer.pending.count = 0;
                    layer.life = 1.0f;
                    layer.fade = 0.0f;
                    layer.fading = false;
                }
            }
        }
        // Bound floating point oscillator drift during long ambient tails.
        for (auto& layer : layers)
            for (int k = 0; k < layer.current.count; ++k)
            {
                auto& l = layer.current.zL[(size_t) k];
                auto& r = layer.current.zR[(size_t) k];
                l *= 1.5f - 0.5f * std::norm (l);
                r *= 1.5f - 0.5f * std::norm (r);
            }
    }
private:
    struct Layer
    {
        Frame current, pending;
        float life = 1.0f, fade = 0.0f;
        bool fading = false;
    };
    std::array<Layer, 4> layers;
    std::array<juce::SmoothedValue<float>, partials> gains;
    juce::SmoothedValue<float> wet;
    double sr = 176400.0;
    int next = 0;
};

// Linear-phase 4:1 FIR decimation. Synthesise at 4x, then remove modulation
// sidebands above the output band. No distortion, waveshaping or noise source.
class SineOutput
{
public:
    static constexpr int factor = 4, taps = 513, latency = (taps - 1) / (2 * factor);
    SineOutput()
    {
        constexpr double cutoff = 0.1125; // 0.45 of the output sample rate
        double sum = 0.0;
        for (int i = 0; i < taps; ++i)
        {
            const double x = i - (taps - 1) * 0.5;
            const double angle = juce::MathConstants<double>::twoPi * i / (taps - 1);
            const double window = 0.35875 - 0.48829 * std::cos (angle)
                + 0.14128 * std::cos (2.0 * angle) - 0.01168 * std::cos (3.0 * angle);
            const double sinc = x == 0.0 ? 2.0 * cutoff
                : std::sin (juce::MathConstants<double>::twoPi * cutoff * x)
                    / (juce::MathConstants<double>::pi * x);
            coefficients[(size_t) i] = (float) (sinc * window);
            sum += sinc * window;
        }
        for (auto& c : coefficients) c /= (float) sum;
    }
    void reset() { history = {}; position = 0; }
    void process (const juce::AudioBuffer<float>& high, juce::AudioBuffer<float>& output,
                  int offset, int count)
    {
        for (int i = 0; i < count; ++i)
            for (int phase = 0; phase < factor; ++phase)
            {
                if (--position < 0) position = taps - 1;
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto& h = history[(size_t) ch];
                    h[(size_t) position] = h[(size_t) position + taps] = high.getSample (ch, i * factor + phase);
                    if (phase == 0)
                    {
                        const auto* data = h.data() + position;
                        float a = 0, b = 0, c = 0, d = 0;
                        for (int j = 0; j < taps - 1; j += 4)
                        {
                            a += data[j] * coefficients[(size_t) j];
                            b += data[j + 1] * coefficients[(size_t) j + 1];
                            c += data[j + 2] * coefficients[(size_t) j + 2];
                            d += data[j + 3] * coefficients[(size_t) j + 3];
                        }
                        output.setSample (ch, offset + i, a + b + c + d + data[taps - 1] * coefficients.back());
                    }
                }
            }
    }
private:
    std::array<float, taps> coefficients {};
    std::array<std::array<float, taps * 2>, 2> history {};
    int position = 0;
};
}
