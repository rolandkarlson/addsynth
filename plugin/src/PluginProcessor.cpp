#include "PluginProcessor.h"
#include "PluginEditor.h"

static constexpr int kNumVoices = 8;

AddSynthProcessor::AddSynthProcessor()
    : AudioProcessor (BusesProperties()
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "params", makeLayout())
{
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
    std::vector<MorphField::Placement> placements;
    for (auto& s : slots)
        if (s.model != nullptr)
            placements.push_back ({ s.model, s.x, s.y });
    auto newField = MorphField::build (placements);

    // notes already playing keep their old snapshot; new notes get this one
    juce::SpinLock::ScopedLockType sl (fieldLock);
    field = newField;
}

bool AddSynthProcessor::addModel (const juce::File& f, float x, float y,
                                  juce::String& error)
{
    if ((int) slots.size() >= MorphField::maxLayers)
    {
        error = "pad is full";
        return false;
    }
    auto m = AddmModel::load (f, error);
    if (m == nullptr)
        return false;
    slots.push_back ({ f.getFullPathName(), m->name,
                       juce::jlimit (0.0f, 1.0f, x),
                       juce::jlimit (0.0f, 1.0f, y), m });
    lastModelDir = f.getParentDirectory();
    rebuildField();
    return true;
}

void AddSynthProcessor::removeSlot (int index)
{
    if (index < 0 || index >= (int) slots.size()) return;
    slots.erase (slots.begin() + index);
    rebuildField();
}

void AddSynthProcessor::moveSlot (int index, float x, float y)
{
    if (index < 0 || index >= (int) slots.size()) return;
    slots[(size_t) index].x = juce::jlimit (0.0f, 1.0f, x);
    slots[(size_t) index].y = juce::jlimit (0.0f, 1.0f, y);
    rebuildField();
}

// ---- state -----------------------------------------------------------------

void AddSynthProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree root ("AddSynthState");
    root.appendChild (apvts.copyState(), nullptr);
    juce::ValueTree st ("slots");
    for (auto& s : slots)
    {
        juce::ValueTree v ("slot");
        v.setProperty ("path", s.path, nullptr);
        v.setProperty ("x", s.x, nullptr);
        v.setProperty ("y", s.y, nullptr);
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
    juce::MemoryOutputStream out (dest, false);
    root.writeToStream (out);
}

void AddSynthProcessor::setStateInformation (const void* data, int size)
{
    auto root = juce::ValueTree::readFromData (data, (size_t) size);
    if (! root.isValid())
        return;
    if (! root.hasType ("AddSynthState") && ! root.hasType ("params")) return;
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
    if (slotTree.isValid())
    {
        for (auto v : slotTree)
        {
            juce::File f (v.getProperty ("path").toString());
            juce::String err;
            addModel (f, (float) v.getProperty ("x", 0.5f),
                         (float) v.getProperty ("y", 0.5f), err);
        }
    }
    else if (params.hasProperty ("modelPath"))  // legacy
    {
        juce::String err;
        addModel (juce::File (params.getProperty ("modelPath").toString()),
                  0.5f, 0.5f, err);
    }
    rebuildField();
}

juce::AudioProcessorEditor* AddSynthProcessor::createEditor()
{
    return new AddSynthEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AddSynthProcessor();
}
