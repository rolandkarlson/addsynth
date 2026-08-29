#pragma once
#include <juce_core/juce_core.h>
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

    struct Layer
    {
        float x = 0.5f, y = 0.5f;
        float inharmonicity = 0.0f;
        juce::String name;
        std::vector<float> logEnv;    // nFrames * nPartials
        std::vector<float> logNoise;  // nFrames * nBands
        std::vector<float> f0Track;   // nFrames, ratio
    };

    int nFrames = 0, nPartials = 0, nBands = 0;
    float controlRate = 172.0f;
    std::vector<Layer> layers;
    std::vector<float> bandFreqs;

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
    };

    static std::shared_ptr<const MorphField> build (const std::vector<Placement>& slots)
    {
        auto f = std::make_shared<MorphField>();
        if (slots.empty())
            return f;

        for (auto& s : slots)
        {
            if ((int) f->layers.size() >= maxLayers) break;
            auto& m = *s.model;
            f->controlRate = std::max (f->controlRate, m.controlRate);
            f->nPartials = std::max (f->nPartials, std::min (m.nPartials, 128));
            if (m.nNoiseBands > f->nBands)
            {
                f->nBands = std::min (m.nNoiseBands, 32);
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

                for (int k = 0; k < std::min (m.nPartials, f->nPartials); ++k)
                {
                    float a = m.envAt (i0, k) * (1 - fr) + m.envAt (i1, k) * fr;
                    L.logEnv[(size_t) t * (size_t) f->nPartials + (size_t) k]
                        = std::log (a + EPS);
                }
                for (int b = 0; b < std::min (m.nNoiseBands, f->nBands); ++b)
                {
                    float a = m.noiseAt (i0, b) * (1 - fr) + m.noiseAt (i1, b) * fr;
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
