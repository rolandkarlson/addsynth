#include "PluginEditorV2.h"

namespace
{
void caption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> bounds,
              float size = 13.0f, bool bold = false)
{
    g.setColour (theme::ink);
    g.setFont (theme::font (size, bold));
    g.drawFittedText (text, bounds, juce::Justification::centredLeft, 1);
}
void card (juce::Graphics& g, juce::Rectangle<int> bounds)
{
    g.setColour (theme::panel.withAlpha (0.7f));
    g.fillRoundedRectangle (bounds.toFloat(), 12.0f);
}
void styleSlider (juce::Slider& slider)
{
    slider.setColour (juce::Slider::textBoxTextColourId, theme::ink);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, theme::pill);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}
}

ModelInspector::ModelInspector (AddSynthProcessor& p) : proc (p)
{
    setComponentID ("model-inspector");
    const char* viewNames[] = { "playback", "envelope", "partials" };
    const char* viewTips[] = {
        "Start point and loop region of this model.",
        "Draw this model's level over its own timeline.",
        "Draw a level for each partial of this model only." };
    for (int i = 0; i < 3; ++i)
    {
        auto& button = views[(size_t) i];
        addAndMakeVisible (button);
        button.setButtonText (viewNames[i]);
        button.setTooltip (viewTips[i]);
        button.setComponentID ("model-view-" + juce::String (i));
        button.onClick = [this, i] { setView (i); };
    }
    for (auto* slider : { &start, &loopIn, &loopOut })
    {
        addAndMakeVisible (*slider);
        slider->setRange (0.0, 100.0, 0.1);
        slider->setSliderStyle (juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 22);
        slider->setTextValueSuffix (" %");
        styleSlider (*slider);
        slider->onValueChange = [this] { if (! refreshing) apply(); };
    }
    start.setComponentID ("model-start");
    loopIn.setComponentID ("model-loop-in");
    loopOut.setComponentID ("model-loop-out");
    const char* labels[] = { "start", "loop in", "loop out" };
    int i = 0;
    for (auto* label : { &startLabel, &inLabel, &outLabel })
    {
        addAndMakeVisible (*label);
        label->setText (labels[i++], juce::dontSendNotification);
    }
    addAndMakeVisible (mode);
    mode.setComponentID ("model-loop-mode");
    mode.addItem ("one shot", 1);
    mode.addItem ("forward loop", 2);
    mode.onChange = [this] { if (! refreshing) apply(); };
    addChildComponent (reset);
    reset.setComponentID ("model-shape-reset");
    reset.onClick = [this]
    {
        if (model == nullptr || selected != proc.getSelectedSlot()) return;
        if (view == envelopeView) shape.envelope.fill (1.0f);
        else shape.partials.fill (1.0f);
        proc.setSlotShape (selected, shape);
        refresh();
    };
    addAndMakeVisible (remove);
    remove.onClick = [this] { proc.removeSlot (proc.getSelectedSlot()); refresh(); };
    start.setTooltip ("Where this model starts on the next note.");
    mode.setTooltip ("Forward loop repeats this model independently; note-off fades it with the voice release.");
    setView (playbackView);
    startTimerHz (20);
}

void ModelInspector::setView (int next)
{
    view = juce::jlimit (0, 2, next);
    drawing = false;
    draggingMarker = hover = -1;
    for (int i = 0; i < 3; ++i)
        views[(size_t) i].setToggleState (i == view, juce::dontSendNotification);
    const bool playbackControls = view == playbackView;
    for (auto* slider : { &start, &loopIn, &loopOut }) slider->setVisible (playbackControls);
    for (auto* label : { &startLabel, &inLabel, &outLabel }) label->setVisible (playbackControls);
    mode.setVisible (playbackControls);
    reset.setVisible (! playbackControls);
    reset.setButtonText (view == envelopeView ? "reset envelope" : "reset partials");
    reset.setTooltip (view == envelopeView ? "Restore this model's level to 100% everywhere."
                                           : "Restore every partial of this model to 100%.");
    setMouseCursor (juce::MouseCursor::NormalCursor);
    refresh();
}

