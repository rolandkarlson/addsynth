#include "PluginProcessor.h"
#include "PluginEditor.h"

static constexpr int kNumVoices = 8;

AddSynthProcessor::AddSynthProcessor()
    : AudioProcessor (BusesProperties()
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "params", makeLayout())
{
    synth.addSound (new AddSound());
    for (int i = 0; i < kNumVoices; ++i)
    {
        auto* v = new AddVoice();
        v->setNoteCounter (&noteCounter);
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
        "speed", "noise", "width", "stretch", "odd/even", "partials",
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
    return layout;
}

void AddSynthProcessor::prepareToPlay (double sampleRate, int)
{
    synth.setCurrentPlaybackSampleRate (sampleRate);
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
    p.noise = get ("noise");
    p.release = get ("release");
    p.attack = get ("attack");
    p.speed = get ("speed");
    p.blur = get ("blur");
    p.tiltDbOct = get ("tilt");
    p.oddEven = get ("oddeven");
    p.stretchB = get ("stretch");
    p.partials = get ("partials");
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
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
                p.bpm = (float) *bpm;
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
            std::fill (std::begin (heldCount), std::end (heldCount), 0);
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

    synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());

    buffer.applyGain (apvts.getRawParameterValue ("gain")->load());
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
    set ("noise",    uni (0.2f, 1.5f));
    set ("width",    rng.nextFloat());
    set ("spreadx",  rng.nextBool() ? 0.0f : uni (0.05f, 0.5f));
    set ("spready",  rng.nextBool() ? 0.0f : uni (0.05f, 0.5f));
    set ("spreadn",  (float) rng.nextInt ({ 2, 9 }));
    set ("ring",     rng.nextBool() ? 0.0f : uni (0.3f, 4.0f));
    set ("damp",     rng.nextFloat());
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
    juce::MemoryOutputStream out (dest, false);
    root.writeToStream (out);
}

void AddSynthProcessor::setStateInformation (const void* data, int size)
{
    auto root = juce::ValueTree::readFromData (data, (size_t) size);
    if (! root.isValid())
        return;

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
