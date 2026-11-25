#include "../src/PluginEditorV2.h"
#include <iostream>
#include <stdexcept>

namespace
{
void require (bool condition, const char* message)
{
    if (! condition) throw std::runtime_error (message);
}

struct Fixtures
{
    juce::File directory = juce::File::getCurrentWorkingDirectory()
        .getNonexistentChildFile ("audio-import-fixtures", "");
    Fixtures() { require (directory.createDirectory().wasOk(), "could not create fixture directory"); }
    ~Fixtures() { directory.deleteRecursively(); }

    juce::File tone (const juce::String& name)
    {
        auto file = directory.getChildFile (name);
        juce::AudioBuffer<float> samples (1, 22050);
        for (int i = 0; i < samples.getNumSamples(); ++i)
            samples.setSample (0, i, .25f * std::sin ((float) (juce::MathConstants<double>::twoPi * 220 * i / 44100.0)));
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            format.createWriterFor (file.createOutputStream().release(), 44100, 1, 24, {}, 0));
        require (writer != nullptr && writer->writeFromAudioSampleBuffer (samples, 0, samples.getNumSamples()),
                 "could not write audio fixture");
        return file;
    }
};

void pumpMessages (int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
}

void finishImport (AddSynthProcessor& processor)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
    while (processor.getImportStatus().busy && juce::Time::getMillisecondCounterHiRes() < deadline)
        pumpMessages (10);
    if (processor.getImportStatus().busy) throw std::runtime_error ("import timed out");
}

float renderNote (AddSynthProcessor& processor)
{
    processor.setRateAndBufferSizeDetails (44100, 256);
    processor.prepareToPlay (44100, 256);
    float peak = 0;
    for (int block = 0; block < 30; ++block)
    {
        juce::AudioBuffer<float> audio (2, 256);
        juce::MidiBuffer midi;
        if (block == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 57, .8f), 0);
        processor.processBlock (audio, midi);
        const float level = audio.getMagnitude (0, 256);
        require (std::isfinite (level) && level < 1, "imported model produced invalid audio");
        peak = juce::jmax (peak, level);
    }
    return peak;
}

void testDropAndRecall()
{
    Fixtures fixtures;
    const auto source = fixtures.tone ("drop tone.wav");
    AddSynthProcessor processor;
    {
        AddSynthEditorV2 editor (processor);
        const juce::StringArray files { source.getFullPathName() };
        require (editor.isInterestedInFileDrag (files), "editor must accept an audio drag");
        require (! editor.isInterestedInFileDrag ({ fixtures.directory.getChildFile ("notes.txt").getFullPathName() }),
                 "editor must reject unrelated files");
        auto* pad = dynamic_cast<MorphPad*> (editor.findChildWithID ("morph-pad"));
        require (pad != nullptr && pad->isInterestedInFileDrag (files), "pad must accept an audio drag");
        pad->filesDropped (files, 16 + (pad->getWidth() - 32) / 4, 16 + (pad->getHeight() - 32) / 4);
        require (processor.getImportStatus().busy, "drop must queue background analysis");
        // Closing a host editor must not cancel a processor-owned import.
    }
    finishImport (processor);
    auto slots = processor.getSlots();
    require (slots.size() == 1, "dropped audio did not create a model");
    require (std::abs (slots[0].x - .25f) < .01f && std::abs (slots[0].y - .75f) < .01f,
             "model must be placed at the pad drop position");
    require (processor.getSelectedSlot() == 0 && slots[0].embeddedModel != nullptr,
             "import must select and persist the new model");
    require (renderNote (processor) > .005f, "dropped sound must be playable");
    processor.setSlotPlayback (0, { .15f, .2f, .7f, true });
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    require (source.deleteFile(), "could not remove source audio for recall test");
    AddSynthProcessor restored;
    restored.setStateInformation (saved.getData(), (int) saved.getSize());
    auto recalled = restored.getSlots();
    require (recalled.size() == 1 && recalled[0].model->env == slots[0].model->env,
             "session must restore imported spectrum without original audio");
    require (recalled[0].playback.loop && std::abs (recalled[0].playback.start - .15f) < .001f,
             "imported model must retain its playback settings");
    require (renderNote (restored) > .005f, "recalled audio import must be playable");
}

void testQueueAndCancellation()
{
    Fixtures fixtures;
    const auto source = fixtures.tone ("queued.wav");
    const auto invalid = fixtures.directory.getChildFile ("broken.wav");
    require (invalid.replaceWithText ("broken audio"), "could not create corrupt audio fixture");
    AddSynthProcessor processor;
    processor.importFiles ({ invalid.getFullPathName(), source.getFullPathName() });
    finishImport (processor);
    require (processor.getSlots().size() == 1, "a failed import must not prevent the next file loading");
    require (processor.getImportStatus().message.contains ("broken.wav"), "failed file must have visible error");

    auto nativeModel = fixtures.directory.getChildFile ("existing.addm");
    {
        juce::FileOutputStream output (nativeModel);
        require (processor.getSlots()[0].model->write (output), "could not create model fixture");
    }
    processor.importFiles ({ source.getFullPathName(), nativeModel.getFullPathName() });
    finishImport (processor);
    const auto mixed = processor.getSlots();
    require (mixed.size() == 3 && mixed[1].embeddedModel != nullptr && mixed[2].embeddedModel == nullptr,
             "mixed audio and ADDM drops must preserve loading behavior");

    processor.importFiles ({ source.getFullPathName(), source.getFullPathName() });
    processor.cancelImports();
    pumpMessages (100);
    require (! processor.getImportStatus().busy && processor.getSlots().size() == 3,
             "cancelled imports must not add delayed models");
    require (processor.getImportStatus().message == "Import cancelled", "stale callbacks must preserve cancel feedback");
    processor.importFiles ({ source.getFullPathName() });
    finishImport (processor);
    require (processor.getSlots().size() == 4, "import must work after cancellation");

    juce::MemoryBlock emptyState;
    AddSynthProcessor empty;
    empty.getStateInformation (emptyState);
    processor.importFiles ({ source.getFullPathName() });
    processor.setStateInformation (emptyState.getData(), (int) emptyState.getSize());
    pumpMessages (100);
    require (processor.getSlots().empty() && ! processor.getImportStatus().busy,
             "restoring a session must discard previous pending imports");
    require (processor.getImportStatus().message.isEmpty(), "state recall must clear old import feedback");
    processor.importFiles ({ source.getFullPathName() });
    finishImport (processor);
    require (processor.getSlots().size() == 1, "import must work after state restoration");

    for (int i = 1; i < MorphField::maxLayers; ++i)
    {
        juce::String error;
        require (processor.addModel (nativeModel, .5f, .5f, error), "could not fill pad");
    }
    processor.importFiles ({ source.getFullPathName() });
    require (! processor.getImportStatus().busy && processor.getSlots().size() == 16
             && processor.getImportStatus().message.containsIgnoreCase ("full"),
             "full pad must reject imports with feedback");

    {
        AddSynthProcessor closing;
        closing.importFiles ({ source.getFullPathName() });
        // A worker may be decoding when the DAW deletes its processor.
    }
}
}

void testModelSerialization();
void testAudioImportAnalysis();

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    try
    {
        testModelSerialization();
        testAudioImportAnalysis();
        testDropAndRecall();
        testQueueAndCancellation();
        std::cout << "PASS: audio analysis, drag/drop, queue/cancel, playable resynthesis and embedded session recall\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
