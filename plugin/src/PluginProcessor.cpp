#include "PluginProcessor.h"
#include "PluginEditor.h"
#if ADDSYNTH_V2
#include "PluginEditorV2.h"
#endif

static constexpr int kNumVoices = 8;

AddSynthProcessor::AddSynthProcessor()
    : AudioProcessor (BusesProperties()
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread ("AddSynth audio import"),
      apvts (*this, nullptr, "params", makeLayout())
{
    planeSmooth.fill (1.0f);
    for (auto& level : planeDisplay) level.store (1.0f);
    for (int k = 0; k < AddVoice::maxPartials; ++k)
        partialGainValues[(size_t) k] = apvts.getRawParameterValue (partialGainID (k));
    synth.addSound (new AddSound());
    for (int i = 0; i < kNumVoices; ++i)
    {
        auto* v = new AddVoice();
        v->setNoteCounter (&noteCounter);
        v->setMemory (&spectralMemory);
        synth.addVoice (v);
    }
}

AddSynthProcessor::~AddSynthProcessor()
{
    signalThreadShouldExit();
    notify();
    // Analysis checks cancellation between decoding/analysis blocks. Join before
    // destroying state, and cancel callbacks only after the worker has stopped.
    stopThread (-1);
    importUpdater.cancelPendingUpdate();
}

juce::AudioProcessorValueTreeState::ParameterLayout AddSynthProcessor::makeLayout()
{
    using P = juce::AudioParameterFloat;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<P> ("gain", "Gain",
        juce::NormalisableRange<float> (0.0f, 1.5f, 0.001f, 0.5f), 0.5f));
    layout.add (std::make_unique<P> ("noise", "Noise",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f, 0.5f), 1.0f));
    layout.add (std::make_unique<P> ("release", "Release",
        juce::NormalisableRange<float> (0.005f, 2.0f, 0.001f, 0.4f), 0.08f));
    layout.add (std::make_unique<P> ("morphX", "Morph X",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.5f));
    layout.add (std::make_unique<P> ("morphY", "Morph Y",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.5f));

    auto speedRange = juce::NormalisableRange<float> (0.0f, 8.0f, 0.001f);
    speedRange.setSkewForCentre (1.0f);
    layout.add (std::make_unique<P> ("speed", "Speed", speedRange, 1.0f));
    layout.add (std::make_unique<P> ("blur", "Blur",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f, 0.4f), 0.0f));
    layout.add (std::make_unique<P> ("attack", "Attack",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f, 0.4f), 0.0f));
    layout.add (std::make_unique<P> ("tilt", "Tilt",
        juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<P> ("oddeven", "Odd/Even",
        juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<P> ("stretch", "Stretch",
        juce::NormalisableRange<float> (0.0f, 0.02f, 0.00001f, 0.3f), 0.0f));
    layout.add (std::make_unique<P> ("partials", "Partials",
        juce::NormalisableRange<float> (1.0f, 192.0f, 0.1f, 0.5f), 192.0f));
    layout.add (std::make_unique<P> ("drift", "Drift",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<P> ("pitchenv", "Pitch Env",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f), 1.0f));
    layout.add (std::make_unique<P> ("spreadx", "Spread X",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<P> ("spready", "Spread Y",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        "spreadn", "Spread N", 1, 8, 5));

    layout.add (std::make_unique<P> ("bend", "Bend",
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.001f, 0.4f), 0.05f));
    layout.add (std::make_unique<P> ("width", "Width",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        "modal", "Modal", false));
    layout.add (std::make_unique<P> ("ring", "Ring",
        juce::NormalisableRange<float> (0.0f, 8.0f, 0.001f, 0.3f), 0.0f));
    layout.add (std::make_unique<P> ("decay", "Decay",
        juce::NormalisableRange<float> (0.0f, 8.0f, 0.001f, 0.3f), 0.0f));
    layout.add (std::make_unique<P> ("sustain", "Sustain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    layout.add (std::make_unique<P> ("damp", "Damp",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.4f));
    layout.add (std::make_unique<P> ("loopstart", "Loop Start",
        juce::NormalisableRange<float> (0.0f, 0.95f, 0.001f), 0.4f));
    layout.add (std::make_unique<P> ("looplen", "Loop Len",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        "syncloop", "Loop Sync", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        "syncspeed", "Speed Sync", false));

    layout.add (std::make_unique<P> ("lfo1rate", "LFO 1 Rate",
        juce::NormalisableRange<float> (0.02f, 20.0f, 0.001f, 0.3f), 0.5f));
    layout.add (std::make_unique<P> ("lfo2rate", "LFO 2 Rate",
        juce::NormalisableRange<float> (0.02f, 20.0f, 0.001f, 0.3f), 0.13f));
    const juce::StringArray modSources { "off", "lfo 1", "lfo 2", "env pos",
        "voice idx", "random", "velocity", "note" };
    const juce::StringArray modDests { "morph x", "morph y", "tilt", "blur",
        "speed", "time spread", "width", "stretch", "odd/even", "partials",
        "pitch", "loop pos" };
    for (int i = 0; i < AddVoice::Params::nModSlots; ++i)
    {
        auto n = juce::String (i);
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            "mod" + n + "src", "Mod " + n + " Source", modSources, 0));
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            "mod" + n + "dst", "Mod " + n + " Dest", modDests, 0));
        layout.add (std::make_unique<P> ("mod" + n + "amt", "Mod " + n + " Amount",
            juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    }
    layout.add (std::make_unique<juce::AudioParameterBool> (
        "midiscale", "MIDI Scale", false));
    static const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F",
                                         "F#", "G", "G#", "A", "A#", "B" };
    for (int i = 0; i < 12; ++i)
        layout.add (std::make_unique<juce::AudioParameterBool> (
            "key" + juce::String (i),
            juce::String ("Scale ") + noteNames[i], false));
    // Append new parameters and give them a later AU version hint so existing
    // sessions retain their original parameter ordering and automation.
    for (int k = 0; k < AddVoice::maxPartials; ++k)
        layout.add (std::make_unique<P> (
            juce::ParameterID { partialGainID (k), 2 },
            "Partial " + juce::String (k + 1) + " Level",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    auto labFloat = [&] (const char* id, const char* name, float lo, float hi, float initial, float skew = 1.0f)
    {
        layout.add (std::make_unique<P> (juce::ParameterID { id, 3 }, name,
            juce::NormalisableRange<float> (lo, hi, 0.001f, skew), initial));
    };
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "seqon", 3 }, "Sequence", false));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "seqrate", 3 }, "Step Length",
        juce::StringArray { "1/64 note", "1/32 note", "1/16 note", "1/8 note", "1/4 note", "1/2 note", "1 bar" }, 2));
    labFloat ("seqglide", "Step Glide", 0.0f, 1.0f, 0.2f);
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "timeline", 3 }, "Partial Timelines", false));
    labFloat ("timespread", "Time Spread", 0.0f, 1.0f, 0.0f);
    labFloat ("timescatter", "Time Scatter", 0.0f, 1.0f, 0.0f);
    labFloat ("timereverse", "Reverse Partials", 0.0f, 1.0f, 0.0f);
    labFloat ("memory", "Spectral Memory", 0.0f, 1.0f, 0.0f);
    labFloat ("memorytime", "Memory Time", 1.0f, 60.0f, 12.0f, 0.5f);
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "planeson", 4 }, "Spectral Planes", false));
    auto planeFloat = [&] (const char* id, const char* name, float lo, float hi, float initial, float skew = 1.0f)
    {
        layout.add (std::make_unique<P> (juce::ParameterID { id, 4 }, name,
            juce::NormalisableRange<float> (lo, hi, 0.001f, skew), initial));
    };
    planeFloat ("orbithz", "Orbit Speed", 0.0f, 4.0f, 0.12f, 0.35f);
    planeFloat ("orbitshape", "Orbit Shape", 0.0f, 1.0f, 1.0f);
    planeFloat ("planemorph", "Plane Morph", 0.0f, 1.0f, 0.0f);
    planeFloat ("planedepth", "Plane Depth", 0.0f, 1.0f, 0.8f);
    planeFloat ("capturepos", "Plane Capture Position", 0.0f, 1.0f, 0.1f);
    planeFloat ("capturegap", "Plane Capture Gap", 0.01f, 1.0f, 0.35f);
    return layout;
}

