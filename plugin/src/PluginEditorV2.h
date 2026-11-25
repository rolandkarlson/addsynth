#pragma once
#include "PluginEditor.h"

// The inspector edits a pad slot, not the shared voice parameters: its playback
// markers, a level envelope over its timeline, and a level for each partial.
class ModelInspector : public juce::Component, private juce::Timer
{
public:
    enum View { playbackView, envelopeView, partialsView };
    explicit ModelInspector (AddSynthProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void refresh();
    void setView (int view);
    juce::Rectangle<float> waveBounds() const;   // plot area of the current view

private:
    void timerCallback() override { refresh(); }
    void apply();
    void buildWaveform (bool modelChanged);
    float synthesise (const std::array<float, MorphField::Shape::maxPartials>& levels);
    int shapeCount() const;
    int shapeIndexAt (float x) const;
    float levelAt (float y) const;
    float& shapeValue (int index);
    AddSynthProcessor& proc;
    std::shared_ptr<const AddmModel> model;
    int selected = -1, draggingMarker = -1, view = playbackView;
    bool refreshing = false, drawing = false;
    int hover = -1, previousIndex = -1;
    float previousLevel = 1.0f, rawPeak = 1.0f;
    MorphField::Playback playback;
    MorphField::Shape shape;
    std::array<float, MorphField::Shape::maxPartials> builtPartials = MorphField::Shape().partials;
    std::array<float, MorphField::Shape::maxPartials> spectrum {};
    std::array<float, 640> lows {}, highs {};
    std::array<juce::TextButton, 3> views;
    juce::Slider start, loopIn, loopOut;
    juce::Label startLabel, inLabel, outLabel;
    juce::ComboBox mode;
    juce::TextButton reset { "reset envelope" }, remove { "remove model" };
};

class AddSynthEditorV2 : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget
{
public:
    explicit AddSynthEditorV2 (AddSynthProcessor&);
    ~AddSynthEditorV2() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    struct ModRow
    {
        juce::ComboBox source, destination;
        juce::Slider amount;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> sourceA, destinationA;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> amountA;
    };
    void addKnob (const juce::String& id, const juce::String& label);
    void placeKnob (const juce::String& id, juce::Rectangle<int>);
    void openModels();
    AddLookAndFeel look;
    AddSynthProcessor& proc;
    MorphPad pad;
    ModelInspector inspector;
    ScaleKeyboard keyboard;
    PartialEditor partials;
    SineLabPanel lab;
    SpectralPlanesPanel planes;
    juce::TextButton add { "add sounds..." }, random { "randomize sound" };
    std::array<juce::TextButton, 3> tabs;
    int page = 0;
    std::map<juce::String, std::unique_ptr<Knob>> knobs;
    std::array<ModRow, AddVoice::Params::nModSlots> mods;
    juce::ToggleButton syncSpeed { "tempo sync" }, syncLoop { "loop sync" },
                       modal { "resonator" }, midiScale { "MIDI scale" };
    std::vector<std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>> toggles;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltip { this, 700 };
    juce::Rectangle<int> morphArea, soundArea, modArea, scaleArea;
    std::array<juce::Rectangle<int>, 3> soundGroups;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddSynthEditorV2)
};
