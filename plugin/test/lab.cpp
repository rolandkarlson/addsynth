#include "../src/PluginEditor.h"
#include <iostream>
#include <stdexcept>

static void check (bool condition, const char* message)
{
    if (! condition) throw std::runtime_error (message);
}

static void set (AddSynthProcessor& processor, const juce::String& id, float value)
{
    auto* parameter = processor.apvts.getParameter (id);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

static void testSequenceStateAndEditing()
{
    AddSynthProcessor processor;
    processor.setLabPreset (false);
    processor.sequence.set (7, 191, 0.37f);
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    AddSynthProcessor restored;
    restored.setStateInformation (saved.getData(), (int) saved.getSize());
    for (int step = 0; step < 8; ++step)
        for (int k = 0; k < 192; ++k)
            check (std::abs (processor.sequence.get (step, k) - restored.sequence.get (step, k)) < 1e-6f,
                   "sequence masks lost during state recall");
    check (restored.apvts.getRawParameterValue ("timeline")->load() == 1.0f, "lab controls not recalled");

    auto state = juce::ValueTree::readFromData (saved.getData(), saved.getSize());
    state.removeChild (state.getChildWithName ("sequence"), nullptr);
    auto legacyParams = state.getChildWithName ("params");
    for (int i = legacyParams.getNumChildren(); --i >= 0;)
    {
        auto* parameter = processor.apvts.getParameter (legacyParams.getChild (i).getProperty ("id").toString());
        if (parameter != nullptr && parameter->getVersionHint() == 3) legacyParams.removeChild (i, nullptr);
    }
    juce::MemoryBlock oldState;
    juce::MemoryOutputStream stream (oldState, false);
    state.writeToStream (stream);
    restored.setStateInformation (oldState.getData(), (int) oldState.getSize());
    check (restored.sequence.get (7, 191) == 1.0f, "legacy states must restore flat steps");
    check (restored.apvts.getRawParameterValue ("seqon")->load() == 0.0f
        && restored.apvts.getRawParameterValue ("timeline")->load() == 0.0f
        && restored.apvts.getRawParameterValue ("memory")->load() == 0.0f,
        "legacy sessions must disable new experiments");

    PartialEditor bars (processor);
    bars.setSize (872, 140);
    bars.selectStep (7);
    auto event = juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
        { 858.0f, 75.0f }, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
        1.0f, 0, 0, 0, 0, &bars, &bars, juce::Time(), { 858.0f, 75.0f }, juce::Time(), 1, false);
    bars.mouseDown (event);
    bars.mouseUp (event);
    check (std::abs (processor.sequence.get (7, 191) - 0.5f) < 0.001f, "drawing must address selected step");
    check (processor.apvts.getRawParameterValue ("partialGain192")->load() == 1.0f, "step drawing changed base mask");

    sineLab::Sequence sequence;
    sineLab::Mask mask;
    sequence.set (0, 0, 0.0f);
    sequence.evaluate (0.0, 1.0f, mask);
    check (mask[0] == 1.0f, "glide must start from previous step");
    sequence.evaluate (0.5, 1.0f, mask);
    check (std::abs (mask[0] - 0.5f) < 1e-5f, "glide midpoint is incorrect");
    sequence.evaluate (-7.5, 1.0f, mask);
    check (std::abs (mask[0] - 0.5f) < 1e-5f, "negative host pre-roll must wrap safely");
    sequence.evaluate (0.0, 0.0f, mask);
    check (mask[0] == 0.0f, "zero glide must select current step");
    check (sineLab::reflectedFrame (-3, 10) == 3 && sineLab::reflectedFrame (13, 10) == 7,
           "reverse playheads must reflect at model edges");
}

static void testOutputFilter()
{
    constexpr int samples = 4096, factor = sineLab::SineOutput::factor;
    for (double sampleRate : { 32000.0, 44100.0, 48000.0, 96000.0 })
        for (double fraction : { 0.05, 0.51, 0.65, 1.15 })
        {
            sineLab::SineOutput filter;
            juce::AudioBuffer<float> high (2, samples * factor), output (2, samples);
            for (int i = 0; i < samples * factor; ++i)
            {
                const float x = (float) std::sin (juce::MathConstants<double>::twoPi
                    * (sampleRate * fraction) * i / (sampleRate * factor));
                high.setSample (0, i, x); high.setSample (1, i, x);
            }
            filter.process (high, output, 0, samples);
            const float rms = output.getRMSLevel (0, 512, samples - 512);
            if (fraction < 0.4)
                check (rms > 0.70f && rms < 0.72f, "decimator damaged passband sines");
            else
                check (rms < 1e-5f, "out-of-band modulation was not rejected below -97 dB relative to sine");
        }
    sineLab::SineOutput filter;
    juce::AudioBuffer<float> impulse (2, 1024), output (2, 256);
    impulse.clear();
    impulse.setSample (0, 0, 1.0f);
    filter.process (impulse, output, 0, 256);
    int peak = 0;
    for (int i = 1; i < 256; ++i)
        if (std::abs (output.getSample (0, i)) > std::abs (output.getSample (0, peak))) peak = i;
    check (peak == sineLab::SineOutput::latency, "reported filter latency differs from actual latency");
    for (double rate : { 32000.0, 44100.0, 48000.0, 96000.0 })
    {
        check (sineLab::bandGain ((float) rate * 0.39f, rate) == 1.0f, "passband guard must be transparent");
        check (sineLab::bandGain ((float) rate * 0.46f, rate) == 0.0f, "carrier guard must reject high partials");
    }
    std::cout << "PASS: output stopband < -97 dB at 32/44.1/48/96 kHz; latency 64 samples\n";
}