void AddSynthProcessor::prepareToPlay (double sampleRate, int)
{
    synth.allNotesOff (0, false);
    synth.setCurrentPlaybackSampleRate (sampleRate * sineLab::SineOutput::factor);
    spectralMemory.prepare (sampleRate * sineLab::SineOutput::factor);
    outputGain.reset (sampleRate * sineLab::SineOutput::factor, 0.02);
    outputGain.setCurrentAndTargetValue (apvts.getRawParameterValue ("gain")->load());
    sineOutput.reset();
    sequenceBeat = 0.0;
    planePhase = 0.0;
    planeSmooth.fill (1.0f);
    highMidi.ensureSize (16384);
    setLatencySamples (sineLab::SineOutput::latency);
    std::fill (std::begin (heldCount), std::end (heldCount), 0);
    heldMaskDisplay.store (0);
}

void AddSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    std::shared_ptr<const MorphField> f;
    {
        juce::SpinLock::ScopedLockType sl (fieldLock);
        f = field;
    }

    AddVoice::Params p;
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    p.noise = 0.0f;
    p.timelineOn = get ("timeline") > 0.5f;
    p.timeSpread = get ("timespread");
    p.timeScatter = get ("timescatter");
    p.timeReverse = get ("timereverse");
    p.memory = get ("memory");
    p.outputRate = getSampleRate() > 0.0 ? getSampleRate() : 44100.0;
    p.release = get ("release");
    p.attack = get ("attack");
    p.speed = get ("speed");
    p.blur = get ("blur");
    p.tiltDbOct = get ("tilt");
    p.oddEven = get ("oddeven");
    p.stretchB = get ("stretch");
    p.partials = get ("partials");
    for (int k = 0; k < AddVoice::maxPartials; ++k)
        p.partialGains[(size_t) k] = partialGainValues[(size_t) k]->load();
    p.drift = get ("drift");
    p.pitchEnvAmt = get ("pitchenv");
    p.cursorX = get ("morphX");
    p.cursorY = get ("morphY");
    p.spreadX = get ("spreadx");
    p.spreadY = get ("spready");
    p.spreadN = (int) get ("spreadn");
    p.bend = get ("bend");
    p.width = get ("width");
    p.modal = get ("modal") > 0.5f;
    p.ring = get ("ring");
    p.damp = get ("damp");
    p.sustain = get ("sustain");
    p.decay = get ("decay");
    p.loopStart = get ("loopstart");
    p.loopLen = get ("looplen");
    p.syncLoop = get ("syncloop") > 0.5f;
    p.syncSpeed = get ("syncspeed") > 0.5f;
    for (int i = 0; i < AddVoice::Params::nModSlots; ++i)
    {
        auto n = juce::String (i);
        p.modSrc[i] = (int) get (("mod" + n + "src").toRawUTF8());
        p.modDst[i] = (int) get (("mod" + n + "dst").toRawUTF8());
        p.modAmt[i] = get (("mod" + n + "amt").toRawUTF8());
    }
    // global LFOs advance per block; voices read the current values
    double srate = getSampleRate() > 0 ? getSampleRate() : 44100.0;
    double dt = buffer.getNumSamples() / srate;
    lfoPhase1 = std::fmod (lfoPhase1 + dt * get ("lfo1rate"), 1.0);
    lfoPhase2 = std::fmod (lfoPhase2 + dt * get ("lfo2rate"), 1.0);
    p.lfo1 = (float) std::sin (lfoPhase1 * juce::MathConstants<double>::twoPi);
    p.lfo2 = (float) std::sin (lfoPhase2 * juce::MathConstants<double>::twoPi);
    p.bpm = 120.0f;
    bool hostClock = false;
    double hostBeat = 0.0;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())
                p.bpm = (float) *bpm;
            if (auto beat = pos->getPpqPosition(); beat && pos->getIsPlaying())
            {
                hostClock = true;
                hostBeat = *beat;
            }
        }
    if (clearMemoryRequested.exchange (false)) spectralMemory.clear();
    // held-note tracking for MIDI scale mode
    for (const auto meta : midi)
    {
        auto m = meta.getMessage();
        if (m.isNoteOn())
            ++heldCount[m.getNoteNumber() % 12];
        else if (m.isNoteOff())
            heldCount[m.getNoteNumber() % 12] =
                juce::jmax (0, heldCount[m.getNoteNumber() % 12] - 1);
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            std::fill (std::begin (heldCount), std::end (heldCount), 0);
            spectralMemory.clear();
        }
    }
    int heldMask = 0;
    for (int i = 0; i < 12; ++i)
        if (heldCount[i] > 0)
            heldMask |= (1 << i);
    heldMaskDisplay.store (heldMask);

    p.midiScale = get ("midiscale") > 0.5f;
    p.keyMask = 0;
    if (p.midiScale)
        p.keyMask = (juce::uint16) heldMask;
    else
        for (int i = 0; i < 12; ++i)
            if (apvts.getRawParameterValue ("key" + juce::String (i))->load() > 0.5f)
                p.keyMask |= (juce::uint16) (1u << i);

    bool anyActive = false;
    for (int i = 0; i < synth.getNumVoices(); ++i)
    {
        auto* voice = synth.getVoice (i);
        anyActive = anyActive || voice->isVoiceActive();
        if (auto* v = dynamic_cast<AddVoice*> (voice))
        {
            v->setField (f);
            v->setParams (p);
        }
    }
    if (! anyActive)
        noteCounter.store (0);  // first note of a phrase is always voice 0

    static constexpr double stepBeats[] = { 0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
    const double stepLength = stepBeats[juce::jlimit (0, 6, (int) get ("seqrate"))];
    const bool sequenceOn = get ("seqon") > 0.5f;
    const float glide = get ("seqglide");
    const float memoryTime = get ("memorytime");
    {
        const juce::SpinLock::ScopedTryLockType lock (planesLock);
        if (lock.isLocked()) audioPlanes = planes;
    }
    const auto orbitPlane = sineLab::SpectralPlane::blend (audioPlanes[0], audioPlanes[1], get ("planemorph"));
    const float planeDepth = get ("planeson") > 0.5f ? get ("planedepth") : 0.0f;
    const float orbitHz = get ("orbithz"), orbitShape = get ("orbitshape");
    outputGain.setTargetValue (get ("gain"));
    if (hostClock) sequenceBeat = hostBeat;
    // A fixed small quantum makes the sequencer independent of DAW block size.
    for (int offset = 0; offset < buffer.getNumSamples();)
    {
        int count = juce::jmin (32, buffer.getNumSamples() - offset);
        // Start memory captures exactly at note-off, even inside a host block.
        for (const auto meta : midi)
            if (meta.samplePosition > offset) count = juce::jmin (count, meta.samplePosition - offset);
        highAudio.clear();
        highMidi.clear();
        for (const auto meta : midi)
            if (meta.samplePosition >= offset && meta.samplePosition < offset + count)
                highMidi.addEvent (meta.getMessage(), (meta.samplePosition - offset) * sineLab::SineOutput::factor);
        const double phase = sequenceBeat / stepLength;
        if (sequenceOn)
        {
            sequence.evaluate (phase, glide, p.sequenceGains);
            sequenceStep.store ((int) (phase - std::floor (phase / sineLab::steps) * sineLab::steps));
        }
        else
        {
            p.sequenceGains.fill (1.0f);
            sequenceStep.store (-1);
        }
        const auto orbitMask = orbitPlane.orbit (planePhase, orbitShape);
        const float follow = 1.0f - std::exp (-(float) count / ((float) srate * 0.02f));
        for (int k = 0; k < sineLab::partials; ++k)
        {
            auto n = (size_t) k;
            const float target = 1.0f + planeDepth * (orbitMask[n] - 1.0f);
            planeSmooth[n] += (target - planeSmooth[n]) * follow;
            p.sequenceGains[n] *= planeSmooth[n];
            planeDisplay[n].store (planeSmooth[n]);
        }
        planePhaseDisplay.store ((float) planePhase);
        planePhase += (double) orbitHz * count / srate;
        planePhase -= std::floor (planePhase);
        for (int i = 0; i < synth.getNumVoices(); ++i)
            if (auto* voice = dynamic_cast<AddVoice*> (synth.getVoice (i))) voice->setParams (p);
        synth.renderNextBlock (highAudio, highMidi, 0, count * sineLab::SineOutput::factor);
        sineLab::Mask memoryMask;
        for (int k = 0; k < AddVoice::maxPartials; ++k)
            memoryMask[(size_t) k] = p.partialGains[(size_t) k] * p.sequenceGains[(size_t) k];
        spectralMemory.render (highAudio, 0, count * sineLab::SineOutput::factor,
                               p.memory, memoryTime, memoryMask);
        for (int sample = 0; sample < count * sineLab::SineOutput::factor; ++sample)
        {
            const float gain = outputGain.getNextValue();
            for (int channel = 0; channel < 2; ++channel)
                highAudio.setSample (channel, sample, highAudio.getSample (channel, sample) * gain);
        }
        sineOutput.process (highAudio, buffer, offset, count);
        sequenceBeat += count / srate * juce::jlimit (20.0f, 400.0f, p.bpm) / 60.0;
        offset += count;
    }
}

