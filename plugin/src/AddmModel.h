#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <limits>

// Parser for the .addm format written by additive/model.py:
//   "ADDM" | uint32 version | uint32 headerLen | JSON header |
//   float32 env[nFrames*nPartials] | float32 f0Track[nFrames] |
//   float32 noiseEnv[nFrames*nNoiseBands] | float32 bandFreqs[nNoiseBands] |
//   float32 detune[nPartials]   (per-partial frequency ratio, ~1.0)
struct AddmModel
{
    juce::String name;
    int nFrames = 0, nPartials = 0, nNoiseBands = 0;
    float f0Ref = 440.0f, controlRate = 172.0f, inharmonicity = 0.0f;
    std::vector<float> env;        // row-major (frame, partial)
    std::vector<float> f0Track;    // ratio to f0Ref
    std::vector<float> noiseEnv;   // row-major (frame, band)
    std::vector<float> bandFreqs;  // Hz
    std::vector<float> detune;     // per-partial freq ratio vs k*f0

    float envAt (int frame, int partial) const noexcept
    {
        return env[(size_t) frame * (size_t) nPartials + (size_t) partial];
    }
    float noiseAt (int frame, int band) const noexcept
    {
        return noiseEnv[(size_t) frame * (size_t) nNoiseBands + (size_t) band];
    }

    static std::shared_ptr<AddmModel> load (const juce::File& file, juce::String& error)
    {
        juce::FileInputStream in (file);
        if (! in.openedOk()) { error = "cannot open file"; return nullptr; }
        return read (in, error);
    }

    // Session-embedded audio imports use the same bytes as standalone ADDM files.
    static std::shared_ptr<AddmModel> read (juce::InputStream& in, juce::String& error)
    {
        error.clear();
        char prefix[12] {};
        if (! readBytes (in, prefix, sizeof (prefix)))
        { error = "truncated ADDM header"; return nullptr; }
        if (memcmp (prefix, "ADDM", 4) != 0)
        { error = "not an ADDM file"; return nullptr; }

        const auto version = juce::ByteOrder::littleEndianInt (prefix + 4);
        const auto headerLen = juce::ByteOrder::littleEndianInt (prefix + 8);
        if (version != 2)
        {
            error = "model format v" + juce::String (version)
                  + " — re-analyze the source (plugin needs v2)";
            return nullptr;
        }
        if (headerLen == 0 || headerLen > maxHeaderBytes)
        { error = "invalid ADDM header length"; return nullptr; }

        juce::MemoryBlock headerBytes ((size_t) headerLen);
        if (! readBytes (in, headerBytes.getData(), (size_t) headerLen))
        { error = "truncated ADDM header"; return nullptr; }
        auto header = juce::JSON::parse (headerBytes.toString());
        if (! header.isObject()) { error = "bad JSON header"; return nullptr; }

        auto m = std::make_shared<AddmModel>();
        m->name          = header.getProperty ("name", "untitled").toString();
        if (! readDimension (header.getProperty ("n_frames", 0), m->nFrames)
         || ! readDimension (header.getProperty ("n_partials", 0), m->nPartials)
         || ! readDimension (header.getProperty ("n_noise_bands", 0), m->nNoiseBands))
        { error = "invalid model dimensions"; return nullptr; }
        m->f0Ref         = (float) (double) header.getProperty ("f0_ref", 440.0);
        m->controlRate   = (float) (double) header.getProperty ("control_rate", 172.0);
        m->inharmonicity = (float) (double) header.getProperty ("inharmonicity", 0.0);

        if (! m->validDimensions())
        { error = "invalid or oversized model (maximum 256 MiB)"; return nullptr; }
        const auto payloadBytes = m->sampleCount() * sizeof (float);
        if (in.getTotalLength() >= 0 && in.getTotalLength() - in.getPosition() < (juce::int64) payloadBytes)
        { error = "truncated model data"; return nullptr; }

        auto readFloats = [&in] (std::vector<float>& v, size_t count) -> bool
        {
            v.resize (count);
            if (! readBytes (in, v.data(), count * sizeof (float))) return false;
           #if JUCE_BIG_ENDIAN
            for (auto& value : v)
            {
                const auto bits = juce::ByteOrder::littleEndianInt (&value);
                std::memcpy (&value, &bits, sizeof (value));
            }
           #endif
            return true;
        };
        if (! readFloats (m->env,       (size_t) m->nFrames * (size_t) m->nPartials)
         || ! readFloats (m->f0Track,   (size_t) m->nFrames)
         || ! readFloats (m->noiseEnv,  (size_t) m->nFrames * (size_t) m->nNoiseBands)
         || ! readFloats (m->bandFreqs, (size_t) m->nNoiseBands)
         || ! readFloats (m->detune,    (size_t) m->nPartials))
        {
            error = "truncated file";
            return nullptr;
        }
        if (! m->validData())
        { error = "model contains invalid spectral data"; return nullptr; }
        return m;
    }

