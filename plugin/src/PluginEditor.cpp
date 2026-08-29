#include "PluginEditor.h"

// ======================================================= AddLookAndFeel

AddLookAndFeel::AddLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, theme::ink);
    setColour (juce::Slider::textBoxOutlineColourId,
               juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, theme::pill);
    setColour (juce::Label::textColourId, theme::ink);
    setColour (juce::TextButton::buttonColourId, theme::pill);
    setColour (juce::TextButton::textColourOffId, theme::ink);
    setColour (juce::PopupMenu::backgroundColourId, theme::pill);
    setColour (juce::PopupMenu::textColourId, theme::ink);
}

void AddLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y,
                                       int w, int h, float pos,
                                       float startAngle, float endAngle,
                                       juce::Slider&)
{
    auto bounds = juce::Rectangle<float> ((float) x, (float) y,
                                          (float) w, (float) h).reduced (3.0f);
    float d = juce::jmin (bounds.getWidth(), bounds.getHeight());
    auto knob = juce::Rectangle<float> (d, d).withCentre (bounds.getCentre());
    auto centre = knob.getCentre();
    float r = d * 0.5f;

    // soft drop shadow + charcoal body with a slight top-light gradient
    g.setColour (theme::ink.withAlpha (0.18f));
    g.fillEllipse (knob.translated (0.0f, 1.5f));
    juce::ColourGradient grad (theme::knobCol.brighter (0.25f),
                               centre.x, knob.getY(),
                               theme::knobCol.darker (0.1f),
                               centre.x, knob.getBottom(), false);
    g.setGradientFill (grad);
    g.fillEllipse (knob);
    g.setColour (theme::ink.withAlpha (0.35f));
    g.drawEllipse (knob, 1.0f);

    // white tick
    float angle = startAngle + pos * (endAngle - startAngle);
    juce::Point<float> p1 (centre.x + 0.35f * r * std::sin (angle),
                           centre.y - 0.35f * r * std::cos (angle));
    juce::Point<float> p2 (centre.x + 0.82f * r * std::sin (angle),
                           centre.y - 0.82f * r * std::cos (angle));
    g.setColour (theme::pill);
    g.drawLine ({ p1, p2 }, juce::jmax (2.0f, r * 0.14f));
}

void AddLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    auto bg = label.findColour (juce::Label::backgroundColourId);
    if (! bg.isTransparent())   // value pills under the knobs
    {
        g.setColour (bg);
        g.fillRoundedRectangle (label.getLocalBounds().toFloat().reduced (1.0f),
                                label.getHeight() * 0.5f - 1.0f);
    }
    if (! label.isBeingEdited())
    {
        g.setColour (label.findColour (juce::Label::textColourId)
                          .withMultipliedAlpha (label.isEnabled() ? 1.0f : 0.5f));
        g.setFont (theme::font (juce::jmin (13.0f,
                                (float) label.getHeight() - 3.0f)));
        g.drawFittedText (label.getText(), label.getLocalBounds(),
                          label.getJustificationType(), 1, 1.0f);
    }
}

void AddLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                           const juce::Colour&,
                                           bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    auto c = theme::pill;
    if (down) c = c.darker (0.12f);
    else if (over) c = c.darker (0.05f);
    g.setColour (c);
    g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
    g.setColour (theme::ink.withAlpha (0.4f));
    g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);
}

// ============================================================ MorphPad

MorphPad::MorphPad (AddSynthProcessor& p)
    : proc (p),
      px (*p.apvts.getParameter ("morphX")),
      py (*p.apvts.getParameter ("morphY"))
{
    startTimerHz (30);
}

juce::Point<float> MorphPad::toPixels (float x, float y) const
{
    auto r = getLocalBounds().toFloat().reduced (nodeRadius + 2);
    return { r.getX() + x * r.getWidth(),
             r.getY() + (1.0f - y) * r.getHeight() };
}

juce::Point<float> MorphPad::toNorm (juce::Point<float> p) const
{
    auto r = getLocalBounds().toFloat().reduced (nodeRadius + 2);
    return { juce::jlimit (0.0f, 1.0f, (p.x - r.getX()) / r.getWidth()),
             juce::jlimit (0.0f, 1.0f, 1.0f - (p.y - r.getY()) / r.getHeight()) };
}

