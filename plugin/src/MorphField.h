#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "AddmModel.h"

// A set of models placed on the XY morph pad, pre-resampled onto one
// common control grid so voices can blend them per control frame.
// Envelopes are stored as log(amp + EPS): morphing = weighted sum of
// logs, then exp — same log-domain interpolation as the Python side.
struct MorphField
{
    static constexpr float EPS = 1e-7f;
    static constexpr int maxLayers = 16;

    struct Playback
    {
        float start = 0.0f, loopIn = 0.0f, loopOut = 1.0f;
        bool loop = false;

        void sanitise()
        {
            auto unit = [] (float value, float fallback)
            { return std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : fallback; };
            start = unit (start, 0.0f);
            loopIn = juce::jmin (0.999f, unit (loopIn, 0.0f));
            loopOut = juce::jmax (loopIn + 0.001f, unit (loopOut, 1.0f));
            if (loop) start = juce::jmin (start, loopOut - 0.001f);
        }
    };

    // Per-model sculpting: a level contour over the model's own timeline and a
    // gain per harmonic. Both are baked into the layer when the field is built,
    // so they cost nothing on the audio thread and morph like the source data.
    struct Shape
    {
        static constexpr int envelopePoints = 64;
        static constexpr int maxPartials = 192;
        std::array<float, envelopePoints> envelope;
        std::array<float, maxPartials> partials;

        Shape() { envelope.fill (1.0f); partials.fill (1.0f); }

        float envelopeAt (double fraction) const noexcept
        {
            const double at = juce::jlimit (0.0, 1.0, fraction) * (envelopePoints - 1);
            const int a = (int) at, b = std::min (a + 1, envelopePoints - 1);
            const float mix = (float) (at - a);
            return envelope[(size_t) a] * (1.0f - mix) + envelope[(size_t) b] * mix;
        }

        bool isNeutral() const noexcept
        {
            const auto unity = [] (float value) { return value == 1.0f; };
            return std::all_of (envelope.begin(), envelope.end(), unity)
                && std::all_of (partials.begin(), partials.end(), unity);
        }

        void sanitise()
        {
            const auto unit = [] (float& value)
            { value = std::isfinite (value) ? juce::jlimit (0.0f, 1.0f, value) : 1.0f; };
            for (auto& value : envelope) unit (value);
            for (auto& value : partials) unit (value);
        }
    };

    struct Layer
    {
        float x = 0.5f, y = 0.5f;
        float inharmonicity = 0.0f;
        juce::String name;
        std::vector<float> detune;    // nPartials, freq ratio vs k*f0
        std::vector<float> logEnv;    // nFrames * nPartials
        std::vector<float> logNoise;  // nFrames * nBands
        std::vector<float> f0Track;   // nFrames, ratio
        Playback playback;
        double sourceLast = 1.0;

        double sourcePosition (double elapsed) const noexcept
        {
            double position = elapsed + playback.start * sourceLast;
            if (playback.loop)
            {
                const double begin = playback.loopIn * sourceLast;
                const double end = playback.loopOut * sourceLast;
                if (position >= end)
                    position = begin + std::fmod (position - end, end - begin);
            }
            return position;
        }
    };

    int nFrames = 0, nPartials = 0, nBands = 0;
    float controlRate = 172.0f;
    std::vector<Layer> layers;
    std::vector<float> bandFreqs;
    bool hasModelLoop = false;

    float sampleEnvelope (const Layer& layer, double elapsed, int partial) const noexcept
    {
        const double position = layer.sourcePosition (elapsed);
        if (position > layer.sourceLast) return std::log (EPS);
        const int a = juce::jlimit (0, nFrames - 1, (int) position);
        const int b = std::min (a + 1, nFrames - 1);
        const float mix = (float) (position - a);
        return logEnvAt (layer, a, partial) * (1.0f - mix) + logEnvAt (layer, b, partial) * mix;
    }

    float samplePitch (const Layer& layer, double elapsed) const noexcept
    {
        const double position = juce::jlimit (0.0, (double) nFrames - 1,
                                              std::min (layer.sourceLast, layer.sourcePosition (elapsed)));
        const int a = (int) position, b = std::min (a + 1, nFrames - 1);
        const float mix = (float) (position - a);
        return layer.f0Track[(size_t) a] * (1.0f - mix) + layer.f0Track[(size_t) b] * mix;
    }

    float logEnvAt (const Layer& l, int frame, int k) const noexcept
    {
        return l.logEnv[(size_t) frame * (size_t) nPartials + (size_t) k];
    }
    float logNoiseAt (const Layer& l, int frame, int b) const noexcept
    {
        return l.logNoise[(size_t) frame * (size_t) nBands + (size_t) b];
    }