    bool write (juce::OutputStream& out) const
    {
        if (! validDimensions() || ! validData()) return false;

        auto* properties = new juce::DynamicObject();
        const juce::var header (properties);
        properties->setProperty ("name", name);
        properties->setProperty ("n_frames", nFrames);
        properties->setProperty ("n_partials", nPartials);
        properties->setProperty ("n_noise_bands", nNoiseBands);
        properties->setProperty ("f0_ref", f0Ref);
        properties->setProperty ("control_rate", controlRate);
        properties->setProperty ("inharmonicity", inharmonicity);
        const auto json = juce::JSON::toString (header, true);
        const auto headerBytes = json.getNumBytesAsUTF8();
        if (headerBytes > maxHeaderBytes) return false;
        if (! out.write ("ADDM", 4) || ! out.writeInt (2)
         || ! out.writeInt ((int) headerBytes) || ! out.write (json.toRawUTF8(), headerBytes))
            return false;

        for (const auto* values : { &env, &f0Track, &noiseEnv, &bandFreqs, &detune })
        {
           #if JUCE_BIG_ENDIAN
            for (float value : *values)
                if (! out.writeFloat (value)) return false;
           #else
            if (! values->empty() && ! out.write (values->data(), values->size() * sizeof (float)))
                return false;
           #endif
        }
        return true;
    }

private:
    static constexpr size_t maxHeaderBytes = 1024 * 1024;
    static constexpr juce::uint64 maxPayloadBytes = 256 * 1024 * 1024;

    static bool readBytes (juce::InputStream& input, void* destination, size_t count)
    {
        auto* cursor = static_cast<char*> (destination);
        while (count > 0)
        {
            const int read = input.read (cursor, (int) count);
            if (read <= 0) return false;
            count -= (size_t) read;
            cursor += read;
        }
        return true;
    }

    static bool readDimension (const juce::var& value, int& result)
    {
        if (! value.isInt() && ! value.isInt64() && ! value.isDouble()) return false;
        const double number = (double) value;
        if (! std::isfinite (number) || number < 0 || number > std::numeric_limits<int>::max()
            || std::floor (number) != number) return false;
        result = (int) number;
        return true;
    }

    juce::uint64 sampleCount() const
    {
        return (juce::uint64) nFrames * ((juce::uint64) nPartials + (juce::uint64) nNoiseBands + 1)
            + (juce::uint64) nPartials + (juce::uint64) nNoiseBands;
    }

    bool validDimensions() const
    {
        return nFrames > 1 && nPartials > 0 && nNoiseBands >= 0
            && sampleCount() <= maxPayloadBytes / sizeof (float)
            && std::isfinite (f0Ref) && f0Ref > 0
            && std::isfinite (controlRate) && controlRate > 0
            && std::isfinite (inharmonicity) && inharmonicity >= 0;
    }

    bool validData() const
    {
        if (env.size() != (size_t) nFrames * (size_t) nPartials
         || f0Track.size() != (size_t) nFrames
         || noiseEnv.size() != (size_t) nFrames * (size_t) nNoiseBands
         || bandFreqs.size() != (size_t) nNoiseBands || detune.size() != (size_t) nPartials)
            return false;
        const auto nonnegative = [] (float value) { return std::isfinite (value) && value >= 0; };
        const auto positive = [] (float value) { return std::isfinite (value) && value > 0; };
        return std::all_of (env.begin(), env.end(), nonnegative)
            && std::all_of (noiseEnv.begin(), noiseEnv.end(), nonnegative)
            && std::all_of (f0Track.begin(), f0Track.end(), positive)
            && std::all_of (bandFreqs.begin(), bandFreqs.end(), positive)
            && std::all_of (detune.begin(), detune.end(), positive);
    }
};