void ModelInspector::refresh()
{
    const int index = proc.getSelectedSlot();
    const auto slots = proc.getSlots();
    auto next = juce::isPositiveAndBelow (index, (int) slots.size()) ? slots[(size_t) index].model : nullptr;
    const bool modelChanged = model != next;
    model = next;
    if (selected != index) { draggingMarker = -1; drawing = false; hover = -1; }
    selected = index;
    if (model != nullptr)
    {
        playback = slots[(size_t) index].playback;
        shape = slots[(size_t) index].shape;
    }
    else shape = {};
    // Partial levels change the resynthesis, so redraw it once a stroke ends.
    if (modelChanged || (! drawing && shape.partials != builtPartials)) buildWaveform (modelChanged);
    refreshing = true;
    start.setValue (100.0 * playback.start, juce::dontSendNotification);
    loopIn.setValue (100.0 * playback.loopIn, juce::dontSendNotification);
    loopOut.setValue (100.0 * playback.loopOut, juce::dontSendNotification);
    mode.setSelectedId (playback.loop ? 2 : 1, juce::dontSendNotification);
    for (auto* slider : { &start, &loopIn, &loopOut }) slider->setEnabled (model != nullptr);
    loopIn.setEnabled (model != nullptr && playback.loop);
    loopOut.setEnabled (model != nullptr && playback.loop);
    mode.setEnabled (model != nullptr);
    reset.setEnabled (model != nullptr);
    remove.setEnabled (model != nullptr);
    refreshing = false;
    repaint();
}

void ModelInspector::apply()
{
    if (selected != proc.getSelectedSlot()) { refresh(); return; }
    playback = { (float) start.getValue() / 100.0f, (float) loopIn.getValue() / 100.0f,
                 (float) loopOut.getValue() / 100.0f, mode.getSelectedId() == 2 };
    // Keep marker ordering meaningful when moving either end of the region.
    if (playback.loopIn >= playback.loopOut) playback.loopOut = playback.loopIn + 0.001f;
    proc.setSlotPlayback (selected, playback);
    refresh();
}

float ModelInspector::synthesise (const std::array<float, MorphField::Shape::maxPartials>& levels)
{
    // ADDM stores no source phase/audio. Sample a deterministic sine resynthesis,
    // then cache min/max bins. This is explicitly labelled as a reconstruction.
    constexpr int samplesPerBin = 32;
    lows.fill (0.0f); highs.fill (0.0f);
    const double duration = (model->nFrames - 1) / (double) model->controlRate;
    const int count = juce::jmin (MorphField::Shape::maxPartials, model->nPartials);
    float peak = 1e-7f;
    for (size_t bin = 0; bin < lows.size(); ++bin)
        for (int sample = 0; sample < samplesPerBin; ++sample)
        {
            const double fraction = (double) (bin * samplesPerBin + (size_t) sample)
                / (double) (lows.size() * samplesPerBin - 1);
            const double frame = fraction * (model->nFrames - 1);
            const int a = (int) frame, b = juce::jmin (a + 1, model->nFrames - 1);
            const float mix = (float) (frame - a);
            double value = 0.0;
            for (int k = 0; k < count; ++k)
            {
                if (levels[(size_t) k] <= 0.0f) continue;
                const double harmonic = k + 1;
                const double frequency = model->f0Ref * harmonic * model->detune[(size_t) k]
                    * std::sqrt (1.0 + model->inharmonicity * harmonic * harmonic);
                const float amplitude = (model->envAt (a, k) * (1.0f - mix) + model->envAt (b, k) * mix)
                    * levels[(size_t) k];
                value += amplitude * std::sin (juce::MathConstants<double>::twoPi * frequency * fraction * duration);
            }
            lows[bin] = juce::jmin (lows[bin], (float) value);
            highs[bin] = juce::jmax (highs[bin], (float) value);
            peak = juce::jmax (peak, std::abs ((float) value));
        }
    return peak;
}

