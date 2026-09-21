#include "../src/PluginEditor.h"
#include <iostream>
#include <stdexcept>

static void require (bool condition, const char* message)
{
    if (! condition) throw std::runtime_error (message);
}

static void near (float actual, float expected, const char* message)
{
    require (std::abs (actual - expected) < 0.002f, message);
}

static void testState()
{
    AddSynthProcessor processor;
    for (int k = 0; k < AddVoice::maxPartials; ++k)
        near (processor.apvts.getParameter (AddSynthProcessor::partialGainID (k))->getValue(),
              1.0f, "new curves must be neutral");

    const int indices[] = { 0, 63, 64, 127, 128, 191 };
    for (int k : indices)
        processor.apvts.getParameter (AddSynthProcessor::partialGainID (k))
            ->setValueNotifyingHost ((float) k / 192.0f);
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    AddSynthProcessor restored;
    restored.setStateInformation (saved.getData(), (int) saved.getSize());
    for (int k : indices)
        near (restored.apvts.getParameter (AddSynthProcessor::partialGainID (k))->getValue(),
              (float) k / 192.0f, "curve must survive session recall");

    auto legacy = juce::ValueTree::readFromData (saved.getData(), saved.getSize());
    auto params = legacy.getChildWithName ("params");
    for (int i = params.getNumChildren(); --i >= 0;)
        if (params.getChild (i).getProperty ("id").toString().startsWith ("partialGain"))
            params.removeChild (i, nullptr);
    juce::MemoryBlock oldState;
    juce::MemoryOutputStream stream (oldState, false);
    legacy.writeToStream (stream);
    restored.setStateInformation (oldState.getData(), (int) oldState.getSize());
    for (int k = 0; k < AddVoice::maxPartials; ++k)
        near (restored.apvts.getParameter (AddSynthProcessor::partialGainID (k))->getValue(),
              1.0f, "old sessions must reset an edited curve to neutral");
}

struct GestureListener : juce::AudioProcessorParameter::Listener
{
    int starts = 0, ends = 0;
    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int, bool starting) override
    {
        if (starting) ++starts;
        else ++ends;
    }
};

static void testDrawing()
{
    AddSynthProcessor processor;
    GestureListener listener;
    for (int k = 0; k < AddVoice::maxPartials; ++k)
        processor.apvts.getParameter (AddSynthProcessor::partialGainID (k))->addListener (&listener);
    {
        PartialEditor editor (processor);
        editor.setBounds (0, 0, 872, 140);
        auto value = [&] (int k)
        {
            return processor.apvts.getParameter (AddSynthProcessor::partialGainID (k))->getValue();
        };
        auto event = [&] (float x, float y)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(),
                { x, y }, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &editor, &editor, juce::Time(),
                { x, y }, juce::Time(), 1, false);
        };
        // Plot is x=46..860, y=36..114 at this size. A single fast event
        // across the full bank must fill every bar, including partial 192.
        const float width = 814.0f / 192.0f;
        editor.mouseDown (event (46.0f + width * 0.5f, 114.0f - 78.0f * 0.2f));
        editor.mouseDrag (event (860.0f - width * 0.5f, 114.0f - 78.0f * 0.8f));
        editor.mouseUp (event (859.0f, 50.0f));
        for (int k = 0; k < 192; ++k)
            near (value (k), 0.2f + 0.6f * (float) k / 191.0f, "fast drawing left holes");
        require (listener.starts == 192 && listener.ends == 192, "host gestures must balance");

        editor.mouseDown (event (859.0f, 75.0f));
        editor.mouseDrag (event (-200.0f, 200.0f));
        editor.mouseUp (event (-200.0f, 200.0f));
        near (value (0), 0.0f, "drawing beyond bottom/left must clamp to mute");
        near (value (96), 0.5f * 96.0f / 191.0f, "reverse strokes must interpolate");
        editor.mouseDoubleClick (event (46.0f + width * 0.5f, 70.0f));
        near (value (0), 1.0f, "double-click must restore one partial");

        juce::ComboBox* range = nullptr;
        juce::TextButton* reset = nullptr;
        for (auto* child : editor.getChildren())
        {
            if (auto* combo = dynamic_cast<juce::ComboBox*> (child)) range = combo;
            if (auto* button = dynamic_cast<juce::TextButton*> (child)) reset = button;
        }
        require (range != nullptr && reset != nullptr, "editor controls missing");
        range->setSelectedId (4, juce::sendNotificationSync);
        editor.mouseDown (event (48.0f, 75.0f));
        editor.mouseDrag (event (1200.0f, -100.0f));
        editor.mouseUp (event (1200.0f, -100.0f));
        near (value (128), 0.5f, "zoom must edit partial 129, not partial 1");
        near (value (191), 1.0f, "upper/right edge must clamp to 100 percent");
        near (value (0), 1.0f, "zoom must leave other banks alone");
        reset->onClick();
        for (int k = 0; k < 192; ++k)
            near (value (k), 1.0f, "reset all must include hidden banks");

        // Closing an editor during a stroke must also close all host gestures.
        editor.mouseDown (event (48.0f, 75.0f));
    }
    require (listener.starts == listener.ends, "editor destruction left open gestures");
    for (int k = 0; k < AddVoice::maxPartials; ++k)
        processor.apvts.getParameter (AddSynthProcessor::partialGainID (k))->removeListener (&listener);
}