void AddSynthProcessor::randomizeParams()
{
    auto& rng = juce::Random::getSystemRandom();
    auto set = [this] (const juce::String& id, float value)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (
                p->getNormalisableRange().convertTo0to1 (value));
            p->endChangeGesture();
        }
    };
    auto uni = [&rng] (float lo, float hi) { return lo + (hi - lo) * rng.nextFloat(); };

    // sound parameters, in musical (not full-range) spans
    set ("speed",    std::exp (uni (std::log (0.25f), std::log (2.0f))));
    set ("attack",   std::pow (rng.nextFloat(), 2.0f) * 0.6f);
    set ("release",  uni (0.05f, 0.9f));
    set ("stretch",  std::pow (rng.nextFloat(), 3.0f) * 0.01f);
    set ("partials", uni (8.0f, 192.0f));
    set ("noise",    0.0f);
    set ("width",    rng.nextFloat());
    set ("spreadx",  rng.nextBool() ? 0.0f : uni (0.05f, 0.5f));
    set ("spready",  rng.nextBool() ? 0.0f : uni (0.05f, 0.5f));
    set ("spreadn",  (float) rng.nextInt ({ 2, 9 }));
    set ("ring",     rng.nextBool() ? 0.0f : uni (0.3f, 4.0f));
    set ("damp",     rng.nextFloat());
    set ("decay",    rng.nextBool() ? 0.0f : uni (0.05f, 2.0f));
    set ("sustain",  rng.nextBool() ? 1.0f : rng.nextFloat());
    set ("loopstart", rng.nextFloat() * 0.9f);
    set ("looplen",  rng.nextBool() ? 0.0f : uni (0.05f, 0.6f));
    set ("lfo1rate", std::exp (uni (std::log (0.05f), std::log (8.0f))));
    set ("lfo2rate", std::exp (uni (std::log (0.05f), std::log (8.0f))));

    // mod matrix: ~60% of slots active with random routing
    for (int i = 0; i < AddVoice::Params::nModSlots; ++i)
    {
        auto n = juce::String (i);
        bool active = rng.nextFloat() < 0.6f;
        set ("mod" + n + "src", active ? (float) rng.nextInt ({ 1, 8 }) : 0.0f);
        set ("mod" + n + "dst", (float) rng.nextInt ({ 0, 12 }));
        set ("mod" + n + "amt", active ? uni (-0.8f, 0.8f) : 0.0f);
    }
    // untouched: gain, morph x/y, tilt, odd/even, blur, drift, pitch env,
    // bend, scale keys, sync toggles, midiscale
}

