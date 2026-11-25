#include "../src/PluginEditorV2.h"
#include <iostream>
#include <stdexcept>

static void require (bool condition, const char* message)
{
    if (! condition) throw std::runtime_error (message);
}
static void near (double a, double b, const char* message)
{
    require (std::abs (a - b) < 0.002, message);
}

static juce::Component* find (juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id) return &parent;
    for (auto* child : parent.getChildren())
        if (auto* result = find (*child, id)) return result;
    return nullptr;
}

static void testMapping()
{
    MorphField::Layer layer;
    layer.sourceLast = 100;
    layer.playback = { .25f, .3f, .6f, false };
    near (layer.sourcePosition (10), 35, "model start must offset its source");
    layer.playback.loop = true;
    near (layer.sourcePosition (45), 40, "forward loop must wrap at its own end");
    near (layer.sourcePosition (345), 40, "loop must continue beyond common field duration");
    auto second = layer;
    second.playback = { 0.0f, .1f, .5f, true };
    near (second.sourcePosition (345), 25, "models must have independent loop periods");
    layer.playback = { NAN, INFINITY, -1, true };
    layer.playback.sanitise();
    require (std::isfinite (layer.sourcePosition (100)), "invalid session markers must be sanitised");
}

static void testShapeBaking()
{
    auto model = std::make_shared<AddmModel>();
    model->nFrames = 65; model->nPartials = 2; model->controlRate = 172.0f;
    model->env.assign (65 * 2, 0.5f);
    model->f0Track.assign (65, 1.0f);
    model->detune.assign (2, 1.0f);
    MorphField::Shape shape;
    shape.partials[1] = 0.5f;
    for (int i = MorphField::Shape::envelopePoints / 2; i < MorphField::Shape::envelopePoints; ++i)
        shape.envelope[(size_t) i] = 0.0f;
    const auto plain = MorphField::build ({ { model, .5f, .5f } });
    const auto shaped = MorphField::build ({ { model, .5f, .5f, {}, shape } });
    const auto level = [] (const MorphField& field, int frame, int partial)
    { return std::exp (field.logEnvAt (field.layers[0], frame, partial)) - MorphField::EPS; };
    near (level (*plain, 8, 0), .5, "neutral shape must leave the model untouched");
    near (level (*shaped, 8, 0), .5, "untouched partials must keep their level");
    near (level (*shaped, 8, 1), .25, "partial level must scale only its own partial");
    near (level (*shaped, 60, 0), 0, "envelope must follow the model timeline");
    shape.envelope[0] = NAN; shape.partials[0] = 9.0f;
    shape.sanitise();
    near (shape.envelope[0], 1, "invalid envelope points must reset to unity");
    near (shape.partials[0], 1, "partial levels must stay within 0..1");
}

static void writeModel (const juce::File& file)
{
    juce::FileOutputStream out (file);
    require (out.openedOk(), "cannot write model test fixture in working directory");
    const juce::String header = R"({"name":"Inspector test","n_frames":65,"n_partials":1,"n_noise_bands":0,"f0_ref":220,"control_rate":32,"inharmonicity":0})";
    out.write ("ADDM", 4); out.writeInt (2); out.writeInt ((int) header.getNumBytesAsUTF8());
    out.write (header.toRawUTF8(), header.getNumBytesAsUTF8());
    for (int i = 0; i < 65; ++i) out.writeFloat (.05f + .2f * (float) i / 64.0f);
    for (int i = 0; i < 65; ++i) out.writeFloat (1.0f);
    out.writeFloat (1.0f);
}

static void addModel (AddSynthProcessor& processor, const juce::File& file, float x = .2f)
{
    juce::String error;
    if (! processor.addModel (file, x, .7f, error)) throw std::runtime_error (error.toStdString());
}

