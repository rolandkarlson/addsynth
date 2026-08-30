#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <complex>
#include "MorphField.h"

struct AddSound : public juce::SynthesiserSound
{
    bool appliesToNote (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
};

// One voice = sine bank driven by the morph field: per control frame the
// layers' log-envelopes are blended with inverse-distance weights from
// the pad cursor, exp'd, then shaped by the timbre macros (tilt, odd/even,
// partial count, drift) and lagged by Blur. Oscillators are phasor-rotation
// (complex multiply, no per-sample sin), plus white noise through a
// constant-Q bandpass bank.
class AddVoice : public juce::SynthesiserVoice
{
public:
    static constexpr int maxPartials = 128;
    static constexpr int maxBands = 32;

    struct Params
    {
        float noise = 1.0f;        // noise layer gain
        float release = 0.08f;     // s
        float attack = 0.001f;     // s
        float speed = 1.0f;        // envelope playback rate, 0 = freeze
        float blur = 0.0f;         // envelope lag, s
        float tiltDbOct = 0.0f;    // spectral tilt
        float oddEven = 0.0f;      // -1 evens only .. +1 odds only
        float stretchB = 0.0f;     // extra inharmonicity
        float partials = 128.0f;   // number of audible partials
        float drift = 0.0f;        // per-partial shimmer depth 0..1
        float pitchEnvAmt = 1.0f;  // scale of the analyzed pitch track
        float cursorX = 0.5f, cursorY = 0.5f;
        float spreadX = 0.0f, spreadY = 0.0f; // per-voice morph offset
        int   spreadN = 5;                    // voice index cycle length
        juce::uint16 keyMask = 0;  // enabled pitch classes (bit 0 = C); 0 = bypass
        float bend = 0.05f;        // glide time (s) between quantized notes
        float width = 0.0f;        // stereo: per-partial pan + L/R micro-detune
        float loopStart = 0.4f;    // sustain loop start, 0..1 of the envelope
        float loopLen = 0.0f;      // loop length, 0..1; 0 = looping off
    };

    void setField (std::shared_ptr<const MorphField> f) { pendingField = std::move (f); }
    void setParams (const Params& p) { params = p; }
    void setNoteCounter (std::atomic<int>* c) { noteCounter = c; }

    // effective pad position of this voice (for UI ghost cursors)
    bool getDisplayCursor (float& x, float& y) const
    {
        if (! isVoiceActive()) return false;
        x = cursorX; y = cursorY;
        return true;
    }

    bool canPlaySound (juce::SynthesiserSound* s) override
    {
        return dynamic_cast<AddSound*> (s) != nullptr;
    }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        field = pendingField;
        if (field == nullptr || field->layers.empty() || field->nFrames < 2)
        {
            clearCurrentNote();
            return;
        }

        sr = getSampleRate();
        midiNoteNum = midiNote;
        f0 = (float) juce::MidiMessage::getMidiNoteInHertz (midiNote);
        vel = 0.2f + 0.8f * velocity;
        quantMidi = (float) midiNote;
        firstQuant = true;
        framePos = 0.0;
        frameIncBase = field->controlRate / sr;
        releasing = false;
        releaseGain = 1.0f;
        attackGain = 0.0f;
        attackStep = 1.0f / (juce::jmax (0.001f, params.attack) * (float) sr);
        voiceIndex = noteCounter != nullptr
            ? noteCounter->fetch_add (1) % juce::jmax (1, params.spreadN)
            : 0;
        cursorX = wrap01 (params.cursorX + (float) voiceIndex * params.spreadX);
        cursorY = wrap01 (params.cursorY + (float) voiceIndex * params.spreadY);
        cursorCoef = 1.0f - std::exp (-1.0f / (0.03f * field->controlRate));

