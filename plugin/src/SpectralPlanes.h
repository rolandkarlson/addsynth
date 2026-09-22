#pragma once
#include "SineLab.h"

namespace sineLab
{
struct SpectralPlane
{
    Mask u {}, v {};
    bool addedCompanion = false;

    static float dot (const Mask& a, const Mask& b)
    {
        double sum = 0;
        for (size_t k = 0; k < a.size(); ++k) sum += (double) a[k] * b[k];
        return (float) sum;
    }
    static bool normalise (Mask& a)
    {
        double energy = 0.0;
        for (float value : a) energy += (double) value * value;
        const double length = std::sqrt (energy);
        if (! std::isfinite (length) || length < 1e-8f) return false;
        for (auto& value : a) value = (float) ((double) value / length);
        return true;
    }
    static SpectralPlane fromSpectra (Mask first, Mask second)
    {
        SpectralPlane result;
        for (auto* values : { &first, &second })
            for (auto& value : *values) if (! std::isfinite (value)) value = 0.0f;
        if (! normalise (first))
        {
            first = second;
            if (! normalise (first)) first[0] = 1.0f;
        }
        normalise (second);
        const float projection = dot (first, second);
        for (size_t k = 0; k < second.size(); ++k) second[k] -= projection * first[k];
        if (dot (second, second) < 1e-8f)
        {
            // A static/one-partial model cannot define a rank-two plane alone.
            // Construct an explicit, deterministic companion and report it.
            result.addedCompanion = true;
            for (size_t k = 0; k < second.size(); ++k)
                second[k] = first[k] * ((k & 1u) == 0 ? 1.0f : -1.0f);
            float along = dot (first, second);
            for (size_t k = 0; k < second.size(); ++k) second[k] -= along * first[k];
            if (! normalise (second))
            {
                size_t least = 0;
                for (size_t k = 1; k < first.size(); ++k)
                    if (std::abs (first[k]) < std::abs (first[least])) least = k;
                second.fill (0.0f);
                second[least] = 1.0f;
                along = first[least];
                for (size_t k = 0; k < second.size(); ++k) second[k] -= along * first[k];
            }
        }
        // Reorthogonalise to protect nearly dependent captured spectra.
        const float residual = dot (first, second);
        for (size_t k = 0; k < second.size(); ++k) second[k] -= residual * first[k];
        normalise (second);
        result.u = first;
        result.v = second;
        return result;
    }

    static SpectralPlane blend (const SpectralPlane& a, const SpectralPlane& b, float amount)
    {
        // Align bases with a 2x2 orthogonal Procrustes transform, including
        // reflections. A sign flip or swapped basis must not change the plane.
        const float aa = dot (a.u, b.u), ab = dot (a.u, b.v);
        const float ba = dot (a.v, b.u), bb = dot (a.v, b.v);
        const float rot = std::hypot (aa + bb, ab - ba);
        const float ref = std::hypot (aa - bb, ab + ba);
        const bool reflection = ref > rot;
        const float angle = reflection ? std::atan2 (ab + ba, aa - bb)
                                       : std::atan2 (ab - ba, aa + bb);
        const float c = std::cos (angle), s = std::sin (angle);
        Mask first, second;
        amount = juce::jlimit (0.0f, 1.0f, amount);
        for (size_t k = 0; k < first.size(); ++k)
        {
            const float bu = b.u[k] * c + b.v[k] * s;
            const float bv = reflection ? b.u[k] * s - b.v[k] * c : -b.u[k] * s + b.v[k] * c;
            first[k] = a.u[k] * (1.0f - amount) + bu * amount;
            second[k] = a.v[k] * (1.0f - amount) + bv * amount;
        }
        return fromSpectra (first, second);
    }

    Mask orbit (double phase, float shape) const
    {
        const float c = (float) std::cos (phase * juce::MathConstants<double>::twoPi);
        const float s = (float) std::sin (phase * juce::MathConstants<double>::twoPi)
                      * (0.15f + 0.85f * juce::jlimit (0.0f, 1.0f, shape));
        Mask result;
        float peak = 1e-8f;
        for (size_t k = 0; k < result.size(); ++k)
        {
            result[k] = std::abs (u[k] * c + v[k] * s);
            peak = juce::jmax (peak, result[k]);
        }
        for (auto& value : result) value /= peak;
        return result;
    }
};

inline std::array<SpectralPlane, 2> defaultPlanes()
{
    std::array<SpectralPlane, 2> result;
    for (int plane = 0; plane < 2; ++plane)
    {
        Mask a, b;
        for (int k = 0; k < partials; ++k)
        {
            const float phase = (float) k * (plane == 0 ? 0.17f : 0.31f);
            a[(size_t) k] = 0.15f + 0.85f * std::pow (0.5f + 0.5f * std::cos (phase), 2.0f);
            b[(size_t) k] = 0.15f + 0.85f * std::pow (0.5f + 0.5f * std::sin (phase), 2.0f);
        }
        result[(size_t) plane] = SpectralPlane::fromSpectra (a, b);
    }
    return result;
}
}