struct Fixture
{
    juce::TemporaryFile file;
    Fixture()
    {
        juce::DynamicObject::Ptr header = new juce::DynamicObject();
        header->setProperty ("name", "sine-lab-test");
        header->setProperty ("n_frames", 172);
        header->setProperty ("n_partials", 192);
        header->setProperty ("n_noise_bands", 0);
        header->setProperty ("control_rate", 172.0);
        header->setProperty ("f0_ref", 55.0);
        auto json = juce::JSON::toString (juce::var (header.get()), true);
        juce::FileOutputStream out (file.getFile());
        out.write ("ADDM", 4); out.writeInt (2);
        out.writeInt ((int) json.getNumBytesAsUTF8());
        out.write (json.toRawUTF8(), json.getNumBytesAsUTF8());
        for (int frame = 0; frame < 172; ++frame)
            for (int k = 0; k < 192; ++k)
            {
                float level = 0.07f / (float) (k + 1);
                if (k > 0) level *= 0.1f + 0.9f * (float) frame / 171.0f;
                out.writeFloat (level);
            }
        for (int frame = 0; frame < 172; ++frame) out.writeFloat (1.0f);
        for (int k = 0; k < 192; ++k) out.writeFloat (1.0f);
    }
    void load (AddSynthProcessor& processor, double sampleRate = 44100.0)
    {
        juce::String error;
        check (processor.addModel (file.getFile(), 0.5f, 0.5f, error), "failed to load lab test model");
        processor.setRateAndBufferSizeDetails (sampleRate, 512);
        processor.prepareToPlay (sampleRate, 512);
    }
};

static juce::AudioBuffer<float> render (AddSynthProcessor& processor, int samples,
                                       bool noteOn = false, bool noteOff = false, int note = 45)
{
    juce::AudioBuffer<float> result (2, samples);
    for (int offset = 0; offset < samples; offset += 256)
    {
        const int count = juce::jmin (256, samples - offset);
        juce::AudioBuffer<float> block (result.getArrayOfWritePointers(), 2, offset, count);
        juce::MidiBuffer midi;
        if (offset == 0 && noteOn) midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
        if (offset == 0 && noteOff) midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
        processor.processBlock (block, midi);
    }
    return result;
}

static void testProcessorAudio()
{
    Fixture fixture;
    AddSynthProcessor memory;
    fixture.load (memory);
    set (memory, "speed", 0.0f);
    set (memory, "memory", 1.0f); set (memory, "memorytime", 1.0f);
    set (memory, "release", 0.005f);
    render (memory, 4096, true);
    auto tail = render (memory, 4410, false, true);
    check (tail.getRMSLevel (0, 2205, 2205) > 0.0001f, "released note left no spectral memory");
    memory.clearMemory();
    render (memory, 4096);
    check (render (memory, 4096).getMagnitude (0, 4096) < 1e-7f, "clear memory must fade to silence");

    AddSynthProcessor plain, timelines;
    fixture.load (plain); fixture.load (timelines);
    set (plain, "looplen", 1.0f);
    set (timelines, "timeline", 1.0f);
    set (timelines, "timespread", 1.0f); set (timelines, "timescatter", 1.0f);
    set (timelines, "timereverse", 0.7f);
    auto a = render (plain, 12000, true);
    auto b = render (timelines, 12000, true);
    double difference = 0;
    for (int i = 0; i < 12000; ++i) difference += std::abs (a.getSample (0, i) - b.getSample (0, i));
    check (difference / 12000 > 0.001, "independent timelines had no audible effect");
    auto held = render (timelines, 60000);
    check (held.getRMSLevel (0, 59000, 1000) > 0.001f, "reflected timelines stopped at model end");

    AddSynthProcessor gated;
    fixture.load (gated);
    set (gated, "speed", 0.0f); set (gated, "seqon", 1.0f); set (gated, "seqglide", 0.0f);
    set (gated, "seqrate", 2.0f); // 125 ms per step at 120 bpm
    for (int step = 0; step < 8; ++step)
        for (int k = 0; k < 192; ++k) gated.sequence.set (step, k, step % 2 == 0 ? 0.0f : 1.0f);
    auto rhythm = render (gated, 15000, true);
    check (rhythm.getMagnitude (500, 4000) < 1e-6f, "silent sequence step leaked tonal audio");
    check (rhythm.getRMSLevel (0, 7000, 2000) > 0.001f, "audible sequence step never opened");

    struct HostClock : juce::AudioPlayHead
    {
        double beat = 66.125;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setBpm (120.0); info.setPpqPosition (beat); info.setIsPlaying (true);
            return info;
        }
    } clock;
    gated.setPlayHead (&clock);
    set (gated, "seqrate", 4.0f);
    render (gated, 128);
    check (gated.getSequenceStep() == 2, "host transport position did not set sequence step");
    clock.beat = -1.875;
    render (gated, 128);
    check (gated.getSequenceStep() == 6, "negative host pre-roll did not wrap to correct step");
    gated.setPlayHead (nullptr);

    for (bool ambient : { false, true })
    {
        AddSynthProcessor processor;
        fixture.load (processor);
        processor.setLabPreset (ambient);
        auto sound = render (processor, 22050, true, false, 100);
        check (sound.getMagnitude (0, 22050) > 1e-5f, "high-note lab preset unexpectedly silent");
        for (int i = 0; i < sound.getNumSamples(); ++i)
            check (std::isfinite (sound.getSample (0, i)) && std::abs (sound.getSample (0, i)) < 1.0f,
                   "high-note lab preset produced invalid/unbounded audio");
    }
    std::cout << "PASS: sequence editing/recall, reflected timelines, memory release/clear, processor sequence and high notes\n";
}

void testSineLab()
{
    testSequenceStateAndEditing();
    testOutputFilter();
    testProcessorAudio();
}