    // inverse-distance-squared weights for a cursor position, normalized
    void computeWeights (float cx, float cy, float* w) const noexcept
    {
        float sum = 0.0f;
        for (size_t i = 0; i < layers.size(); ++i)
        {
            auto dx = layers[i].x - cx, dy = layers[i].y - cy;
            w[i] = 1.0f / (dx * dx + dy * dy + 1e-4f);
            sum += w[i];
        }
        if (sum > 0)
            for (size_t i = 0; i < layers.size(); ++i)
                w[i] /= sum;
    }

    struct Placement
    {
        std::shared_ptr<const AddmModel> model;
        float x, y;
        Playback playback {};
        Shape shape {};
    };

    static std::shared_ptr<const MorphField> build (const std::vector<Placement>& slots)
    {
        auto f = std::make_shared<MorphField>();
        if (slots.empty())
            return f;

        // Choose the common rate before sizing any layer (mixed-rate models).
        for (auto& s : slots) f->controlRate = std::max (f->controlRate, s.model->controlRate);
        for (auto& s : slots)
        {
            if ((int) f->layers.size() >= maxLayers) break;
            auto& m = *s.model;
            f->controlRate = std::max (f->controlRate, m.controlRate);
            f->nPartials = std::max (f->nPartials, std::min (m.nPartials, Shape::maxPartials));
            if (m.nNoiseBands > f->nBands)
            {
                f->nBands = std::min (m.nNoiseBands, 48);
                f->bandFreqs.assign (m.bandFreqs.begin(),
                                     m.bandFreqs.begin() + f->nBands);
            }
            int frames = (int) std::ceil (m.nFrames * f->controlRate / m.controlRate);
            f->nFrames = std::max (f->nFrames, frames);
            f->layers.push_back ({});
        }
        f->nFrames = std::max (f->nFrames, 2);

        const float logEps = std::log (EPS);
        for (size_t li = 0; li < f->layers.size(); ++li)
        {
            auto& m = *slots[li].model;
            auto& L = f->layers[li];
            L.x = slots[li].x;
            L.y = slots[li].y;
            L.inharmonicity = m.inharmonicity;
            L.name = m.name;
            L.playback = slots[li].playback;
            L.playback.sanitise();
            L.sourceLast = (m.nFrames - 1) * (double) f->controlRate / m.controlRate;
            auto shape = slots[li].shape;
            shape.sanitise();
            f->hasModelLoop = f->hasModelLoop || L.playback.loop;
            L.detune.assign ((size_t) f->nPartials, 1.0f);
            for (int k = 0; k < std::min (m.nPartials, f->nPartials); ++k)
                L.detune[(size_t) k] = m.detune[(size_t) k];
            L.logEnv.assign ((size_t) f->nFrames * (size_t) f->nPartials, logEps);
            L.logNoise.assign ((size_t) f->nFrames * (size_t) f->nBands, logEps);
            L.f0Track.assign ((size_t) f->nFrames, 1.0f);

            // map common frame t -> source frame position, linear interp,
            // frames past the source end stay silent (logEps)
            for (int t = 0; t < f->nFrames; ++t)
            {
                float sp = t * m.controlRate / f->controlRate;
                if (sp > (float) (m.nFrames - 1))
                {
                    L.f0Track[(size_t) t] = m.f0Track[(size_t) m.nFrames - 1];
                    continue;
                }
                int i0 = (int) sp;
                int i1 = std::min (i0 + 1, m.nFrames - 1);
                float fr = sp - (float) i0;
                const float level = shape.envelopeAt (sp / (double) (m.nFrames - 1));

                for (int k = 0; k < std::min (m.nPartials, f->nPartials); ++k)
                {
                    float a = (m.envAt (i0, k) * (1 - fr) + m.envAt (i1, k) * fr)
                            * level * shape.partials[(size_t) k];
                    L.logEnv[(size_t) t * (size_t) f->nPartials + (size_t) k]
                        = std::log (a + EPS);
                }
                for (int b = 0; b < std::min (m.nNoiseBands, f->nBands); ++b)
                {
                    float a = (m.noiseAt (i0, b) * (1 - fr) + m.noiseAt (i1, b) * fr) * level;
                    L.logNoise[(size_t) t * (size_t) f->nBands + (size_t) b]
                        = std::log (a + EPS);
                }
                L.f0Track[(size_t) t] = m.f0Track[(size_t) i0] * (1 - fr)
                                      + m.f0Track[(size_t) i1] * fr;
            }
        }
        return f;
    }
};
