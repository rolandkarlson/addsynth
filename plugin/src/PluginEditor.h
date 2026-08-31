#pragma once
#include "PluginProcessor.h"

// soothe-inspired theme: powder blue field, charcoal knobs, paper pills,
// lowercase ink typography, colored node dots with white rims.
namespace theme
{
    const juce::Colour bg      { 0xffb7cbe7 };  // window field
    const juce::Colour panel   { 0xffaabfe0 };  // side panel
    const juce::Colour padBg   { 0xffd2e0f2 };  // pad surface
    const juce::Colour ink     { 0xff2c3240 };  // text / outlines
    const juce::Colour knobCol { 0xff3a4048 };  // knob body
    const juce::Colour pill    { 0xfff5f2ea };  // value pills / white keys
    const juce::Colour accent  { 0xff43b0c0 };  // enabled keys, highlights

    inline juce::Colour node (int i)
    {
        static const juce::Colour c[] = {
            juce::Colour (0xffe0526b), juce::Colour (0xff33b3c4),
            juce::Colour (0xffc95f9d), juce::Colour (0xff3fa170),
            juce::Colour (0xffe1954f), juce::Colour (0xff7a6fd0),
            juce::Colour (0xff4f7fd9), juce::Colour (0xffd4b13f) };
        return c[(size_t) i % 8];
    }

    inline juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions ("Avenir Next", size,
            bold ? juce::Font::bold : juce::Font::plain));
    }
}

class AddLookAndFeel : public juce::LookAndFeel_V4
{
public:
    AddLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h,
                           float pos, float startAngle, float endAngle,
                           juce::Slider&) override;
    void drawLabel (juce::Graphics&, juce::Label&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour&, bool, bool) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override
    {
        return theme::font (15.0f);
    }
};

// The XY morph pad: model nodes are draggable, double-click removes one,
// dragging anywhere else moves the morph cursor (morphX/morphY params).
class MorphPad : public juce::Component, private juce::Timer
{
public:
    explicit MorphPad (AddSynthProcessor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    void timerCallback() override { repaint(); }
    int hitNode (juce::Point<float> pos) const;
    juce::Point<float> toPixels (float x, float y) const;
    juce::Point<float> toNorm (juce::Point<float> px) const;
    void sendCursor (juce::Point<float> px, bool gestureStart);

    AddSynthProcessor& proc;
    juce::RangedAudioParameter& px;
    juce::RangedAudioParameter& py;
    int draggingNode = -1;
    bool draggingCursor = false;

    static constexpr float nodeRadius = 14.0f;
};

// One-octave keyboard for the pitch-envelope scale quantizer: click keys
// to toggle the enabled pitch classes (key0..key11 params). All off = bypass.
class ScaleKeyboard : public juce::Component, private juce::Timer
{
public:
    explicit ScaleKeyboard (AddSynthProcessor&);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    void timerCallback() override { repaint(); }
    int hitKey (juce::Point<float>) const;
    juce::Rectangle<float> whiteKeyRect (int whiteIndex) const;
    juce::Rectangle<float> blackKeyRect (int pc) const;

    AddSynthProcessor& proc;
};

class AddSynthEditor : public juce::AudioProcessorEditor
{
public:
    explicit AddSynthEditor (AddSynthProcessor&);
    ~AddSynthEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void openModelChooser();

    AddLookAndFeel lnf;   // declared first: outlives the components below
    juce::Rectangle<int> sidePanelArea;
    AddSynthProcessor& proc;

    MorphPad pad;
    ScaleKeyboard keyboard;
    juce::TextButton addButton { "Add models..." };
    juce::Label hint;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;
    };
    std::vector<std::unique_ptr<Knob>> knobs;

    // modulation matrix strip
    struct ModRow
    {
        juce::ComboBox src, dst;
        juce::Slider amt;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> srcA, dstA;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> amtA;
    };
    std::vector<std::unique_ptr<ModRow>> modRows;
    Knob lfo1Knob, lfo2Knob;
    juce::Rectangle<int> modPanelArea;

    juce::ToggleButton syncLoopBtn { "loop sync" }, syncSpeedBtn { "speed sync" },
                       midiScaleBtn { "midi scale" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        syncLoopA, syncSpeedA, midiScaleA;
    juce::TextButton randomButton { "randomize" };
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddSynthEditor)
};