static std::shared_ptr<AddmModel> model (float level)
{
    auto result = std::make_shared<AddmModel>();
    result->nFrames = 32;
    result->nPartials = 192;
    result->nNoiseBands = 1;
    result->env.resize (32 * 192);
    for (int frame = 0; frame < 32; ++frame)
        for (int k : { 0, 2, 69, 191 })
            result->env[(size_t) frame * 192 + (size_t) k] = level;
    result->f0Track.assign (32, 1.0f);
    result->detune.assign (192, 1.0f);
    result->bandFreqs = { 500.0f };
    result->noiseEnv.assign (32, 0.1f);
    return result;
}

struct RenderVoice
{
    juce::Synthesiser synth;
    AddVoice* voice = new AddVoice();
    RenderVoice (std::shared_ptr<const MorphField> field, const AddVoice::Params& params)
    {
        synth.addSound (new AddSound());
        synth.addVoice (voice);
        synth.setCurrentPlaybackSampleRate (44100.0);
        voice->setField (std::move (field));
        voice->setParams (params);
        synth.noteOn (1, 33, 1.0f); // 55 Hz, all 192 partials below Nyquist
    }
    juce::AudioBuffer<float> render (int count = 8820)
    {
        juce::AudioBuffer<float> buffer (2, count);
        buffer.clear();
        juce::MidiBuffer midi;
        synth.renderNextBlock (buffer, midi, 0, count);
        return buffer;
    }
};

static float harmonicAmplitude (const juce::AudioBuffer<float>& buffer, int harmonic)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const double phase = juce::MathConstants<double>::twoPi * 55.0 * harmonic * i / 44100.0;
        re += buffer.getSample (0, i) * std::cos (phase);
        im += buffer.getSample (0, i) * std::sin (phase);
    }
    return (float) (2.0 * std::hypot (re, im) / buffer.getNumSamples());
}

static void testAudio()
{
    auto field = MorphField::build ({ { model (0.1f), 0.0f, 0.5f },
                                     { model (0.3f), 1.0f, 0.5f } });
    AddVoice::Params params;
    params.speed = 0.0f;
    params.noise = 0.0f;
    for (float cursor : { 0.01f, 0.5f, 0.99f })
    {
        params.cursorX = cursor;
        RenderVoice original (field, params);
        original.render (1024); // measure after the protected note-on transition
        auto unshaped = original.render();
        auto shapedParams = params;
        shapedParams.partialGains[0] = 0.0f;
        shapedParams.partialGains[69] = 0.35f;
        shapedParams.partialGains[191] = 0.6f;
        RenderVoice shaped (field, shapedParams);
        shaped.render (1024);
        auto output = shaped.render();
        for (int k : { 0, 2, 69, 191 })
        {
            auto reference = harmonicAmplitude (unshaped, k + 1);
            require (reference > 0.05f, "reference partial missing");
            near (harmonicAmplitude (output, k + 1) / reference,
                  shapedParams.partialGains[(size_t) k], "mask must scale every morph position");
        }
    }

    // Editing a held, frozen note must respond even with long blur and resonance.
    for (bool modal : { false, true })
    {
        params.modal = modal;
        params.ring = 2.0f;
        params.blur = 2.0f;
        params.partialGains.fill (1.0f);
        RenderVoice live (field, params);
        require (live.render().getMagnitude (0, 8820) > 0.01f, "live note was silent");
        params.partialGains.fill (0.0f);
        live.voice->setParams (params);
        auto transition = live.render (1024);
        for (int i = 1; i < transition.getNumSamples(); ++i)
            require (std::isfinite (transition.getSample (0, i)), "non-finite live output");
        auto muted = live.render();
        require (muted.getMagnitude (0, 8820) < 1e-7f, "live mute must also silence blur/modal tails");
        params.partialGains.fill (1.0f);
        live.voice->setParams (params);
        require (live.render().getMagnitude (0, 8820) > 0.01f, "live reset must restore the sound");
    }
    params.modal = false;
    params.blur = 0.0f;
    params.noise = 1.0f;
    params.partialGains.fill (0.0f);
    RenderVoice noise (field, params);
    require (noise.render().getMagnitude (0, 8820) < 1e-7f, "sine-only branch must never render the noise layer");
}

void testSineLab();

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    try
    {
        testState();
        testDrawing();
        testAudio();
        testSineLab();
        std::cout << "PASS: partial editor and sine lab\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