std::array<sineLab::SpectralPlane, 2> AddSynthProcessor::getPlanes() const
{
    const juce::SpinLock::ScopedLockType lock (planesLock);
    return planes;
}

juce::String AddSynthProcessor::getPlaneName (int index) const
{
    const juce::SpinLock::ScopedLockType lock (planesLock);
    return planeNames[(size_t) juce::jlimit (0, 1, index)];
}

bool AddSynthProcessor::capturePlane (int index, juce::String& message)
{
    if (index < 0 || index > 1) return false;
    std::shared_ptr<const MorphField> source;
    {
        const juce::SpinLock::ScopedLockType lock (fieldLock);
        source = field;
    }
    if (source == nullptr || source->layers.empty())
    {
        message = "add a model before capturing a plane";
        return false;
    }
    const float position = apvts.getRawParameterValue ("capturepos")->load();
    const float gap = apvts.getRawParameterValue ("capturegap")->load();
    const float second = position + gap <= 1.0f ? position + gap : juce::jmax (0.0f, position - gap);
    float weights[MorphField::maxLayers] {};
    source->computeWeights (apvts.getRawParameterValue ("morphX")->load(),
                            apvts.getRawParameterValue ("morphY")->load(), weights);
    auto spectrum = [&] (float at)
    {
        sineLab::Mask result {};
        const float frame = at * (float) (source->nFrames - 1);
        const int i = (int) frame, j = juce::jmin (i + 1, source->nFrames - 1);
        const float fraction = frame - (float) i;
        for (int k = 0; k < source->nPartials; ++k)
        {
            float value = 0.0f;
            for (size_t layer = 0; layer < source->layers.size(); ++layer)
                value += weights[layer] * ((1.0f - fraction) * source->logEnvAt (source->layers[layer], i, k)
                                          + fraction * source->logEnvAt (source->layers[layer], j, k));
            result[(size_t) k] = juce::jmax (0.0f, std::exp (value) - MorphField::EPS)
                               * partialGainValues[(size_t) k]->load();
        }
        return result;
    };
    const auto first = spectrum (position), other = spectrum (second);
    if (sineLab::SpectralPlane::dot (first, first) + sineLab::SpectralPlane::dot (other, other) < 1e-14f)
    {
        message = "silent capture: change position or raise the base bars";
        return false;
    }
    const auto captured = sineLab::SpectralPlane::fromSpectra (first, other);
    message = "captured " + juce::String (juce::roundToInt (100.0f * position)) + "% / "
            + juce::String (juce::roundToInt (100.0f * second)) + "%";
    if (captured.addedCompanion) message += " (companion added)";
    {
        const juce::SpinLock::ScopedLockType lock (planesLock);
        planes[(size_t) index] = captured;
        planeNames[(size_t) index] = message;
    }
    sequenceEdited();
    return true;
}