        nP = juce::jmin (field->nPartials, maxPartials);
        nB = juce::jmin (field->nBands, maxBands);
        for (int k = 0; k < nP; ++k)
        {
            zL[k] = zR[k] = { 1.0f, 0.0f };
            amp0[k] = amp1[k] = lag[k] = 0.0f;
            log2k[k] = std::log2 ((float) (k + 1));
            driftPhase[k] = rng.nextFloat() * juce::MathConstants<float>::twoPi;
            // each partial shimmers at its own slow rate, 0.1..1.6 Hz
            driftInc[k] = juce::MathConstants<float>::twoPi
                        * (0.1f + 1.5f * rng.nextFloat()) / field->controlRate;
            // fixed pseudo-random stereo personality per partial:
            // pan side and detune side (deterministic -> consistent timbre)
            juce::uint32 h = (juce::uint32) (k + 1) * 2654435761u;
            panSign[k] = ((h >> 13) & 1) ? 1.0f : -1.0f;
            detSign[k] = ((h >> 22) & 1) ? 1.0f : -1.0f;
        }
        for (int b = 0; b < nB; ++b)
            ng0[b] = ng1[b] = nlag[b] = 0.0f;

        currentFrame = -1;
        ctrlCountdown = 0;
        updateControlFrame (0);

        auto nyq = sr * 0.45;
        for (int b = 0; b < nB; ++b)
        {
            auto fc = juce::jlimit (30.0, nyq, (double) field->bandFreqs[(size_t) b]);
            bandFilter[b].coefficients =
                juce::dsp::IIR::Coefficients<float>::makeBandPass (sr, fc, 2.0f);
            bandFilter[b].reset();
        }
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            releasing = true;
            releaseStep = 1.0f / (juce::jmax (0.005f, params.release) * (float) sr);
        }
        else
            clearCurrentNote();
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (field == nullptr || ! isVoiceActive()) return;

        auto* l = out.getWritePointer (0, start);
        auto* r = out.getNumChannels() > 1 ? out.getWritePointer (1, start) : nullptr;
        double frameInc = frameIncBase * juce::jmax (0.0f, params.speed);

        for (int i = 0; i < num; ++i)
        {
            int frame = (int) framePos;
            if (frame >= field->nFrames - 1) { clearCurrentNote(); return; }
            // refresh at control rate even when frozen (speed = 0), so
            // morph, blur, drift and timbre knobs keep working
            if (frame != currentFrame || --ctrlCountdown <= 0)
                updateControlFrame (frame);
            float frac = (float) (framePos - frame);

            float sL = 0.0f, sR = 0.0f;
            for (int k = 0; k < nP; ++k)
            {
                zL[k] *= wL[k];
                zR[k] *= wR[k];
                float a = amp0[k] + (amp1[k] - amp0[k]) * frac;
                sL += a * gL[k] * zL[k].imag();
                sR += a * gR[k] * zR[k].imag();
            }

            if (params.noise > 0.0f && nB > 0)
            {
                float n = rng.nextFloat() * 2.0f - 1.0f;
                for (int b = 0; b < nB; ++b)
                {
                    float g = ng0[b] + (ng1[b] - ng0[b]) * frac;
                    float y = g * bandFilter[b].processSample (n);
                    sL += params.noise * ngL[b] * y;
                    sR += params.noise * ngR[b] * y;
                }
            }

            float env = vel;
            if (attackGain < 1.0f)
            {
                attackGain += attackStep;
                if (attackGain > 1.0f) attackGain = 1.0f;
                env *= attackGain;
            }
            if (releasing)
            {
                releaseGain -= releaseStep;
                if (releaseGain <= 0.0f) { clearCurrentNote(); return; }
                env *= releaseGain;
            }

            l[i] += sL * env;
            if (r != nullptr) r[i] += sR * env;
            framePos += frameInc;

            // sustain loop: while the key is held, cycle a region of the
            // envelope; release lets it play out past the loop naturally.
            // Phasor oscillators keep their phase across the jump, and the
            // amp targets lerp from the pre-jump values over one control
            // frame, so the wrap is click-free.
            if (! releasing && params.loopLen > 0.001f)
            {
                double last = (double) (field->nFrames - 1);
                double ls = params.loopStart * last;
                double le = juce::jmin (last,
                    ls + juce::jmax (0.02f, params.loopLen) * last);
                if (le > ls + 2.0 && framePos >= le)
                    framePos = ls + std::fmod (framePos - le, le - ls);
            }
        }
    }

