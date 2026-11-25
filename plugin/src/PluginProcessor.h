#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "AddmModel.h"
#include "MorphField.h"
#include "Voice.h"
#include "SpectralPlanes.h"
#include "AudioModelImporter.h"
#include <deque>

class AddSynthProcessor : public juce::AudioProcessor, private juce::Thread
{
public:
    AddSynthProcessor();
    ~AddSynthProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override
    {
       #if ADDSYNTH_V2
        return "AddSynth Sine Lab 2";
       #else
        return "AddSynth Sine Lab";
       #endif
    }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 120.0; }

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
        MorphField::Playback playback;
        std::shared_ptr<const juce::MemoryBlock> embeddedModel;
        MorphField::Shape shape;
    };

    bool addModel (const juce::File& f, float x, float y, juce::String& error);
    void removeSlot (int index);
    void moveSlot (int index, float x, float y);
    void setSlotPlayback (int index, MorphField::Playback);
    void setSlotShape (int index, MorphField::Shape);
    void selectSlot (int index);
    int getSelectedSlot() const { return selectedSlot.load(); }
    std::vector<Slot> getSlots() const;
    juce::File getLastModelDir() const;

    struct ImportStatus
    {
        juce::String message;
        float progress = 0.0f;
        bool busy = false;
    };
    static bool canImportFile (const juce::File& file);
    static juce::String importWildcards();
    void importFiles (const juce::StringArray&, juce::Point<float> position = { -1.0f, -1.0f });
    void cancelImports();
    ImportStatus getImportStatus() const;

    // effective pad positions of active voices (for the pad's ghost cursors)
    std::vector<std::pair<float, float>> getActiveVoiceCursors() const;

    // randomize mod matrix + sound parameters (message thread; gain,
    // morph cursor, scale keys and sync toggles are left alone)
    void randomizeParams();

    // MIDI scale mode state for the keyboard UI
    bool isMidiScaleMode() const
    {
        return apvts.getRawParameterValue ("midiscale")->load() > 0.5f;
    }
    int getHeldMask() const { return heldMaskDisplay.load(); }

    static juce::String partialGainID (int index)
    {
        return "partialGain" + juce::String (index + 1);
    }

    sineLab::Sequence sequence;
    int getSequenceStep() const { return sequenceStep.load(); }
    void clearMemory() { clearMemoryRequested.store (true); }
    void setLabPreset (bool ambient);
    void setPlanesPreset();
    bool capturePlane (int index, juce::String& message);
    std::array<sineLab::SpectralPlane, 2> getPlanes() const;
    juce::String getPlaneName (int index) const;
    float getPlanePhase() const { return planePhaseDisplay.load(); }
    float getPlaneLevel (int index) const { return planeDisplay[(size_t) index].load(); }
    void sequenceEdited() { updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true)); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void rebuildField();
    bool addPreparedModel (const juce::File&, std::shared_ptr<const AddmModel>,
                           std::shared_ptr<const juce::MemoryBlock>, float x, float y,
                           juce::String& error, bool rebuild = true);
    void run() override;
    void handleAsyncUpdate();

    struct ImportRequest
    {
        juce::File file;
        juce::Point<float> position;
        uint64_t generation = 0;
    };
    struct ImportCompletion
    {
        ImportRequest request;
        std::shared_ptr<AddmModel> model;
        std::shared_ptr<juce::MemoryBlock> embeddedModel;
        juce::String error, note;
    };
    mutable juce::CriticalSection importLock;
    std::deque<ImportRequest> importQueue;
    std::vector<ImportCompletion> importCompletions;
    std::atomic<uint64_t> importGeneration { 0 };
    int pendingImports = 0, importedCount = 0;
    juce::StringArray importErrors, importNotes;
    ImportStatus importStatus;
    juce::LockingAsyncUpdater importUpdater { [this] { handleAsyncUpdate(); } };

    juce::Synthesiser synth;
    sineLab::Memory spectralMemory;
    sineLab::SineOutput sineOutput;
    juce::SmoothedValue<float> outputGain;
    juce::AudioBuffer<float> highAudio { 2, 32 * sineLab::SineOutput::factor };
    juce::MidiBuffer highMidi;
    double sequenceBeat = 0.0;
    mutable juce::SpinLock planesLock;
    std::array<sineLab::SpectralPlane, 2> planes { sineLab::defaultPlanes() };
    std::array<sineLab::SpectralPlane, 2> audioPlanes { sineLab::defaultPlanes() };
    std::array<juce::String, 2> planeNames { "harmonic ribbons", "spectral ripples" };
    sineLab::Mask planeSmooth {};
    std::array<std::atomic<float>, sineLab::partials> planeDisplay {};
    double planePhase = 0.0;
    std::atomic<float> planePhaseDisplay { 0.0f };
    std::atomic<int> sequenceStep { -1 };
    std::atomic<bool> clearMemoryRequested { false };
    std::array<std::atomic<float>*, AddVoice::maxPartials> partialGainValues {};
    std::atomic<int> noteCounter { 0 };
    double lfoPhase1 = 0.0, lfoPhase2 = 0.25;

    // MIDI scale mode: pitch classes of currently held notes (audio
    // thread tracks; UI reads the atomic mask for display)
    int heldCount[12] {};
    std::atomic<int> heldMaskDisplay { 0 };

    std::vector<Slot> slots;
    mutable juce::CriticalSection slotsLock;
    std::atomic<int> selectedSlot { -1 };
    juce::File lastModelDir;

    juce::SpinLock fieldLock;
    std::shared_ptr<const MorphField> field { MorphField::build ({}) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AddSynthProcessor)
};
