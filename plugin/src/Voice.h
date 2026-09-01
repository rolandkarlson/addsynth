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
    static constexpr int maxPartials = 192;
    static constexpr int maxBands = 48;

    struct Params
    {
        float noise = 1.0f;        // noise layer gain
        float release = 0.08f;     // s
        float attack = 0.0f;       // s (0 = instant)
        float speed = 1.0f;        // envelope playback rate, 0 = freeze
        float blur = 0.0f;         // envelope lag, s
        float tiltDbOct = 0.0f;    // spectral tilt
        float oddEven = 0.0f;      // -1 evens only .. +1 odds only
        float stretchB = 0.0f;     // extra inharmonicity
        float partials = 192.0f;   // number of audible partials
        float drift = 0.0f;        // per-partial shimmer depth 0..1
        float pitchEnvAmt = 1.0f;  // scale of the analyzed pitch track
        float cursorX = 0.5f, cursorY = 0.5f;
        float spreadX = 0.0f, spreadY = 0.0f; // per-voice morph offset
        int   spreadN = 5;                    // voice index cycle length
        juce::uint16 keyMask = 0;  // enabled pitch classes (bit 0 = C); 0 = bypass
        bool  midiScale = false;   // keyMask mirrors currently held notes
        float bend = 0.05f;        // glide time (s) between quantized notes
        float width = 0.0f;        // stereo: per-partial pan + L/R micro-detune
        bool  modal = false;       // enable the modal/resonator mode
        float ring = 0.0f;         // resonator decay time (s), 0 = off
        float damp = 0.4f;         // high partials decay faster (0..1)
        float decay = 0.0f;        // ADSR decay to sustain (s), 0 = no decay stage
        float sustain = 1.0f;      // ADSR sustain level of the model/exciter
        float loopStart = 0.4f;    // sustain loop start, 0..1 of the envelope
        float loopLen = 0.0f;      // loop length, 0..1; 0 = looping off
        bool  syncLoop = false;    // loopLen picks beats (1/4..16) at host bpm
        bool  syncSpeed = false;   // speed picks bars (1/4..8) per full scan
        float bpm = 120.0f;        // host tempo (fallback 120)

        // ---- modulation matrix (all applied at control rate) ----
        static constexpr int nModSlots = 6;
        // sources: 0 off, 1 lfo1, 2 lfo2, 3 env pos, 4 voice idx,
        //          5 random, 6 velocity, 7 note
        // dests:   0 morph x, 1 morph y, 2 tilt, 3 blur, 4 speed, 5 noise,
        //          6 width, 7 stretch, 8 odd/even, 9 partials, 10 pitch,
        //          11 loop pos
        int   modSrc[nModSlots] {};
        int   modDst[nModSlots] {};
        float modAmt[nModSlots] {};
        float lfo1 = 0.0f, lfo2 = 0.0f;   // current global LFO values, -1..1
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
        velocity01 = velocity;
        noteRandom = rng.nextFloat();
        quantMidi = (float) midiNote;
        firstQuant = true;
        lastHeldMask = params.keyMask;
        framePos = 0.0;
        frameIncBase = field->controlRate / sr;
        releasing = false;
        loopArmed = false;
        releaseGain = 1.0f;
        attackGain = 0.0f;
        decayLevel = 1.0f;
        ringMax = 0.0f;
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
            amp0[k] = amp1[k] = lag[k] = ringAmp[k] = 0.0f;
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
            ng0[b] = ng1[b] = nlag[b] = ringNoise[b] = 0.0f;

        currentFrame = -1;
        ctrlCountdown = 0;
        updateControlFrame (0);
        if (params.attack <= 0.0005f)
        {
            // instant attack: start directly on the model's first frame
            attackGain = 1.0f;
            for (int k = 0; k < nP; ++k) amp0[k] = amp1[k];
            for (int b = 0; b < nB; ++b) ng0[b] = ng1[b];
        }

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

        // effective speed: free multiplier, or "scan the envelope in N bars"
        double effSpeed = juce::jmax (0.0f, params.speed);
        if (params.syncSpeed && effSpeed > 0.01)
        {
            static const double bars[] = { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 };
            double best = bars[0];
            for (double b : bars)
                if (std::abs (b - effSpeed) < std::abs (best - effSpeed)) best = b;
            double scanSec = best * 4.0 * 60.0 / juce::jmax (20.0f, params.bpm);
            effSpeed = (field->nFrames / field->controlRate) / scanSec;
        }
        if (float sm = modFor (4); sm != 0.0f)
            effSpeed *= std::exp2 ((double) sm);
        double frameInc = frameIncBase * effSpeed;
        const bool modalOn = params.modal && params.ring > 0.005f;
        const float decayCoef = params.decay > 0.001f
            ? 1.0f - std::exp (-1.0f / (params.decay * (float) sr)) : 1.0f;

        // loop length in frames: musical beats at host tempo (compensated
        // by speed so the audible period stays on the grid), or a fraction
        // of the envelope
        double total = (double) (field->nFrames - 1);
        double loopL = 0.0;
        if (params.loopLen > 0.001f)
        {
            if (params.syncLoop)
            {
                static const double beats[] = { 0.25, 0.5, 1, 2, 4, 8, 16 };
                int bi = juce::jlimit (0, 6, (int) std::lround (params.loopLen * 6.0f));
                double loopSec = beats[bi] * 60.0 / juce::jmax (20.0f, params.bpm);
                loopL = juce::jmin (total, loopSec * field->controlRate * effSpeed);
            }
            else
                loopL = juce::jmax (2.0, (double) params.loopLen * total);
        }
        double ls = juce::jlimit (0.0f, 0.98f,
                          params.loopStart + modFor (11)) * total;

        for (int i = 0; i < num; ++i)
        {
            int frame = (int) framePos;
            if (frame >= field->nFrames - 1)
            {
                if (params.modal && params.ring > 0.005f)
                {
                    // modal mode: hold at the envelope end and let the
                    // ring energy decay; release still ends the voice
                    framePos = (double) (field->nFrames - 1) - 1e-3;
                    frame = (int) framePos;
                }
                else { clearCurrentNote(); return; }
            }
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

            if (effNoise > 0.0f && nB > 0)
            {
                float n = rng.nextFloat() * 2.0f - 1.0f;
                for (int b = 0; b < nB; ++b)
                {
                    float g = ng0[b] + (ng1[b] - ng0[b]) * frac;
                    float y = g * bandFilter[b].processSample (n);
                    sL += effNoise * ngL[b] * y;
                    sR += effNoise * ngR[b] * y;
                }
            }

            // ADSR: attack -> decay to sustain -> release. It shapes the
            // MODEL (the exciter). In modal mode the ring is excited by
            // model x ADSR and then lives on its own: a short ADSR is a
            // short strike, and the body still rings out.
            if (attackGain < 1.0f)
            {
                attackGain += attackStep;
                if (attackGain > 1.0f) attackGain = 1.0f;
            }
            else if (params.decay > 0.001f)
                decayLevel += (params.sustain - decayLevel) * decayCoef;
            else
                decayLevel = 1.0f;
            if (releasing)
            {
                releaseGain -= releaseStep;
                if (releaseGain <= 0.0f)
                {
                    releaseGain = 0.0f;
                    if (! modalOn) { clearCurrentNote(); return; }
                    if (ringMax < 1e-5f) { clearCurrentNote(); return; }
                }
            }
            adsr = attackGain * decayLevel * releaseGain;

            float env = modalOn ? vel : vel * adsr;
            l[i] += sL * env;
            if (r != nullptr) r[i] += sR * env;
            framePos += frameInc;

            // sustain loop: while the key is held, cycle a region of the
            // envelope; release lets it play out past the loop naturally.
            // If start+len runs past the envelope end, the region wraps
            // around through the beginning. Phasor oscillators keep their
            // phase across jumps, and amp targets lerp from the pre-jump
            // values over one control frame, so wraps are click-free.
            if (! releasing && loopL > 2.0)
            {
                if (framePos >= ls) loopArmed = true;
                if (loopArmed)
                {
                    double le = ls + loopL;
                    if (le <= total)
                    {
                        if (framePos >= le)
                            framePos = ls + std::fmod (framePos - le, loopL);
                    }
                    else  // wrapped region: [ls, total) then [0, le - total)
                    {
                        double leW = le - total;
                        if (framePos >= total)
                            framePos -= total;
                        else if (framePos < ls && framePos >= leW)
                            framePos = ls + std::fmod (framePos - leW, loopL);
                    }
                }
            }
        }
    }