void ModelInspector::buildWaveform (bool modelChanged)
{
    builtPartials = shape.partials;
    if (model == nullptr)
    {
        lows.fill (0.0f); highs.fill (0.0f); spectrum.fill (0.0f);
        return;
    }
    const std::array<float, MorphField::Shape::maxPartials> neutral = MorphField::Shape().partials;
    if (modelChanged)
    {
        // Scale against the untouched model so lowered partials visibly shrink it.
        rawPeak = synthesise (neutral);
        spectrum.fill (0.0f);
        float loudest = 1e-7f;
        const int count = juce::jmin (MorphField::Shape::maxPartials, model->nPartials);
        for (int k = 0; k < count; ++k)
        {
            for (int frame = 0; frame < model->nFrames; ++frame)
                spectrum[(size_t) k] = juce::jmax (spectrum[(size_t) k], model->envAt (frame, k));
            loudest = juce::jmax (loudest, spectrum[(size_t) k]);
        }
        // Peak level per partial on a 60 dB scale, for the partials backdrop.
        for (auto& value : spectrum)
            value = juce::jlimit (0.0f, 1.0f, 1.0f + juce::Decibels::gainToDecibels (value / loudest, -60.0f) / 60.0f);
    }
    if (! modelChanged || shape.partials != neutral) synthesise (shape.partials);
    for (size_t i = 0; i < lows.size(); ++i) { lows[i] /= rawPeak; highs[i] /= rawPeak; }
}

juce::Rectangle<float> ModelInspector::waveBounds() const
{
    const float bottomMargin = view == playbackView ? 70.0f : (view == envelopeView ? 34.0f : 48.0f);
    return { 14.0f, 57.0f, (float) getWidth() - 28.0f, (float) getHeight() - 57.0f - bottomMargin };
}

int ModelInspector::shapeCount() const
{
    if (view == envelopeView) return MorphField::Shape::envelopePoints;
    return model != nullptr ? juce::jmin (model->nPartials, MorphField::Shape::maxPartials) : 0;
}

int ModelInspector::shapeIndexAt (float x) const
{
    const auto plot = waveBounds();
    const float fraction = (x - plot.getX()) / plot.getWidth();
    const int count = shapeCount();
    return juce::jlimit (0, juce::jmax (0, count - 1), view == envelopeView
        ? juce::roundToInt (fraction * (float) (count - 1)) : (int) std::floor (fraction * (float) count));
}

float ModelInspector::levelAt (float y) const
{
    const auto plot = waveBounds();
    return juce::jlimit (0.0f, 1.0f, (plot.getBottom() - y) / plot.getHeight());
}

float& ModelInspector::shapeValue (int index)
{
    return view == envelopeView ? shape.envelope[(size_t) index] : shape.partials[(size_t) index];
}