void AddSynthProcessor::setPlanesPreset()
{
    setLabPreset (true);
    {
        const juce::SpinLock::ScopedLockType lock (planesLock);
        planes = sineLab::defaultPlanes();
        planeNames = { "harmonic ribbons", "spectral ripples" };
    }
    for (const auto& item : std::initializer_list<std::pair<const char*, float>> {
            { "seqon", 0.0f }, { "planeson", 1.0f }, { "orbithz", 0.18f },
            { "orbitshape", 0.75f }, { "planemorph", 0.35f }, { "planedepth", 0.9f } })
    {
        auto* parameter = apvts.getParameter (item.first);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (item.second));
        parameter->endChangeGesture();
    }
    sequenceEdited();
}

void AddSynthProcessor::setLabPreset (bool ambient)
{
    auto set = [this] (const juce::String& id, float value)
    {
        auto* parameter = apvts.getParameter (id);
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        parameter->endChangeGesture();
    };
    for (int step = 0; step < sineLab::steps; ++step)
        for (int k = 0; k < AddVoice::maxPartials; ++k)
        {
            float value;
            if (ambient)
            {
                const float phase = (float) k * 0.095f - (float) step * juce::MathConstants<float>::twoPi / 8.0f;
                value = 0.12f + 0.8f * std::pow (0.5f + 0.5f * std::cos (phase), 2.0f);
            }
            else
                value = (k / 3 + step * 3) % 8 < 2 ? 0.95f : 0.025f;
            sequence.set (step, k, k == 0 ? 0.65f : value);
        }
    for (int k = 0; k < AddVoice::maxPartials; ++k) set (partialGainID (k), 1.0f);
    set ("seqon", 1.0f); set ("seqrate", ambient ? 6.0f : 0.0f);
    set ("planeson", 0.0f);
    set ("seqglide", ambient ? 1.0f : 0.12f);
    set ("timeline", 1.0f);
    set ("timespread", ambient ? 0.85f : 0.75f);
    set ("timescatter", ambient ? 0.7f : 0.4f);
    set ("timereverse", ambient ? 0.6f : 0.55f);
    set ("memory", ambient ? 0.65f : 0.3f);
    set ("memorytime", ambient ? 30.0f : 5.0f);
    set ("speed", ambient ? 0.2f : 0.7f);
    set ("blur", ambient ? 0.18f : 0.006f);
    set ("attack", ambient ? 0.8f : 0.002f);
    set ("release", ambient ? 1.5f : 0.1f);
    set ("width", ambient ? 0.9f : 0.7f);
    set ("modal", 0.0f); set ("noise", 0.0f);
    set ("partials", 192.0f); set ("tilt", 0.0f); set ("oddeven", 0.0f);
    set ("stretch", 0.0f); set ("pitchenv", 0.5f);
    set ("decay", 0.0f); set ("sustain", 1.0f); set ("looplen", 0.0f);
    set ("syncloop", 0.0f); set ("syncspeed", 0.0f);
    for (int i = 0; i < AddVoice::Params::nModSlots; ++i)
        set ("mod" + juce::String (i) + "src", 0.0f);
    clearMemory();
    sequenceEdited();
}

std::vector<std::pair<float, float>> AddSynthProcessor::getActiveVoiceCursors() const
{
    std::vector<std::pair<float, float>> out;
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<const AddVoice*> (synth.getVoice (i)))
        {
            float x, y;
            if (v->getDisplayCursor (x, y))
                out.emplace_back (x, y);
        }
    return out;
}

// ---- morph pad management (message thread) --------------------------------

void AddSynthProcessor::rebuildField()
{
    const juce::ScopedLock lock (slotsLock);
    std::vector<MorphField::Placement> placements;
    for (auto& s : slots)
        if (s.model != nullptr)
            placements.push_back ({ s.model, s.x, s.y, s.playback, s.shape });
    auto newField = MorphField::build (placements);

    // notes already playing keep their old snapshot; new notes get this one
    juce::SpinLock::ScopedLockType sl (fieldLock);
    field = newField;
}