static void testStateAndSelection (const juce::File& file)
{
    AddSynthProcessor processor;
    addModel (processor, file); addModel (processor, file, .8f);
    processor.setSlotPlayback (0, { .15f, .2f, .6f, true });
    processor.setSlotPlayback (1, { .4f, .5f, .9f, false });
    MorphField::Shape sculpted;
    sculpted.envelope[5] = .25f; sculpted.partials[0] = .4f;
    processor.setSlotShape (1, sculpted);
    processor.selectSlot (0);
    juce::MemoryBlock state;
    processor.getStateInformation (state);
    AddSynthProcessor restored;
    restored.setStateInformation (state.getData(), (int) state.getSize());
    require (restored.getSlots().size() == 2, "all models must survive recall");
    near (restored.getSlots()[0].playback.start, .15, "model start must survive recall");
    near (restored.getSlots()[0].playback.loopOut, .6, "loop out must survive recall");
    require (restored.getSlots()[0].playback.loop, "loop mode must survive recall");
    near (restored.getSlots()[1].playback.start, .4, "settings must remain per-model");
    require (! restored.getSlots()[1].playback.loop, "one-shot mode must survive recall");
    require (restored.getSelectedSlot() == 0, "selection must survive recall");
    near (restored.getSlots()[1].shape.envelope[5], .25, "model envelope must survive recall");
    near (restored.getSlots()[1].shape.partials[0], .4, "model partial levels must survive recall");
    require (restored.getSlots()[0].shape.isNeutral(), "envelope and partials must remain per-model");

    auto root = juce::ValueTree::readFromData (state.getData(), state.getSize());
    for (auto slot : root.getChildWithName ("slots"))
        for (const auto* name : { "start", "loopIn", "loopOut", "loop", "envelope", "partialLevels" })
            slot.removeProperty (name, nullptr);
    juce::MemoryBlock old;
    { juce::MemoryOutputStream stream (old, false); root.writeToStream (stream); }
    restored.setStateInformation (old.getData(), (int) old.getSize());
    require (! restored.getSlots()[0].playback.loop, "old sessions must default to one-shot");
    near (restored.getSlots()[1].playback.start, 0, "old sessions must start at zero");
    require (restored.getSlots()[1].shape.isNeutral(), "old sessions must default to a neutral shape");
    processor.setSlotShape (1, {});

    AddSynthEditorV2 editor (processor);
    auto* pad = dynamic_cast<MorphPad*> (find (editor, "morph-pad"));
    require (pad != nullptr, "XY pad missing");
    auto click = [&] (int index)
    {
        const auto slot = processor.getSlots()[(size_t) index];
        const juce::Point<float> point { 16 + slot.x * (pad->getWidth() - 32),
                                        16 + (1 - slot.y) * (pad->getHeight() - 32) };
        const juce::MouseEvent event (juce::Desktop::getInstance().getMainMouseSource(), point,
            juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
            pad, pad, juce::Time(), point, juce::Time(), 1, false);
        pad->mouseDown (event); pad->mouseUp (event);
    };
    const auto cursor = processor.apvts.getParameter ("morphX")->getValue();
    click (1);
    require (processor.getSelectedSlot() == 1, "node click must select that model");
    near (processor.apvts.getParameter ("morphX")->getValue(), cursor, "selection must not move morph cursor");
    auto* inspector = dynamic_cast<ModelInspector*> (find (editor, "model-inspector"));
    inspector->refresh();
    auto* start = dynamic_cast<juce::Slider*> (find (editor, "model-start"));
    start->setValue (35, juce::sendNotificationSync);
    near (processor.getSlots()[1].playback.start, .35, "inspector must edit selected model");
    near (processor.getSlots()[0].playback.start, .15, "inspector must leave other models alone");

    auto stroke = [&] (juce::Point<float> from, juce::Point<float> to, int clicks = 1)
    {
        auto event = [&] (juce::Point<float> point)
        {
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), point,
                juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 1, 0, 0, 0, 0,
                inspector, inspector, juce::Time(), from, juce::Time(), clicks, false);
        };
        inspector->mouseDown (event (from));
        if (clicks == 2) inspector->mouseDoubleClick (event (from));
        else inspector->mouseDrag (event (to));
        inspector->mouseUp (event (to));
    };
    auto view = [&] (int index)
    { dynamic_cast<juce::TextButton*> (find (editor, "model-view-" + juce::String (index)))->onClick(); };
    view (1);
    require (! start->isVisible(), "playback sliders must give way to the envelope plot");
    auto plot = inspector->waveBounds();
    stroke ({ plot.getX(), plot.getCentreY() }, { plot.getCentreX(), plot.getBottom() });
    auto shape = processor.getSlots()[1].shape;
    near (shape.envelope[0], .5, "envelope drawing must set the level under the pointer");
    require (shape.envelope[16] < shape.envelope[0] && shape.envelope[16] > shape.envelope[31],
             "envelope drawing must fill the points it crosses");
    near (shape.envelope[63], 1, "envelope drawing must leave untouched points alone");
    require (processor.getSlots()[0].shape.isNeutral(), "envelope must only edit the selected model");
    near (processor.getSlots()[1].playback.start, .35, "envelope drawing must not move playback markers");
    stroke ({ plot.getX(), plot.getCentreY() }, {}, 2);
    near (processor.getSlots()[1].shape.envelope[0], 1, "double-click must reset an envelope point");

    view (2);
    plot = inspector->waveBounds();
    stroke ({ plot.getCentreX(), plot.getBottom() - plot.getHeight() * .25f },
            { plot.getCentreX(), plot.getBottom() - plot.getHeight() * .25f });
    near (processor.getSlots()[1].shape.partials[0], .25, "partial drawing must set that model's partial level");
    require (processor.getSlots()[0].shape.partials[0] == 1.0f, "partial levels must only edit the selected model");
    near (processor.apvts.getParameter (AddSynthProcessor::partialGainID (0))->getValue(), 1,
          "model partial levels must leave the shared partial editor alone");
    dynamic_cast<juce::TextButton*> (find (editor, "model-shape-reset"))->onClick();
    near (processor.getSlots()[1].shape.partials[0], 1, "reset must restore the model's partial levels");
    require (processor.getSlots()[1].shape.envelope[20] < 1, "resetting partials must keep the envelope");
    view (1);
    dynamic_cast<juce::TextButton*> (find (editor, "model-shape-reset"))->onClick();
    require (processor.getSlots()[1].shape.isNeutral(), "reset must restore the model's envelope");
    view (0);
    require (start->isVisible(), "playback view must bring its sliders back");

    for (auto size : { juce::Point<int> (1380, 900), juce::Point<int> (1180, 800), juce::Point<int> (1920, 1200) })
    {
        editor.setSize (size.x, size.y);
        for (int page = 0; page < 3; ++page)
        {
            auto* tab = dynamic_cast<juce::TextButton*> (find (editor, "workspace-tab-" + juce::String (page)));
            tab->onClick();
            require (pad->isVisible() && pad->getWidth() >= 320 && pad->getHeight() >= 280,
                     "XY pad must stay usable on every tab and size");
            require (inspector->isVisible(), "model inspector must stay visible on every tab");
            for (int index = 0; index < 3; ++index)
            {
                view (index);
                require (inspector->waveBounds().getHeight() >= 70, "every model view needs a usable plot");
                for (auto* child : inspector->getChildren())
                    require (! child->isVisible() || (inspector->getLocalBounds().contains (child->getBounds())
                        && ! inspector->waveBounds().toNearestInt().intersects (child->getBounds())),
                        "model inspector controls must not cover the plot");
            }
            view (0);
            require (! pad->getBounds().intersects (inspector->getBounds()), "pad and inspector must not overlap");
            for (auto* panel : editor.getChildren())
                if (panel->isVisible() && (dynamic_cast<SineLabPanel*> (panel) != nullptr
                    || dynamic_cast<SpectralPlanesPanel*> (panel) != nullptr))
                    for (auto* child : panel->getChildren())
                        require (panel->getLocalBounds().contains (child->getBounds()), "tool panel clips a control");
        }
    }
    processor.removeSlot (0);
    require (processor.getSelectedSlot() == 0, "removal must preserve selection of remaining model");
    processor.removeSlot (0); inspector->refresh();
    require (processor.getSelectedSlot() == -1 && ! start->isEnabled(), "empty pad must disable model edits");
}