void ModelInspector::paint (juce::Graphics& g)
{
    card (g, getLocalBounds());
    caption (g, "SELECTED MODEL", { 14, 8, 130, 17 }, 11.0f, true);
    caption (g, model != nullptr ? model->name : "Choose a model on the XY pad",
             { 14, 26, juce::jmax (80, views[0].getX() - 22), 25 }, 18.0f, true);
    auto wave = waveBounds();
    g.setColour (theme::padBg);
    g.fillRoundedRectangle (wave, 7.0f);
    if (model == nullptr)
    {
        caption (g, "Drop audio onto the pad, then select a model to edit its playback, envelope and partials.",
                 wave.toNearestInt().reduced (18), 14.0f);
        return;
    }
    const juce::Rectangle<int> footer { 16, getHeight() - 28, getWidth() - 32, 18 };
    const auto percent = [] (float level)
    {
        return juce::String (juce::roundToInt (level * 100.0f)) + "%  /  "
            + (level <= 0.0f ? juce::String ("muted") : juce::String (20.0f * std::log10 (level), 1) + " dB");
    };
    const float seconds = (float) (model->nFrames - 1) / model->controlRate;
    if (view == partialsView)
    {
        const int count = shapeCount();
        const float width = wave.getWidth() / (float) count;
        g.setColour (theme::ink.withAlpha (0.12f));
        g.drawHorizontalLine (juce::roundToInt (wave.getCentreY()), wave.getX(), wave.getRight());
        g.setFont (theme::font (10.5f));
        const int tickStep = count > 96 ? 16 : (count > 32 ? 8 : (count > 12 ? 4 : 1));
        juce::Path outline;
        for (int k = 0; k < count; ++k)
        {
            const float level = shape.partials[(size_t) k];
            const float height = juce::jmax (1.5f, level * wave.getHeight());
            const float x = wave.getX() + (float) k * width;
            const bool hovered = k == hover;
            g.setColour (hovered ? theme::ink : theme::accent.withAlpha (0.85f));
            g.fillRect (x + 0.5f, wave.getBottom() - height, juce::jmax (1.0f, width - 1.0f), height);
            g.setColour (hovered ? theme::pill : theme::ink.withAlpha (0.55f));
            g.fillRect (x + 0.5f, wave.getBottom() - height, juce::jmax (1.0f, width - 1.0f), 1.5f);
            const float y = wave.getBottom() - 2.0f - spectrum[(size_t) k] * (wave.getHeight() - 4.0f);
            if (k == 0) outline.startNewSubPath (x + width * 0.5f, y);
            else outline.lineTo (x + width * 0.5f, y);
            if (k == 0 || (k + 1) % tickStep == 0)
            {
                g.setColour (theme::ink.withAlpha (0.65f));
                g.drawText (juce::String (k + 1), (int) juce::jlimit (wave.getX(), wave.getRight() - 28.0f,
                            x + width * 0.5f - 14.0f), (int) wave.getBottom() + 2, 28, 13, juce::Justification::centred);
            }
        }
        g.setColour (theme::node (selected).withAlpha (0.9f));
        g.strokePath (outline, juce::PathStrokeType (1.6f));
        caption (g, hover >= 0 ? "partial " + juce::String (hover + 1) + "  /  " + percent (shape.partials[(size_t) hover])
                               : juce::String ("drag to draw this model's partial levels  /  line: its own spectrum"
                                               "  /  double-click a bar to reset  /  edits apply to new notes"),
                 footer, 11.0f);
        return;
    }

    auto x = [&] (float position) { return wave.getX() + position * wave.getWidth(); };
    if (playback.loop)
    {
        g.setColour (theme::accent.withAlpha (view == playbackView ? 0.18f : 0.1f));
        g.fillRect (juce::Rectangle<float> (x (playback.loopIn), wave.getY(),
                     (playback.loopOut - playback.loopIn) * wave.getWidth(), wave.getHeight()));
    }
    const float middle = wave.getCentreY(), height = wave.getHeight() * (view == playbackView ? 0.35f : 0.42f);
    const auto drawWave = [&] (float alpha, bool shaped)
    {
        g.setColour (theme::node (selected).withAlpha (alpha));
        for (size_t i = 0; i < lows.size(); ++i)
        {
            const float level = shaped ? shape.envelopeAt ((double) i / (double) (lows.size() - 1)) : 1.0f;
            const float xx = wave.getX() + (float) i * wave.getWidth() / (float) lows.size();
            g.drawLine (xx, middle - highs[i] * height * level, xx, middle - lows[i] * height * level,
                        juce::jmax (1.0f, wave.getWidth() / (float) lows.size()));
        }
    };
    if (view == envelopeView)
    {
        drawWave (0.22f, false);
        drawWave (0.8f, true);
        g.setColour (theme::ink.withAlpha (0.12f));
        g.drawHorizontalLine (juce::roundToInt (wave.getCentreY()), wave.getX(), wave.getRight());
        g.setColour (theme::ink.withAlpha (0.35f));
        g.drawLine (x (playback.start), wave.getY() + 2, x (playback.start), wave.getBottom() - 2, 1.0f);
        juce::Path curve;
        const int points = MorphField::Shape::envelopePoints;
        const auto point = [&] (int index)
        {
            return juce::Point<float> (x ((float) index / (float) (points - 1)),
                wave.getBottom() - 1.0f - shape.envelope[(size_t) index] * (wave.getHeight() - 2.0f));
        };
        curve.startNewSubPath (point (0));
        for (int index = 1; index < points; ++index) curve.lineTo (point (index));
        g.setColour (theme::ink);
        g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved));
        if (hover >= 0)
        {
            g.setColour (theme::pill);
            g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (point (hover)));
            g.setColour (theme::ink);
            g.drawEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (point (hover)), 1.6f);
        }
        caption (g, hover >= 0 ? juce::String (seconds * (float) hover / (float) (points - 1), 2) + " s  /  "
                                     + percent (shape.envelope[(size_t) hover])
                               : juce::String ("drag to draw this model's level over time  /  double-click a point"
                                               " to reset  /  edits apply to new notes"),
                 footer, 11.0f);
        return;
    }

    drawWave (0.8f, true);
    auto marker = [&] (float position, juce::Colour colour, const juce::String& label, int offset)
    {
        g.setColour (colour);
        g.drawLine (x (position), wave.getY() + 2, x (position), wave.getBottom() - 2, 2.0f);
        g.fillEllipse (x (position) - 4, wave.getY() + (float) offset, 8, 8);
        g.setFont (theme::font (10.0f, true));
        g.drawText (label, juce::jlimit ((int) wave.getX() + 2, (int) wave.getRight() - 40,
                    (int) x (position) + 6), (int) wave.getY() + offset - 2, 38, 15,
                    juce::Justification::centredLeft);
    };
    if (playback.loop)
    {
        marker (playback.loopIn, theme::accent.darker (0.4f), "IN", 16);
        marker (playback.loopOut, theme::accent.darker (0.4f), "OUT", 16);
    }
    marker (playback.start, theme::ink, "START", 3);
    caption (g, "resynthesized waveform  /  " + juce::String (seconds, 2)
        + " s  /  edits apply to new notes", { 16, getHeight() - 68, getWidth() - 32, 18 }, 11.0f);
}

