#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AddmModel.h"
#include "MorphField.h"
#include "Voice.h"

class AddSynthProcessor : public juce::AudioProcessor
{
public:
    AddSynthProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AddSynth"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- morph pad slots (message thread only) ----
    struct Slot
    {
        juce::String path, name;
        float x = 0.5f, y = 0.5f;
        std::shared_ptr<const AddmModel> model;
    };

    bool addModel (const juce::File& f, float x, float y, juce::String& error);
    void removeSlot (int index);
    void moveSlot (int index, float x, float y);
    const std::vector<Slot>& getSlots() const { return slots; }
    juce::File getLastModelDir() const { return lastModelDir; }

    // effective pad positions of active voices (for the pad's ghost cursors)
    std::vector<std::pair<float, float>> getActiveVoiceCursors() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void rebuildField();

    juce::Synthesiser synth;
    std::atomic<int> noteCounter { 0 };
    double lfoPhase1 = 0.0, lfoPhase2 = 0.25;

    std::vector<Slot> slots;
    juce::File lastModelDir;

    juce::SpinLock fieldLock;
    std::shared_ptr<const MorphField> field { MorphField::build ({}) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddSynthProcessor)
};