private:
    static float wrap01 (float v) noexcept { return v - std::floor (v); }

    static float nearestAllowedMidi (float m, juce::uint16 mask) noexcept
    {
        int center = (int) std::lround (m);
        float best = m, bestDist = 1e9f;
        for (int cand = center - 12; cand <= center + 12; ++cand)
        {
            int pc = ((cand % 12) + 12) % 12;
            if ((mask >> pc) & 1)
            {
                float d = std::abs ((float) cand - m);
                if (d < bestDist) { bestDist = d; best = (float) cand; }
            }
        }
        return best;
    }

    void updateControlFrame (int frame)
    {
        currentFrame = frame;
        ctrlCountdown = juce::jmax (8, (int) (sr / field->controlRate));
        auto& fld = *field;
        int f1 = juce::jmin (frame + 1, fld.nFrames - 1);
        auto nLayers = fld.layers.size();

        // this voice's own pad position: global cursor + wrapped spread offset
        float tx = wrap01 (params.cursorX + (float) voiceIndex * params.spreadX);
        float ty = wrap01 (params.cursorY + (float) voiceIndex * params.spreadY);
        cursorX += (tx - cursorX) * cursorCoef;
        cursorY += (ty - cursorY) * cursorCoef;
        float wts[MorphField::maxLayers];
        fld.computeWeights (cursorX, cursorY, wts);

        float ratio = 0.0f, B = 0.0f;
        for (size_t li = 0; li < nLayers; ++li)
        {
            ratio += wts[li] * fld.layers[li].f0Track[(size_t) f1];
            B += wts[li] * fld.layers[li].inharmonicity;
        }
        if (ratio <= 0.0f) ratio = 1.0f;
        B += params.stretchB;                                 // Stretch

        // Pitch Env scales the track in semitone space (a linear-ratio
        // scale would go negative on deep dips at high amounts)
        float envSemis = 12.0f * std::log2 (juce::jlimit (0.25f, 4.0f, ratio))
                       * params.pitchEnvAmt;

        // ---- scale quantizer: snap the sounding pitch to the enabled
        // pitch classes (any octave), gliding over `bend` seconds --------
        float soundingMidi = (float) midiNoteNum + envSemis;
        float targetMidi = params.keyMask != 0
                         ? nearestAllowedMidi (soundingMidi, params.keyMask)
                         : soundingMidi;
        if (firstQuant || params.keyMask == 0)
        {
            quantMidi = targetMidi;   // no glide at note start / when bypassed
            firstQuant = false;
        }
        else
        {
            float bendCoef = params.bend <= 0.002f ? 1.0f
                : 1.0f - std::exp (-1.0f / (params.bend * fld.controlRate));
            quantMidi += (targetMidi - quantMidi) * bendCoef;
        }
        float baseFreq = 440.0f * std::exp2 ((quantMidi - 69.0f) / 12.0f);

        // timbre macro coefficients (constant across partials this frame)
        const float tiltCoef = params.tiltDbOct * 0.1151293f; // ln(10)/20
        const float oddG = juce::jmin (1.0f, 1.0f + params.oddEven);
        const float evenG = juce::jmin (1.0f, 1.0f - params.oddEven);
        const float blurAlpha = params.blur <= 0.001f ? 1.0f
            : 1.0f - std::exp (-1.0f / (params.blur * fld.controlRate));
        const float nPart = juce::jlimit (1.0f, (float) nP, params.partials);

        // stereo width: pan spread grows with harmonic number (fundamental
        // stays centered), micro-detune decorrelates L/R phase over time
        const float widthPan = 0.85f * params.width;
        const float widthDet = 2.0f * params.width * 5.7735e-4f; // ~2 cents

        auto nyq = 0.98f * (float) sr * 0.5f;
        for (int k = 0; k < nP; ++k)
        {
            amp0[k] = amp1[k];

            float kk = (float) (k + 1);
            float freq = baseFreq * kk * std::sqrt (1.0f + B * kk * kk);
            if (freq >= nyq)
            {
                lag[k] += (0.0f - lag[k]) * blurAlpha;
                amp1[k] = lag[k];
                wL[k] = wR[k] = { 1.0f, 0.0f };
                continue;
            }

            float acc = 0.0f;
            for (size_t li = 0; li < nLayers; ++li)
                acc += wts[li] * fld.logEnvAt (fld.layers[li], f1, k);
            float tgt = std::exp (acc) - MorphField::EPS;
            if (tgt < 0.0f) tgt = 0.0f;

            // Tilt / Odd-Even / Partials mask / Drift
            // (tilt pivots around partial 8 and is capped at +18 dB so
            // extreme settings recolor instead of exploding)
            float g = juce::jmin (8.0f, std::exp (tiltCoef * (log2k[k] - 3.0f)));
            g *= ((k + 1) & 1) ? evenG : oddG;   // k+1 even -> evenG
            float over = kk - nPart;              // smooth partial-count edge
            if (over > 0.0f) g *= juce::jmax (0.0f, 1.0f - over);
            if (params.drift > 0.0f)
            {
                driftPhase[k] += driftInc[k];
                g *= 1.0f + 0.9f * params.drift * std::sin (driftPhase[k]);
            }
            tgt *= g;

            lag[k] += (tgt - lag[k]) * blurAlpha;  // Blur
            amp1[k] = lag[k];

            float pan = widthPan * panSign[k]
                      * juce::jmin (1.0f, kk / 6.0f);      // lows centered
            gL[k] = std::sqrt (1.0f - pan);
            gR[k] = std::sqrt (1.0f + pan);

            float det = widthDet * detSign[k];
            float ph = juce::MathConstants<float>::twoPi * freq / (float) sr;
            float phL = ph * (1.0f + det), phR = ph * (1.0f - det);
            wL[k] = { std::cos (phL), std::sin (phL) };
            wR[k] = { std::cos (phR), std::sin (phR) };
            float m2 = std::norm (zL[k]);
            if (m2 > 0.0f) zL[k] *= (1.5f - 0.5f * m2); // 1st-order renorm
            m2 = std::norm (zR[k]);
            if (m2 > 0.0f) zR[k] *= (1.5f - 0.5f * m2);
        }

        for (int b = 0; b < nB; ++b)
        {
            ng0[b] = ng1[b];
            float acc = 0.0f;
            for (size_t li = 0; li < nLayers; ++li)
                acc += wts[li] * fld.logNoiseAt (fld.layers[li], f1, b);
            float tgt = juce::jmax (0.0f, std::exp (acc) - MorphField::EPS);
            nlag[b] += (tgt - nlag[b]) * blurAlpha;
            ng1[b] = nlag[b];

            float pan = 0.7f * params.width * ((b & 1) ? 1.0f : -1.0f);
            ngL[b] = std::sqrt (1.0f - pan);
            ngR[b] = std::sqrt (1.0f + pan);
        }
    }

    std::shared_ptr<const MorphField> pendingField, field;
    Params params;
    std::atomic<int>* noteCounter = nullptr;
    int voiceIndex = 0;
    int midiNoteNum = 60;
    float quantMidi = 60.0f;
    bool firstQuant = true;
    double sr = 44100.0, framePos = 0.0, frameIncBase = 0.0;
    float f0 = 440.0f, vel = 1.0f;
    int nP = 0, nB = 0, currentFrame = -1, ctrlCountdown = 0;
    bool releasing = false;
    float releaseGain = 1.0f, releaseStep = 0.001f;
    float attackGain = 1.0f, attackStep = 1.0f;
    float cursorX = 0.5f, cursorY = 0.5f, cursorCoef = 0.1f;

    std::complex<float> zL[maxPartials], wL[maxPartials];
    std::complex<float> zR[maxPartials], wR[maxPartials];
    float amp0[maxPartials] {}, amp1[maxPartials] {}, lag[maxPartials] {};
    float gL[maxPartials] {}, gR[maxPartials] {};
    float panSign[maxPartials] {}, detSign[maxPartials] {};
    float log2k[maxPartials] {}, driftPhase[maxPartials] {}, driftInc[maxPartials] {};
    float ng0[maxBands] {}, ng1[maxBands] {}, nlag[maxBands] {};
    float ngL[maxBands] {}, ngR[maxBands] {};
    juce::dsp::IIR::Filter<float> bandFilter[maxBands];
    juce::Random rng;
};