void ModelInspector::resized()
{
    remove.setBounds (getWidth() - 132, 17, 118, 27);
    mode.setBounds (getWidth() - 286, 17, 144, 27);
    reset.setBounds (mode.getBounds());
    for (int i = 0; i < 3; ++i)
        views[(size_t) i].setBounds (getWidth() - 296 - (3 - i) * 76, 17, 74, 27);
    const int width = (getWidth() - 28) / 3;
    std::array<juce::Slider*, 3> sliders { &start, &loopIn, &loopOut };
    std::array<juce::Label*, 3> labels { &startLabel, &inLabel, &outLabel };
    for (int i = 0; i < 3; ++i)
    {
        auto cell = juce::Rectangle<int> (14 + i * width, getHeight() - 43, width - 8, 29);
        labels[(size_t) i]->setBounds (cell.removeFromLeft (52));
        sliders[(size_t) i]->setBounds (cell);
    }
}

void ModelInspector::mouseDown (const juce::MouseEvent& e)
{
    if (model == nullptr || ! waveBounds().contains (e.position)) return;
    if (view != playbackView)
    {
        if (selected != proc.getSelectedSlot() || shapeCount() == 0) return;
        drawing = true;
        previousIndex = hover = shapeIndexAt (e.position.x);
        previousLevel = levelAt (e.position.y);
        shapeValue (previousIndex) = previousLevel;
        proc.setSlotShape (selected, shape);
        repaint();
        return;
    }
    const float position = (e.position.x - waveBounds().getX()) / waveBounds().getWidth();
    draggingMarker = 0;
    float distance = std::abs (position - playback.start);
    if (playback.loop)
    {
        if (std::abs (position - playback.loopIn) < distance)
        { draggingMarker = 1; distance = std::abs (position - playback.loopIn); }
        if (std::abs (position - playback.loopOut) < distance) draggingMarker = 2;
        // When start and loop-in coincide, use the vertically offset handle.
        if (std::abs (playback.start - playback.loopIn) < 0.01f && distance < 0.025f)
            draggingMarker = e.position.y < waveBounds().getY() + 15 ? 0 : 1;
    }
    mouseDrag (e);
}

void ModelInspector::mouseDrag (const juce::MouseEvent& e)
{
    if (drawing)
    {
        if (selected != proc.getSelectedSlot()) { drawing = false; return; }
        const int next = shapeIndexAt (e.position.x);
        const float level = levelAt (e.position.y);
        const int distance = std::abs (next - previousIndex);
        const int direction = next >= previousIndex ? 1 : -1;
        // Fill every crossed point even when mouse events skip several.
        for (int step = 0; step <= distance; ++step)
        {
            const float fraction = distance == 0 ? 1.0f : (float) step / (float) distance;
            shapeValue (previousIndex + step * direction) = previousLevel + (level - previousLevel) * fraction;
        }
        previousIndex = hover = next;
        previousLevel = level;
        proc.setSlotShape (selected, shape);
        repaint();
        return;
    }
    if (draggingMarker < 0 || selected != proc.getSelectedSlot()) return;
    const float position = juce::jlimit (0.0f, 1.0f,
        (e.position.x - waveBounds().getX()) / waveBounds().getWidth());
    if (draggingMarker == 0) playback.start = position;
    if (draggingMarker == 1) playback.loopIn = juce::jmin (position, playback.loopOut - 0.001f);
    if (draggingMarker == 2) playback.loopOut = juce::jmax (position, playback.loopIn + 0.001f);
    proc.setSlotPlayback (selected, playback);
    refresh();
}

