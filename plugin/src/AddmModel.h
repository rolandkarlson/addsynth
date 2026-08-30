#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <memory>

// Parser for the .addm format written by additive/model.py:
//   "ADDM" | uint32 version | uint32 headerLen | JSON header |
//   float32 env[nFrames*nPartials] | float32 f0Track[nFrames] |
//   float32 noiseEnv[nFrames*nNoiseBands] | float32 bandFreqs[nNoiseBands]
struct AddmModel
{
    juce::String name;
    int nFrames = 0, nPartials = 0, nNoiseBands = 0;
    float f0Ref = 440.0f, controlRate = 172.0f, inharmonicity = 0.0f;
    std::vector<float> env;        // row-major (frame, partial)
    std::vector<float> f0Track;    // ratio to f0Ref
    std::vector<float> noiseEnv;   // row-major (frame, band)
    std::vector<float> bandFreqs;  // Hz

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

        char magic[4];
        in.read (magic, 4);
        if (memcmp (magic, "ADDM", 4) != 0) { error = "not an ADDM file"; return nullptr; }

        auto version = (juce::uint32) in.readInt();
        auto headerLen = (juce::uint32) in.readInt();
        if (version != 1) { error = "unsupported version"; return nullptr; }

        juce::MemoryBlock headerBytes;
        in.readIntoMemoryBlock (headerBytes, (int) headerLen);
        auto header = juce::JSON::parse (headerBytes.toString());
        if (header.isVoid()) { error = "bad JSON header"; return nullptr; }

        auto m = std::make_shared<AddmModel>();
        m->name          = header.getProperty ("name", "untitled").toString();
        m->nFrames       = (int) header.getProperty ("n_frames", 0);
        m->nPartials     = (int) header.getProperty ("n_partials", 0);
        m->nNoiseBands   = (int) header.getProperty ("n_noise_bands", 0);
        m->f0Ref         = (float) (double) header.getProperty ("f0_ref", 440.0);
        m->controlRate   = (float) (double) header.getProperty ("control_rate", 172.0);
        m->inharmonicity = (float) (double) header.getProperty ("inharmonicity", 0.0);

        if (m->nFrames <= 1 || m->nPartials <= 0) { error = "empty model"; return nullptr; }

        auto readFloats = [&in] (std::vector<float>& v, size_t count) -> bool
        {
            v.resize (count);
            return in.read (v.data(), (int) (count * sizeof (float)))
                     == (int) (count * sizeof (float));
        };
        if (! readFloats (m->env,       (size_t) m->nFrames * (size_t) m->nPartials)
         || ! readFloats (m->f0Track,   (size_t) m->nFrames)
         || ! readFloats (m->noiseEnv,  (size_t) m->nFrames * (size_t) m->nNoiseBands)
         || ! readFloats (m->bandFreqs, (size_t) m->nNoiseBands))
        {
            error = "truncated file";
            return nullptr;
        }
        return m;
    }
};