int MorphPad::hitNode (juce::Point<float> pos) const
{
    auto& slots = proc.getSlots();
    for (int i = (int) slots.size() - 1; i >= 0; --i)
        if (toPixels (slots[(size_t) i].x, slots[(size_t) i].y)
                .getDistanceFrom (pos) <= nodeRadius + 3)
            return i;
    return -1;
}

void MorphPad::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (theme::padBg);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    for (int i = 1; i < 4; ++i)
    {
        g.drawVerticalLine ((int) (r.getWidth() * i / 4.0f), 6.0f, r.getHeight() - 6.0f);
        g.drawHorizontalLine ((int) (r.getHeight() * i / 4.0f), 6.0f, r.getWidth() - 6.0f);
    }
    g.setColour (theme::ink.withAlpha (0.25f));
    g.drawRoundedRectangle (r.reduced (0.5f), 10.0f, 1.0f);

    auto& slots = proc.getSlots();
    auto cursor = toPixels (px.getValue(), py.getValue());

    // weight visualization: line thickness ~ influence on the cursor
    if (! slots.empty())
    {
        float wsum = 0, w[64];
        for (size_t i = 0; i < slots.size() && i < 64; ++i)
        {
            auto n = toPixels (slots[i].x, slots[i].y);
            auto c = toNorm (cursor), nn = toNorm (n);
            auto dx = c.x - nn.x, dy = c.y - nn.y;
            w[i] = 1.0f / (dx * dx + dy * dy + 1e-4f);
            wsum += w[i];
        }
        for (size_t i = 0; i < slots.size() && i < 64; ++i)
        {
            auto n = toPixels (slots[i].x, slots[i].y);
            float wi = w[i] / wsum;
            g.setColour (juce::Colours::white.withAlpha (0.25f + 0.55f * wi));
            g.drawLine ({ n, cursor }, 1.0f + 3.5f * wi);
        }
    }

    // model nodes: colored dots with white rims, soothe-style
    for (size_t i = 0; i < slots.size(); ++i)
    {
        auto c = toPixels (slots[i].x, slots[i].y);
        float nr = 8.0f;
        g.setColour (theme::ink.withAlpha (0.2f));
        g.fillEllipse (c.x - nr, c.y - nr + 1.5f, nr * 2, nr * 2);
        g.setColour (theme::node ((int) i));
        g.fillEllipse (c.x - nr, c.y - nr, nr * 2, nr * 2);
        g.setColour (juce::Colours::white);
        g.drawEllipse (c.x - nr, c.y - nr, nr * 2, nr * 2, 2.0f);

        g.setColour (theme::ink.withAlpha (0.8f));
        g.setFont (theme::font (11.0f));
        g.drawText (slots[i].name.substring (0, 14),
                    (int) (c.x - 50), (int) (c.y + nr + 3),
                    100, 13, juce::Justification::centred);
    }

    // ghost cursors: where each active voice actually sits after spread
    g.setColour (theme::ink.withAlpha (0.55f));
    for (auto& [vx, vy] : proc.getActiveVoiceCursors())
    {
        auto c = toPixels (vx, vy);
        g.drawEllipse (c.x - 5, c.y - 5, 10, 10, 1.5f);
    }

    // morph cursor: white dot with ink ring
    g.setColour (theme::ink.withAlpha (0.25f));
    g.fillEllipse (cursor.x - 8, cursor.y - 6.5f, 16, 16);
    g.setColour (juce::Colours::white);
    g.fillEllipse (cursor.x - 8, cursor.y - 8, 16, 16);
    g.setColour (theme::ink);
    g.drawEllipse (cursor.x - 8, cursor.y - 8, 16, 16, 1.6f);

    if (slots.empty())
    {
        g.setColour (theme::ink.withAlpha (0.5f));
        g.setFont (theme::font (14.0f));
        g.drawText ("add models, drag them around, move the circle to morph",
                    getLocalBounds(), juce::Justification::centred);
    }
}

void MorphPad::sendCursor (juce::Point<float> pos, bool gestureStart)
{
    auto n = toNorm (pos);
    if (gestureStart) { px.beginChangeGesture(); py.beginChangeGesture(); }
    px.setValueNotifyingHost (n.x);
    py.setValueNotifyingHost (n.y);
}

void MorphPad::mouseDown (const juce::MouseEvent& e)
{
    draggingNode = hitNode (e.position);
    if (draggingNode < 0)
    {
        draggingCursor = true;
        sendCursor (e.position, true);
    }
}