void ModelInspector::mouseUp (const juce::MouseEvent&)
{
    draggingMarker = -1;
    if (drawing) { drawing = false; refresh(); }
}

void ModelInspector::mouseMove (const juce::MouseEvent& e)
{
    const bool inside = view != playbackView && model != nullptr && shapeCount() > 0
        && waveBounds().contains (e.position);
    hover = inside ? shapeIndexAt (e.position.x) : -1;
    setMouseCursor (inside ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void ModelInspector::mouseExit (const juce::MouseEvent&)
{
    hover = -1;
    repaint();
}

void ModelInspector::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (view == playbackView || model == nullptr || shapeCount() == 0
        || selected != proc.getSelectedSlot() || ! waveBounds().contains (e.position)) return;
    drawing = false;
    shapeValue (shapeIndexAt (e.position.x)) = 1.0f;
    proc.setSlotShape (selected, shape);
    refresh();
}

AddSynthEditorV2::AddSynthEditorV2 (AddSynthProcessor& p)
    : AudioProcessorEditor (p), proc (p), pad (p), inspector (p), keyboard (p),
      partials (p), lab (p, partials), planes (p)
{
    setLookAndFeel (&look);
    pad.selectModels = true;
    pad.setComponentID ("morph-pad");
    for (auto* child : std::array<juce::Component*, 8> { &pad, &inspector,
           &keyboard, &partials, &lab, &planes, &add, &random }) addAndMakeVisible (*child);
    add.onClick = [this] { openModels(); };
    random.onClick = [this] { proc.randomizeParams(); };
    const char* names[] = { "voice & tone", "motion & memory", "spectral planes" };
    for (int i = 0; i < 3; ++i)
    {
        auto& tab = tabs[(size_t) i];
        addAndMakeVisible (tab);
        tab.setButtonText (names[i]);
        tab.setComponentID ("workspace-tab-" + juce::String (i));
        tab.onClick = [this, i] { page = i; resized(); repaint(); };
    }
    const std::pair<const char*, const char*> definitions[] = {
        { "attack", "attack" }, { "decay", "decay" }, { "sustain", "sustain" },
        { "release", "release" }, { "speed", "scan speed" }, { "blur", "blur" },
        { "tilt", "tilt" }, { "oddeven", "odd / even" }, { "stretch", "stretch" },
        { "partials", "partials" }, { "drift", "drift" }, { "pitchenv", "pitch env" },
        { "bend", "pitch glide" }, { "ring", "ring" }, { "damp", "damp" },
        { "loopstart", "loop position" }, { "looplen", "loop length" },
        { "spreadx", "spread x" }, { "spready", "spread y" }, { "spreadn", "voices" },
        { "width", "stereo width" }, { "gain", "master" },
        { "lfo1rate", "lfo 1 hz" }, { "lfo2rate", "lfo 2 hz" }
    };
    for (auto [id, name] : definitions) addKnob (id, name);
    knobs.at ("gain")->slider.setSliderStyle (juce::Slider::LinearHorizontal);
    knobs.at ("gain")->slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 54, 22);
    const std::pair<juce::ToggleButton*, const char*> switches[] = {
        { &syncSpeed, "syncspeed" }, { &syncLoop, "syncloop" },
        { &modal, "modal" }, { &midiScale, "midiscale" }
    };
    for (auto [button, id] : switches)
    {
        addAndMakeVisible (*button);
        button->setColour (juce::ToggleButton::textColourId, theme::ink);
        button->setColour (juce::ToggleButton::tickColourId, theme::ink);
        toggles.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, id, *button));
    }
    for (size_t i = 0; i < mods.size(); ++i)
    {
        auto& row = mods[i];
        row.source.addItemList ({ "off", "lfo 1", "lfo 2", "env pos", "voice idx", "random", "velocity", "note" }, 1);
        row.destination.addItemList ({ "morph x", "morph y", "tilt", "blur", "speed", "time spread", "width",
                                      "stretch", "odd/even", "partials", "pitch", "loop pos" }, 1);
        row.amount.setSliderStyle (juce::Slider::LinearHorizontal);
        row.amount.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 22);
        styleSlider (row.amount);
        addAndMakeVisible (row.source); addAndMakeVisible (row.destination); addAndMakeVisible (row.amount);
        const auto id = "mod" + juce::String ((int) i);
        row.sourceA = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id + "src", row.source);
        row.destinationA = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id + "dst", row.destination);
        row.amountA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id + "amt", row.amount);
    }
    setResizable (true, true);
    setResizeLimits (1180, 800, 1920, 1200);
    setSize (1380, 900);
}