static float render (AddSynthProcessor& processor, int blocks, bool noteOn, bool noteOff = false)
{
    float finalPeak = 0;
    for (int block = 0; block < blocks; ++block)
    {
        juce::AudioBuffer<float> audio (2, 256);
        juce::MidiBuffer midi;
        if (block == 0 && noteOn) midi.addEvent (juce::MidiMessage::noteOn (1, 57, .8f), 0);
        if (block == 0 && noteOff) midi.addEvent (juce::MidiMessage::noteOff (1, 57), 0);
        processor.processBlock (audio, midi);
        finalPeak = audio.getMagnitude (0, 256);
        require (std::isfinite (finalPeak) && finalPeak < 1, "playback must remain finite and bounded");
    }
    return finalPeak;
}

static void testAudio (const juce::File& file)
{
    AddSynthProcessor oneShot, looping, shifted, quiet, muted;
    for (auto* processor : { &oneShot, &looping, &shifted, &quiet, &muted })
    {
        addModel (*processor, file);
        processor->setRateAndBufferSizeDetails (44100, 256);
        processor->prepareToPlay (44100, 256);
    }
    looping.setSlotPlayback (0, { .1f, .2f, .45f, true });
    shifted.setSlotPlayback (0, { .65f, 0, 1, false });
    MorphField::Shape half, silent;
    half.partials[0] = .5f;
    silent.envelope.fill (0.0f);
    quiet.setSlotShape (0, half);
    muted.setSlotShape (0, silent);
    const auto initial = render (oneShot, 20, true);
    near (render (quiet, 20, true) / initial, .5, "model partial level must scale the rendered partial");
    require (render (muted, 20, true) < 1e-6f, "a zero model envelope must silence the model");
    require (render (shifted, 20, true) > initial * 1.8f, "start marker must change rendered source position");
    require (render (oneShot, 700, false) < 1e-6f, "one shot must end");
    require (render (looping, 720, true) > .01f, "model loop must sustain beyond source duration");
    require (render (looping, 100, false, true) < 1e-6f, "looping note must release normally");
}

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto file = juce::File::getCurrentWorkingDirectory()
        .getNonexistentChildFile ("addsynth-model-controls", ".addm");
    try
    {
        writeModel (file);
        testMapping(); testShapeBaking(); testStateAndSelection (file); testAudio (file);
        file.deleteFile();
        std::cout << "PASS: model playback, envelope, partial levels, state recall, selection, persistent XY layout and audio\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        file.deleteFile();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