void MorphPad::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingNode >= 0)
    {
        auto n = toNorm (e.position);
        proc.moveSlot (draggingNode, n.x, n.y);
        repaint();
    }
    else if (draggingCursor)
        sendCursor (e.position, false);
}

void MorphPad::mouseUp (const juce::MouseEvent&)
{
    if (draggingCursor) { px.endChangeGesture(); py.endChangeGesture(); }
    draggingNode = -1;
    draggingCursor = false;
}

void MorphPad::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto i = hitNode (e.position);
    if (i >= 0)
    {
        proc.removeSlot (i);
        repaint();
    }
}

// ======================================================== ScaleKeyboard

// white-key order C D E F G A B -> pitch classes, and black keys between
static const int whitePCs[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const int blackPCs[7] = { 1, 3, -1, 6, 8, 10, -1 }; // after white i

ScaleKeyboard::ScaleKeyboard (AddSynthProcessor& p) : proc (p)
{
    startTimerHz (15);
}

juce::Rectangle<float> ScaleKeyboard::whiteKeyRect (int wi) const
{
    auto r = getLocalBounds().toFloat();
    float w = r.getWidth() / 7.0f;
    return { r.getX() + wi * w, r.getY(), w - 1.0f, r.getHeight() };
}

juce::Rectangle<float> ScaleKeyboard::blackKeyRect (int wi) const
{
    auto white = whiteKeyRect (wi);
    float w = white.getWidth() * 0.62f;
    return { white.getRight() - w * 0.5f, white.getY(),
             w, white.getHeight() * 0.6f };
}

int ScaleKeyboard::hitKey (juce::Point<float> pos) const
{
    for (int wi = 0; wi < 7; ++wi)
        if (blackPCs[wi] >= 0 && blackKeyRect (wi).contains (pos))
            return blackPCs[wi];
    for (int wi = 0; wi < 7; ++wi)
        if (whiteKeyRect (wi).contains (pos))
            return whitePCs[wi];
    return -1;
}

void ScaleKeyboard::paint (juce::Graphics& g)
{
    auto isOn = [this] (int pc)
    {
        return proc.apvts.getRawParameterValue ("key" + juce::String (pc))
                   ->load() > 0.5f;
    };
    static const char* names[12] = { "c", "c#", "d", "d#", "e", "f",
                                     "f#", "g", "g#", "a", "a#", "b" };
    g.setFont (theme::font (10.0f));
    for (int wi = 0; wi < 7; ++wi)
    {
        auto r = whiteKeyRect (wi);
        int pc = whitePCs[wi];
        g.setColour (isOn (pc) ? theme::accent : theme::pill);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (theme::ink.withAlpha (0.3f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (isOn (pc) ? juce::Colours::white : theme::ink);
        g.drawText (names[pc], r.removeFromBottom (14),
                    juce::Justification::centred);
    }
    for (int wi = 0; wi < 7; ++wi)
    {
        int pc = blackPCs[wi];
        if (pc < 0) continue;
        auto r = blackKeyRect (wi);
        g.setColour (isOn (pc) ? theme::accent : theme::ink);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (isOn (pc) ? 1.0f : 0.0f));
        g.drawText (names[pc], r, juce::Justification::centred);
    }
}

void ScaleKeyboard::mouseDown (const juce::MouseEvent& e)
{
    int pc = hitKey (e.position);
    if (pc < 0) return;
    if (auto* param = proc.apvts.getParameter ("key" + juce::String (pc)))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->getValue() > 0.5f ? 0.0f : 1.0f);
        param->endChangeGesture();
    }
    repaint();
}

// ======================================================== AddSynthEditor