bool AddSynthProcessor::addModel (const juce::File& f, float x, float y,
                                  juce::String& error)
{
    auto model = AddmModel::load (f, error);
    return model != nullptr && addPreparedModel (f, std::move (model), nullptr, x, y, error);
}

bool AddSynthProcessor::addPreparedModel (const juce::File& file,
    std::shared_ptr<const AddmModel> model, std::shared_ptr<const juce::MemoryBlock> embedded,
    float x, float y, juce::String& error, bool rebuild)
{
    const juce::ScopedLock lock (slotsLock);
    if ((int) slots.size() >= MorphField::maxLayers)
    {
        error = "pad is full";
        return false;
    }
    slots.push_back ({ file.getFullPathName(), model->name,
                       juce::jlimit (0.0f, 1.0f, x),
                       juce::jlimit (0.0f, 1.0f, y), std::move (model), {}, std::move (embedded) });
    lastModelDir = file.getParentDirectory();
    selectSlot ((int) slots.size() - 1);
    if (rebuild)
    {
        rebuildField();
        sequenceEdited();
    }
    return true;
}

void AddSynthProcessor::removeSlot (int index)
{
    const juce::ScopedLock lock (slotsLock);
    if (index < 0 || index >= (int) slots.size()) return;
    slots.erase (slots.begin() + index);
    if (selectedSlot > index) --selectedSlot;
    else if (selectedSlot == index) selectedSlot = juce::jmin (index, (int) slots.size() - 1);
    rebuildField();
    sequenceEdited();
}

void AddSynthProcessor::moveSlot (int index, float x, float y)
{
    const juce::ScopedLock lock (slotsLock);
    if (index < 0 || index >= (int) slots.size()) return;
    slots[(size_t) index].x = juce::jlimit (0.0f, 1.0f, x);
    slots[(size_t) index].y = juce::jlimit (0.0f, 1.0f, y);
    rebuildField();
    sequenceEdited();
}

void AddSynthProcessor::setSlotPlayback (int index, MorphField::Playback playback)
{
    const juce::ScopedLock lock (slotsLock);
    if (! juce::isPositiveAndBelow (index, (int) slots.size())) return;
    playback.sanitise();
    slots[(size_t) index].playback = playback;
    rebuildField();
    sequenceEdited();
}

void AddSynthProcessor::setSlotShape (int index, MorphField::Shape shape)
{
    const juce::ScopedLock lock (slotsLock);
    if (! juce::isPositiveAndBelow (index, (int) slots.size())) return;
    shape.sanitise();
    slots[(size_t) index].shape = shape;
    rebuildField();
    sequenceEdited();
}

void AddSynthProcessor::selectSlot (int index)
{
    const juce::ScopedLock lock (slotsLock);
    selectedSlot.store (juce::isPositiveAndBelow (index, (int) slots.size()) ? index : -1);
}

std::vector<AddSynthProcessor::Slot> AddSynthProcessor::getSlots() const
{
    const juce::ScopedLock lock (slotsLock);
    return slots;
}

juce::File AddSynthProcessor::getLastModelDir() const
{
    const juce::ScopedLock lock (slotsLock);
    return lastModelDir;
}

bool AddSynthProcessor::canImportFile (const juce::File& file)
{
    return file.hasFileExtension ("addm") || audioImport::supports (file);
}

juce::String AddSynthProcessor::importWildcards()
{
    return "*.addm;" + audioImport::supportedWildcards();
}

AddSynthProcessor::ImportStatus AddSynthProcessor::getImportStatus() const
{
    const juce::ScopedLock lock (importLock);
    return importStatus;
}

void AddSynthProcessor::cancelImports()
{
    const juce::ScopedLock lock (importLock);
    ++importGeneration;
    importQueue.clear();
    importCompletions.clear();
    pendingImports = 0;
    importStatus = { "Import cancelled", 0.0f, false };
    notify();
}

void AddSynthProcessor::importFiles (const juce::StringArray& files, juce::Point<float> position)
{
    if (files.isEmpty()) return;
    const juce::ScopedLock slotsGuard (slotsLock);
    const juce::ScopedLock importGuard (importLock);
    if (pendingImports == 0)
    {
        importedCount = 0;
        importErrors.clear();
        importNotes.clear();
    }
    int added = 0;
    for (const auto& path : files)
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
        if (! canImportFile (file))
        {
            importErrors.add (file.getFileName() + ": unsupported file type");
            continue;
        }
        if ((int) slots.size() + pendingImports >= MorphField::maxLayers)
        {
            importErrors.add ("Pad is full (16 models). Remove a model to add more.");
            break;
        }
        juce::Point<float> at;
        if (position.x >= 0.0f && position.y >= 0.0f)
            at = { juce::jlimit (0.0f, 1.0f, position.x + 0.055f * (float) added),
                   juce::jlimit (0.0f, 1.0f, position.y - 0.055f * (float) added) };
        else
        {
            const int index = (int) slots.size() + pendingImports;
            at = { 0.15f + 0.23f * (float) (index % 4), 0.85f - 0.23f * (float) (index / 4) };
        }
        importQueue.push_back ({ file, at, importGeneration.load() });
        ++pendingImports;
        ++added;
    }
    if (pendingImports == 0)
    {
        importStatus = { importErrors.joinIntoString ("\n"), 0.0f, false };
        return;
    }
    importStatus = { "Queued " + juce::String (pendingImports) + " sound(s)", 0.0f, true };
    if (! isThreadRunning() && ! startThread())
    {
        importQueue.clear();
        pendingImports = 0;
        importStatus = { "Could not start audio import. Please try again.", 0.0f, false };
    }
    notify();
}