private:
    static float wrap01 (float v) noexcept { return v - std::floor (v); }

    float sourceValue (int src) const noexcept
    {
        switch (src)
        {
            case 1: return params.lfo1;                          // -1..1
            case 2: return params.lfo2;                          // -1..1
            case 3: return field != nullptr                      // env pos 0..1
                ? (float) (framePos / (double) juce::jmax (1, field->nFrames - 1))
                : 0.0f;
            case 4: return (float) voiceIndex                    // voice idx 0..1
                / (float) juce::jmax (1, params.spreadN - 1);
            case 5: return noteRandom;                           // 0..1 per note
            case 6: return velocity01;                           // 0..1
            case 7: return ((float) midiNoteNum - 60.0f) / 24.0f; // +-1 per 2 oct
            default: return 0.0f;
        }
    }

    // summed modulation for one destination id
    float modFor (int dst) const noexcept
    {
        float m = 0.0f;
        for (int i = 0; i < Params::nModSlots; ++i)
            if (params.modDst[i] == dst && params.modSrc[i] != 0
                && params.modAmt[i] != 0.0f)
                m += params.modAmt[i] * sourceValue (params.modSrc[i]);
        return m;
    }

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
        float tx = wrap01 (params.cursorX + (float) voiceIndex * params.spreadX
                           + modFor (0));
        float ty = wrap01 (params.cursorY + (float) voiceIndex * params.spreadY
                           + modFor (1));
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
        B += params.stretchB
           + 0.01f * juce::jmax (0.0f, modFor (7));           // Stretch

        // Pitch Env scales the track in semitone space (a linear-ratio
        // scale would go negative on deep dips at high amounts)
        float envSemis = 12.0f * std::log2 (juce::jlimit (0.25f, 4.0f, ratio))
                       * params.pitchEnvAmt;

        // ---- scale quantizer: snap the sounding pitch to the enabled
        // pitch classes (any octave), gliding over `bend` seconds --------
        float soundingMidi = (float) midiNoteNum + envSemis;
        juce::uint16 mask = params.keyMask;
        if (params.midiScale)
        {
            if (mask != 0) lastHeldMask = mask;   // chord released: keep
            else           mask = lastHeldMask;   // quantizing the tail
        }
        float targetMidi = mask != 0
                         ? nearestAllowedMidi (soundingMidi, mask)
                         : soundingMidi;
        if (firstQuant || mask == 0)
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
        float baseFreq = 440.0f * std::exp2 ((quantMidi - 69.0f) / 12.0f
                                             + modFor (10));

        // timbre macro coefficients (constant across partials this frame)
        const float tiltDb = params.tiltDbOct + 12.0f * modFor (2);
        const float tiltCoef = tiltDb * 0.1151293f; // ln(10)/20
        const float oe = juce::jlimit (-1.0f, 1.0f,
                                       params.oddEven + modFor (8));
        const float oddG = juce::jmin (1.0f, 1.0f + oe);
        const float evenG = juce::jmin (1.0f, 1.0f - oe);
        const float blurT = juce::jmax (0.0f, params.blur + 2.0f * modFor (3));
        const float blurAlpha = blurT <= 0.001f ? 1.0f
            : 1.0f - std::exp (-1.0f / (blurT * fld.controlRate));
        const float nPart = juce::jlimit (1.0f, (float) nP,
                                          params.partials + 64.0f * modFor (9));
        effNoise = juce::jmax (0.0f, params.noise * std::exp2 (modFor (5)));

        // stereo width: pan spread grows with harmonic number (fundamental
        // stays centered), micro-detune decorrelates L/R phase over time
        const float widthEff = juce::jlimit (0.0f, 1.0f,
                                             params.width + modFor (6));
        const float widthPan = 0.85f * widthEff;
        const float widthDet = 2.0f * widthEff * 5.7735e-4f; // ~2 cents

        float frameRingMax = 0.0f;
        auto nyq = 0.98f * (float) sr * 0.5f;
        for (int k = 0; k < nP; ++k)
        {
            amp0[k] = amp1[k];

            float kk = (float) (k + 1);
            float pdet = 0.0f;
            for (size_t li = 0; li < nLayers; ++li)
                pdet += wts[li] * fld.layers[li].detune[(size_t) k];
            float freq = baseFreq * kk * std::sqrt (1.0f + B * kk * kk) * pdet;
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
            float a1 = lag[k];
            if (params.modal && params.ring > 0.005f)
            {
                // resonator: the envelope excites, partials ring down at
                // their own rate — higher ones damped faster (physical)
                float Tk = params.ring * std::pow (kk, -1.5f * params.damp);
                float df = std::exp (-1.0f / juce::jmax (1e-4f, Tk)
                                     / fld.controlRate);
                ringAmp[k] = juce::jmax (a1 * adsr, ringAmp[k] * df);
                a1 = ringAmp[k];
                frameRingMax = juce::jmax (frameRingMax, a1);
            }
            amp1[k] = a1;

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
            float n1 = nlag[b];
            if (params.modal && params.ring > 0.005f)
            {
                float Tb = params.ring * std::pow ((float) (b + 1),
                                                   -1.5f * params.damp);
                float df = std::exp (-1.0f / juce::jmax (1e-4f, Tb)
                                     / fld.controlRate);
                ringNoise[b] = juce::jmax (n1 * adsr, ringNoise[b] * df);
                n1 = ringNoise[b];
                frameRingMax = juce::jmax (frameRingMax, n1);
            }
            ng1[b] = n1;

            float pan = 0.7f * params.width * ((b & 1) ? 1.0f : -1.0f);
            ngL[b] = std::sqrt (1.0f - pan);
            ngR[b] = std::sqrt (1.0f + pan);
        }
        ringMax = frameRingMax;
    }

    std::shared_ptr<const MorphField> pendingField, field;
    Params params;
    std::atomic<int>* noteCounter = nullptr;
    int voiceIndex = 0;
    int midiNoteNum = 60;
    juce::uint16 lastHeldMask = 0;
    float velocity01 = 1.0f, noteRandom = 0.0f;
    float quantMidi = 60.0f;
    bool firstQuant = true;
    double sr = 44100.0, framePos = 0.0, frameIncBase = 0.0;
    float f0 = 440.0f, vel = 1.0f;
    int nP = 0, nB = 0, currentFrame = -1, ctrlCountdown = 0;
    bool releasing = false, loopArmed = false;
    float releaseGain = 1.0f, releaseStep = 0.001f;
    float attackGain = 1.0f, attackStep = 1.0f;
    float decayLevel = 1.0f, adsr = 1.0f, ringMax = 0.0f;
    float effNoise = 1.0f;
    float cursorX = 0.5f, cursorY = 0.5f, cursorCoef = 0.1f;

    std::complex<float> zL[maxPartials], wL[maxPartials];
    std::complex<float> zR[maxPartials], wR[maxPartials];
    float amp0[maxPartials] {}, amp1[maxPartials] {}, lag[maxPartials] {};
    float ringAmp[maxPartials] {};
    float gL[maxPartials] {}, gR[maxPartials] {};
    float panSign[maxPartials] {}, detSign[maxPartials] {};
    float log2k[maxPartials] {}, driftPhase[maxPartials] {}, driftInc[maxPartials] {};
    float ng0[maxBands] {}, ng1[maxBands] {}, nlag[maxBands] {};
    float ringNoise[maxBands] {};
    float ngL[maxBands] {}, ngR[maxBands] {};
    juce::dsp::IIR::Filter<float> bandFilter[maxBands];
    juce::Random rng;
};