AddSynthEditor::AddSynthEditor (AddSynthProcessor& p)
    : AudioProcessorEditor (p), proc (p), pad (p), keyboard (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (pad);
    addAndMakeVisible (keyboard);

    addAndMakeVisible (addButton);
    addButton.setButtonText ("add models...");
    addButton.onClick = [this] { openModelChooser(); };

    addAndMakeVisible (hint);
    hint.setFont (theme::font (11.5f));
    hint.setColour (juce::Label::textColourId, theme::ink.withAlpha (0.55f));
    hint.setText ("drag nodes to place, double-click to remove\n"
                  "drag circle to morph\n"
                  "keys below the pad: pitch-envelope scale",
                  juce::dontSendNotification);
    hint.setJustificationType (juce::Justification::topLeft);

    static const std::pair<const char*, const char*> knobDefs[] = {
        { "gain", "gain" },       { "noise", "noise" },     { "speed", "speed" },
        { "tilt", "tilt" },       { "oddeven", "odd/even" },{ "stretch", "stretch" },
        { "partials", "partials" },{ "blur", "blur" },      { "drift", "drift" },
        { "attack", "attack" },   { "release", "release" }, { "pitchenv", "pitch env" },
        { "spreadx", "spread x" },{ "spready", "spread y" },{ "spreadn", "spread n" },
        { "bend", "bend" },       { "width", "width" },
    };
    for (auto& [id, title] : knobDefs)
    {
        auto k = std::make_unique<Knob>();
        k->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k->slider.setColour (juce::Slider::textBoxTextColourId, theme::ink);
        k->slider.setColour (juce::Slider::textBoxBackgroundColourId, theme::pill);
        k->slider.setColour (juce::Slider::textBoxOutlineColourId,
                             juce::Colours::transparentBlack);
        k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 58, 15);
        addAndMakeVisible (k->slider);
        k->label.setText (title, juce::dontSendNotification);
        k->label.setFont (theme::font (12.0f));
        k->label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (k->label);
        k->attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            proc.apvts, id, k->slider);
        knobs.push_back (std::move (k));
    }

    setSize (860, 540);
}

AddSynthEditor::~AddSynthEditor()
{
    setLookAndFeel (nullptr);
}

void AddSynthEditor::openModelChooser()
{
    auto start = proc.getLastModelDir().isDirectory()
               ? proc.getLastModelDir()
               : juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Add additive models",
                                                   start, "*.addm");
    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::canSelectMultipleItems,
        [this] (const juce::FileChooser& fc)
        {
            // spread new nodes over preset spots
            static const float spots[][2] = {
                { 0.15f, 0.85f }, { 0.85f, 0.85f }, { 0.15f, 0.15f },
                { 0.85f, 0.15f }, { 0.5f, 0.9f },  { 0.1f, 0.5f },
                { 0.9f, 0.5f },  { 0.5f, 0.1f },  { 0.35f, 0.65f },
                { 0.65f, 0.65f }, { 0.35f, 0.35f }, { 0.65f, 0.35f } };
            for (auto& f : fc.getResults())
            {
                auto i = proc.getSlots().size() % (sizeof (spots) / sizeof (spots[0]));
                juce::String err;
                if (! proc.addModel (f, spots[i][0], spots[i][1], err))
                    hint.setText ("load failed: " + err, juce::dontSendNotification);
            }
            pad.repaint();
        });
}

void AddSynthEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);

    // side panel behind the knobs
    g.setColour (theme::panel);
    g.fillRoundedRectangle (sidePanelArea.toFloat(), 10.0f);
    g.setColour (theme::ink.withAlpha (0.15f));
    g.drawRoundedRectangle (sidePanelArea.toFloat().reduced (0.5f), 10.0f, 1.0f);

    // header: lowercase wordmark, soothe-style
    g.setColour (theme::ink);
    g.setFont (theme::font (20.0f, true));
    g.drawText ("addsynth", 16, 6, 200, 24, juce::Justification::centredLeft);
    g.setColour (theme::ink.withAlpha (0.5f));
    g.setFont (theme::font (12.0f));
    g.drawText ("additive morph synth", 118, 10, 220, 18,
                juce::Justification::centredLeft);
}

void AddSynthEditor::resized()
{
    auto r = getLocalBounds().reduced (14);
    r.removeFromTop (28);

    // left column: pad with the scale keyboard underneath
    auto left = r.removeFromLeft (r.getHeight() - 58);
    keyboard.setBounds (left.removeFromBottom (52));
    left.removeFromBottom (6);
    pad.setBounds (left);

    r.removeFromLeft (12);
    sidePanelArea = r;
    auto side = r.reduced (12, 10);
    addButton.setBounds (side.removeFromTop (26).removeFromLeft (150));
    side.removeFromTop (2);
    hint.setBounds (side.removeFromTop (44));
    side.removeFromTop (2);

    const int cols = 4, rows = 5;
    auto cellW = side.getWidth() / cols;
    auto cellH = side.getHeight() / rows;
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        auto cell = juce::Rectangle<int> (
            side.getX() + (int) (i % cols) * cellW,
            side.getY() + (int) (i / cols) * cellH,
            cellW, cellH).reduced (2);
        knobs[i]->label.setBounds (cell.removeFromTop (14));
        knobs[i]->slider.setBounds (cell);
    }
}