void AddSynthProcessor::run()
{
    while (! threadShouldExit())
    {
        ImportRequest request;
        bool haveRequest = false;
        {
            const juce::ScopedLock lock (importLock);
            if (! importQueue.empty())
            {
                request = std::move (importQueue.front());
                importQueue.pop_front();
                haveRequest = true;
                importStatus.message = "Importing " + request.file.getFileName();
                importStatus.progress = 0.0f;
            }
        }
        if (! haveRequest) { wait (250); continue; }
        ImportCompletion completion { request, nullptr, nullptr, {}, {} };
        try
        {
            if (request.file.hasFileExtension ("addm"))
                completion.model = AddmModel::load (request.file, completion.error);
            else
            {
                auto result = audioImport::analyze (request.file, [this, generation = request.generation] (float progress)
                {
                    if (threadShouldExit() || generation != importGeneration.load()) return false;
                    const juce::ScopedLock lock (importLock);
                    if (generation != importGeneration.load()) return false;
                    importStatus.progress = progress;
                    return true;
                });
                completion.model = std::move (result.model);
                completion.error = result.error;
                completion.note = result.note;
                if (completion.model != nullptr)
                {
                    completion.embeddedModel = std::make_shared<juce::MemoryBlock>();
                    juce::MemoryOutputStream output (*completion.embeddedModel, false);
                    if (! completion.model->write (output))
                    {
                        completion.model.reset();
                        completion.error = "Could not store the converted model";
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            completion.model.reset();
            completion.error = "Could not analyze this sound";
        }
        bool ready = false;
        {
            const juce::ScopedLock lock (importLock);
            if (! threadShouldExit() && request.generation == importGeneration.load())
            {
                importCompletions.push_back (std::move (completion));
                ready = true;
            }
        }
        // LockingAsyncUpdater waits for an active callback. Never trigger it
        // while holding a lock that the message-thread callback acquires.
        if (ready) importUpdater.triggerAsyncUpdate();
    }
}

void AddSynthProcessor::handleAsyncUpdate()
{
    const juce::ScopedLock slotsGuard (slotsLock);
    const juce::ScopedLock importGuard (importLock);
    if (importCompletions.empty()) return;
    bool changed = false;
    for (auto& completion : importCompletions)
    {
        if (completion.request.generation != importGeneration.load()) continue;
        bool added = false;
        if (completion.model != nullptr)
            added = addPreparedModel (completion.request.file, std::move (completion.model),
                std::move (completion.embeddedModel), completion.request.position.x,
                completion.request.position.y, completion.error, false);
        if (added)
        {
            ++importedCount;
            changed = true;
            if (completion.note.isNotEmpty()) importNotes.add (completion.request.file.getFileName() + ": " + completion.note);
        }
        else importErrors.add (completion.request.file.getFileName() + ": " + completion.error);
        --pendingImports;
    }
    importCompletions.clear();
    if (changed)
    {
        rebuildField();
        sequenceEdited();
    }
    if (pendingImports == 0)
    {
        juce::String message = "Imported " + juce::String (importedCount) + " sound(s)";
        if (! importErrors.isEmpty()) message += "\n" + importErrors.joinIntoString ("\n");
        if (! importNotes.isEmpty()) message += "\n" + importNotes.joinIntoString ("\n");
        importStatus = { message, 1.0f, false };
    }
}

// ---- state -----------------------------------------------------------------

void AddSynthProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    const juce::ScopedLock lock (slotsLock);
    juce::ValueTree root ("AddSynthState");
    root.setProperty ("selectedSlot", selectedSlot.load(), nullptr);
    root.appendChild (apvts.copyState(), nullptr);
    juce::ValueTree st ("slots");
    for (auto& s : slots)
    {
        juce::ValueTree v ("slot");
        v.setProperty ("path", s.path, nullptr);
        v.setProperty ("x", s.x, nullptr);
        v.setProperty ("y", s.y, nullptr);
        v.setProperty ("start", s.playback.start, nullptr);
        v.setProperty ("loopIn", s.playback.loopIn, nullptr);
        v.setProperty ("loopOut", s.playback.loopOut, nullptr);
        v.setProperty ("loop", s.playback.loop, nullptr);
        if (! s.shape.isNeutral())
        {
            juce::Array<juce::var> envelope, partialLevels;
            for (float value : s.shape.envelope) envelope.add (value);
            for (float value : s.shape.partials) partialLevels.add (value);
            v.setProperty ("envelope", juce::var (envelope), nullptr);
            v.setProperty ("partialLevels", juce::var (partialLevels), nullptr);
        }
        if (s.embeddedModel != nullptr)
            v.setProperty ("modelData", juce::var (*s.embeddedModel), nullptr);
        st.appendChild (v, nullptr);
    }
    root.appendChild (st, nullptr);
    juce::ValueTree patterns ("sequence");
    for (int step = 0; step < sineLab::steps; ++step)
    {
        juce::ValueTree row ("step");
        juce::Array<juce::var> levels;
        for (int k = 0; k < AddVoice::maxPartials; ++k) levels.add (sequence.get (step, k));
        row.setProperty ("levels", juce::var (levels), nullptr);
        patterns.appendChild (row, nullptr);
    }
    root.appendChild (patterns, nullptr);
    juce::ValueTree planeTree ("spectralPlanes");
    {
        const juce::SpinLock::ScopedLockType lock (planesLock);
        for (int index = 0; index < 2; ++index)
        {
            juce::ValueTree plane ("plane");
            plane.setProperty ("name", planeNames[(size_t) index], nullptr);
            for (int axis = 0; axis < 2; ++axis)
            {
                juce::Array<juce::var> values;
                const auto& basis = axis == 0 ? planes[(size_t) index].u : planes[(size_t) index].v;
                for (float value : basis) values.add (value);
                plane.setProperty (axis == 0 ? "u" : "v", juce::var (values), nullptr);
            }
            planeTree.appendChild (plane, nullptr);
        }
    }
    root.appendChild (planeTree, nullptr);
    juce::MemoryOutputStream out (dest, false);
    root.writeToStream (out);
}

void AddSynthProcessor::setStateInformation (const void* data, int size)
{
    auto root = juce::ValueTree::readFromData (data, (size_t) size);
    if (! root.isValid())
        return;
    if (! root.hasType ("AddSynthState") && ! root.hasType ("params")) return;
    const juce::ScopedLock slotsGuard (slotsLock);
    cancelImports();
    {
        const juce::ScopedLock importGuard (importLock);
        importStatus = {};
    }
    {
        auto restored = sineLab::defaultPlanes();
        std::array<juce::String, 2> names { "harmonic ribbons", "spectral ripples" };
        const auto tree = root.getChildWithName ("spectralPlanes");
        for (int index = 0; index < juce::jmin (2, tree.getNumChildren()); ++index)
        {
            const auto child = tree.getChild (index);
            const auto u = child.getProperty ("u"), v = child.getProperty ("v");
            auto* first = u.getArray(); auto* second = v.getArray();
            if (first == nullptr || second == nullptr || first->size() != sineLab::partials
                || second->size() != sineLab::partials) continue;
            sineLab::Mask a, b;
            for (int k = 0; k < sineLab::partials; ++k)
            {
                a[(size_t) k] = (float) (double) first->getReference (k);
                b[(size_t) k] = (float) (double) second->getReference (k);
            }
            restored[(size_t) index] = sineLab::SpectralPlane::fromSpectra (a, b);
            names[(size_t) index] = child.getProperty ("name", "captured plane").toString();
        }
        const juce::SpinLock::ScopedLockType lock (planesLock);
        planes = restored;
        planeNames = names;
    }
    sequence.reset();
    auto patterns = root.getChildWithName ("sequence");
    for (int step = 0; step < juce::jmin (patterns.getNumChildren(), sineLab::steps); ++step)
    {
        auto levels = patterns.getChild (step).getProperty ("levels");
        if (auto* array = levels.getArray())
            for (int k = 0; k < juce::jmin (array->size(), AddVoice::maxPartials); ++k)
                sequence.set (step, k, (float) (double) array->getReference (k));
    }
    clearMemory();

    juce::ValueTree params, slotTree;
    if (root.hasType ("AddSynthState"))
    {
        params = root.getChildWithName ("params");
        slotTree = root.getChildWithName ("slots");
    }
    else if (root.hasType ("params"))  // legacy single-model state
    {
        params = root;
    }
    if (params.isValid())
        apvts.replaceState (params);

    slots.clear();
    selectedSlot = -1;
    if (slotTree.isValid())
    {
        for (auto v : slotTree)
        {
            juce::File f (v.getProperty ("path").toString());
            juce::String err;
            bool added = false;
            const auto embedded = v.getProperty ("modelData");
            const float x = (float) v.getProperty ("x", 0.5f), y = (float) v.getProperty ("y", 0.5f);
            if (const auto* bytes = embedded.getBinaryData())
            {
                juce::MemoryInputStream input (*bytes, false);
                auto model = AddmModel::read (input, err);
                if (model != nullptr)
                    added = addPreparedModel (f, std::move (model),
                        std::make_shared<juce::MemoryBlock> (*bytes), x, y, err);
            }
            else added = addModel (f, x, y, err);
            if (added)
            {
                auto& playback = slots.back().playback;
                playback.start = (float) v.getProperty ("start", 0.0f);
                playback.loopIn = (float) v.getProperty ("loopIn", 0.0f);
                playback.loopOut = (float) v.getProperty ("loopOut", 1.0f);
                playback.loop = (bool) v.getProperty ("loop", false);
                playback.sanitise();
                auto& shape = slots.back().shape;
                const auto restore = [&v] (const char* name, float* values, int count)
                {
                    if (const auto* array = v.getProperty (name).getArray())
                        for (int i = 0; i < juce::jmin (count, array->size()); ++i)
                            values[i] = (float) (double) array->getReference (i);
                };
                restore ("envelope", shape.envelope.data(), (int) shape.envelope.size());
                restore ("partialLevels", shape.partials.data(), (int) shape.partials.size());
                shape.sanitise();
            }
        }
    }
    else if (params.hasProperty ("modelPath"))  // legacy
    {
        juce::String err;
        addModel (juce::File (params.getProperty ("modelPath").toString()),
                  0.5f, 0.5f, err);
    }
    selectSlot ((int) root.getProperty ("selectedSlot", slots.empty() ? -1 : 0));
    rebuildField();
}

juce::AudioProcessorEditor* AddSynthProcessor::createEditor()
{
   #if ADDSYNTH_V2
    return new AddSynthEditorV2 (*this);
   #else
    return new AddSynthEditor (*this);
   #endif
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AddSynthProcessor();
}