AddSynthEditorV2::~AddSynthEditorV2() { setLookAndFeel (nullptr); }

void AddSynthEditorV2::addKnob (const juce::String& id, const juce::String& label)
{
    auto knob = std::make_unique<Knob>();
    knob->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 66, 18);
    styleSlider (knob->slider);
    knob->slider.setComponentID (id);
    knob->label.setText (label, juce::dontSendNotification);
    knob->label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (knob->slider); addAndMakeVisible (knob->label);
    knob->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, knob->slider);
    knobs.emplace (id, std::move (knob));
}

void AddSynthEditorV2::placeKnob (const juce::String& id, juce::Rectangle<int> bounds)
{
    auto& knob = *knobs.at (id);
    knob.label.setBounds (bounds.removeFromTop (17));
    knob.slider.setBounds (bounds);
}

void AddSynthEditorV2::openModels()
{
    chooser = std::make_unique<juce::FileChooser> ("Add audio or models", proc.getLastModelDir(),
                                                 AddSynthProcessor::importWildcards());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                          | juce::FileBrowserComponent::canSelectMultipleItems,
        [safe = juce::Component::SafePointer<AddSynthEditorV2> (this)] (const juce::FileChooser& choice)
        {
            if (safe == nullptr) return;
            juce::StringArray files;
            for (const auto& file : choice.getResults())
                files.add (file.getFullPathName());
            safe->proc.importFiles (files);
        });
}

bool AddSynthEditorV2::isInterestedInFileDrag (const juce::StringArray& files)
{
    return pad.isInterestedInFileDrag (files);
}

void AddSynthEditorV2::filesDropped (const juce::StringArray& files, int, int)
{
    proc.importFiles (files);
}

void AddSynthEditorV2::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);
    caption (g, "addsynth", { 20, 11, 145, 32 }, 29.0f, true);
    caption (g, "SINE LAB 2", { 168, 16, 124, 25 }, 12.0f, true);
    caption (g, "models become an instrument", { 302, 18, 250, 22 }, 12.0f);
    card (g, morphArea);
    caption (g, "MORPH FIELD", morphArea.withHeight (28).translated (12, 3), 12.0f, true);
    caption (g, "drop audio to add  /  click a model to edit  /  drag to morph",
             { morphArea.getX() + 12, pad.getBottom() + 5, morphArea.getWidth() - 24, 20 }, 11.0f);
    caption (g, "VOICE SPREAD & SPACE", { morphArea.getX() + 12, pad.getBottom() + 32,
             morphArea.getWidth() - 24, 20 }, 11.0f, true);
    caption (g, "PITCH SCALE", scaleArea.withHeight (22), 11.0f, true);
    if (page == 0)
    {
        const char* titles[] = { "VOICE & PLAYBACK", "TONE & PITCH", "RESONANCE & SHARED LOOP" };
        for (size_t i = 0; i < soundGroups.size(); ++i)
        {
            card (g, soundGroups[i]);
            caption (g, titles[i], soundGroups[i].reduced (10, 4).withHeight (22), 11.0f, true);
        }
    }
    card (g, modArea);
    caption (g, "MODULATION  /  source > amount > destination", modArea.reduced (12, 6).withHeight (18), 11.0f, true);
}

void AddSynthEditorV2::resized()
{
    auto bounds = getLocalBounds().reduced (16);
    auto header = bounds.removeFromTop (42);
    placeKnob ("gain", header.removeFromRight (220));
    header.removeFromRight (16);
    random.setBounds (header.removeFromRight (148).withHeight (29).translated (0, 4));
    bounds.removeFromTop (8);
    modArea = bounds.removeFromBottom (126);
    bounds.removeFromBottom (12);
    auto left = bounds.removeFromLeft (juce::jlimit (350, 460, getWidth() / 3));
    morphArea = left;
    bounds.removeFromLeft (14);
    auto leftInner = left.reduced (12);
    auto top = leftInner.removeFromTop (28);
    add.setBounds (top.removeFromRight (124));
    leftInner.removeFromTop (6);
    scaleArea = leftInner.removeFromBottom (84);
    keyboard.setBounds (scaleArea.withTrimmedTop (28));
    midiScale.setBounds (scaleArea.getRight() - 105, scaleArea.getY(), 105, 22);
    leftInner.removeFromBottom (12);
    auto spread = leftInner.removeFromBottom (128);
    pad.setBounds (leftInner);
    auto spreadKnobs = spread.withTrimmedTop (53);
    for (const auto& id : { "spreadx", "spready", "spreadn", "width" })
        placeKnob (id, spreadKnobs.removeFromLeft (spread.getWidth() / 4).reduced (2, 0));

    inspector.setBounds (bounds.removeFromTop (juce::jlimit (202, 260, getHeight() / 4)));
    bounds.removeFromTop (10);
    partials.setBounds (bounds.removeFromBottom (132));
    bounds.removeFromBottom (10);
    auto tabRow = bounds.removeFromTop (30);
    for (int i = 0; i < 3; ++i)
    {
        tabs[(size_t) i].setBounds (tabRow.removeFromLeft (162).reduced (0, 1).withTrimmedRight (8));
        tabs[(size_t) i].setToggleState (page == i, juce::dontSendNotification);
    }
    bounds.removeFromTop (7);
    soundArea = bounds;
    lab.setBounds (bounds); planes.setBounds (bounds);
    lab.setVisible (page == 1); planes.setVisible (page == 2);
    const std::vector<std::vector<juce::String>> groups {
        { "attack", "decay", "sustain", "release", "speed", "blur" },
        { "tilt", "oddeven", "stretch", "partials", "drift", "pitchenv", "bend" },
        { "ring", "damp", "loopstart", "looplen" }
    };
    for (int group = 0; group < 3; ++group)
    {
        auto area = soundArea.withWidth (soundArea.getWidth() / 3)
            .translated (group * (soundArea.getWidth() / 3), 0).withTrimmedRight (group < 2 ? 8 : 0);
        soundGroups[(size_t) group] = area;
        auto cells = area.reduced (7, 5).withTrimmedTop (24).withTrimmedBottom (26);
        const int cols = group == 1 ? 4 : (group == 0 ? 3 : 2);
        const auto& ids = groups[(size_t) group];
        for (size_t i = 0; i < ids.size(); ++i)
        {
            auto& knob = *knobs.at (ids[i]);
            knob.slider.setVisible (page == 0); knob.label.setVisible (page == 0);
            placeKnob (ids[i], { cells.getX() + (int) (i % (size_t) cols) * cells.getWidth() / cols,
                cells.getY() + (int) (i / (size_t) cols) * cells.getHeight() / 2,
                cells.getWidth() / cols, cells.getHeight() / 2 });
        }
    }
    syncSpeed.setVisible (page == 0); syncLoop.setVisible (page == 0); modal.setVisible (page == 0);
    syncSpeed.setBounds (soundGroups[0].getX() + 10, soundGroups[0].getBottom() - 27, 122, 23);
    modal.setBounds (soundGroups[2].getX() + 6, soundGroups[2].getBottom() - 27, 116, 23);
    syncLoop.setBounds (soundGroups[2].getRight() - 100, soundGroups[2].getBottom() - 27, 96, 23);

    auto mod = modArea.reduced (12, 7).withTrimmedTop (22);
    auto lfos = mod.removeFromRight (154);
    placeKnob ("lfo1rate", lfos.removeFromLeft (77));
    placeKnob ("lfo2rate", lfos);
    mod.removeFromRight (10);
    for (size_t i = 0; i < mods.size(); ++i)
    {
        auto row = juce::Rectangle<int> (mod.getX() + (int) (i % 3) * mod.getWidth() / 3,
            mod.getY() + (int) (i / 3) * 42, mod.getWidth() / 3 - 12, 34);
        mods[i].source.setBounds (row.removeFromLeft (90).withHeight (27));
        mods[i].destination.setBounds (row.removeFromRight (100).withHeight (27));
        mods[i].amount.setBounds (row.withHeight (27));
    }
}
