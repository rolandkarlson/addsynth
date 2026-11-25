// Renders the plugin editor offscreen to a PNG (UI review without a DAW).
// usage: ui_snapshot [--glitch|--ambient|--planes] [--page n] [--model-view n] [--size w h]
//        [--render out.wav] [models...] out.png
#include <juce_gui_basics/juce_gui_basics.h>
#include "../src/PluginProcessor.h"
#include "../src/PluginEditor.h"

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: ui_snapshot [models...] out.png\n"; return 2; }
    juce::ScopedJuceInitialiser_GUI init;

    AddSynthProcessor proc;
    proc.setRateAndBufferSizeDetails (44100.0, 512);
    proc.prepareToPlay (44100.0, 512);
    int preset = -1;
    int page = 0, width = 0, height = 0, modelView = 0;
    juce::String audioPath;

    static const float spots[][2] = { { 0.2f, 0.72f }, { 0.78f, 0.8f },
                                      { 0.5f, 0.25f }, { 0.85f, 0.3f } };
    int mi = 0;
    for (int i = 1; i < argc - 1; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--glitch") { preset = 0; continue; }
        if (arg == "--ambient") { preset = 1; continue; }
        if (arg == "--planes") { preset = 2; continue; }
        if (arg == "--render" && i + 1 < argc - 1) { audioPath = argv[++i]; continue; }
        if (arg == "--page" && i + 1 < argc - 1) { page = juce::String (argv[++i]).getIntValue(); continue; }
        if (arg == "--model-view" && i + 1 < argc - 1) { modelView = juce::String (argv[++i]).getIntValue(); continue; }
        if (arg == "--size" && i + 2 < argc - 1)
        { width = juce::String (argv[++i]).getIntValue(); height = juce::String (argv[++i]).getIntValue(); continue; }
        if (mi >= 4) continue;
        auto f = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i]);
        if (AddSynthProcessor::canImportFile (f))
        {
            proc.importFiles ({ f.getFullPathName() }, { spots[mi][0], spots[mi][1] });
            ++mi;
        }
        else
            std::cerr << "skip " << argv[i] << ": unsupported file type\n";
    }
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000;
    while (proc.getImportStatus().busy && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
    if (proc.getImportStatus().busy || (mi > 0 && proc.getSlots().empty()))
    {
        std::cerr << "import failed: " << proc.getImportStatus().message << '\n';
        return 1;
    }

    // dress the UI a little: C minor pentatonic + cursor off-center
    for (int pc : { 0, 3, 5, 7, 10 })
        proc.apvts.getParameter ("key" + juce::String (pc))
            ->setValueNotifyingHost (1.0f);
    proc.apvts.getParameter ("morphX")->setValueNotifyingHost (0.42f);
    proc.apvts.getParameter ("morphY")->setValueNotifyingHost (0.58f);
    if (preset == 2) proc.setPlanesPreset();
    else if (preset >= 0) proc.setLabPreset (preset == 1);

    if (audioPath.isNotEmpty())
    {
        constexpr int sr = 44100, total = sr * 22;
        juce::AudioBuffer<float> audio (2, total);
        struct Event { int sample, note; bool on; };
        std::vector<Event> events;
        for (int phrase = 0; phrase < 2; ++phrase)
            for (int voice = 0; voice < 4; ++voice)
            {
                const int notes[] = { 48, 55, 60, 63 };
                const double start = phrase * 8.0 + voice * 0.3;
                const int note = notes[voice] - phrase * 5;
                events.push_back ({ (int) (start * sr), note, true });
                events.push_back ({ (int) ((start + 4.0) * sr), note, false });
            }
        const auto started = juce::Time::getMillisecondCounterHiRes();
        for (int offset = 0; offset < total; offset += 512)
        {
            const int count = juce::jmin (512, total - offset);
            juce::AudioBuffer<float> block (audio.getArrayOfWritePointers(), 2, offset, count);
            juce::MidiBuffer midi;
            for (const auto& event : events)
                if (event.sample >= offset && event.sample < offset + count)
                    midi.addEvent (event.on ? juce::MidiMessage::noteOn (1, event.note, 0.7f)
                                           : juce::MidiMessage::noteOff (1, event.note), event.sample - offset);
            proc.processBlock (block, midi);
        }
        const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - started) / 1000.0;
        const float peak = audio.getMagnitude (0, total);
        if (! std::isfinite (peak) || peak > 0.99f || peak < 1e-6f)
        {
            std::cerr << "demo audio invalid: peak=" << peak << '\n';
            return 1;
        }
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (audioPath);
        file.deleteFile();
        auto stream = file.createOutputStream();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (
            stream ? wav.createWriterFor (stream.release(), sr, 2, 24, {}, 0) : nullptr);
        if (writer == nullptr || ! writer->writeFromAudioSampleBuffer (audio, 0, total)) return 1;
        std::cout << "rendered 22 s in " << elapsed << " s; peak=" << peak << " -> " << file.getFullPathName() << '\n';
    }

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    if (width > 0 && height > 0) ed->setSize (width, height);
    for (auto* child : ed->getChildren())
        if (child->getComponentID() == "workspace-tab-" + juce::String (page))
            if (auto* button = dynamic_cast<juce::TextButton*> (child)) button->onClick();
    if (modelView > 0 && ! proc.getSlots().empty())
    {
        // Sculpt the first model so the per-model editors have something to show.
        MorphField::Shape shape;
        for (size_t i = 0; i < shape.envelope.size(); ++i)
            shape.envelope[i] = juce::jmin (1.0f, (float) i / 8.0f) * (1.0f - 0.8f * (float) i / 63.0f);
        for (size_t k = 0; k < shape.partials.size(); ++k)
            shape.partials[k] = k % 2 == 1 ? 0.35f : 1.0f - 0.5f * (float) k / 191.0f;
        proc.selectSlot (0);
        proc.setSlotShape (0, shape);
        if (auto* inspector = ed->findChildWithID ("model-inspector"))
            if (auto* button = dynamic_cast<juce::TextButton*> (
                    inspector->findChildWithID ("model-view-" + juce::String (modelView))))
                button->onClick();
    }
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);

    auto out = juce::File::getCurrentWorkingDirectory()
                   .getChildFile (argv[argc - 1]);
    out.deleteFile();
    juce::FileOutputStream os (out);
    juce::PNGImageFormat png;
    if (! os.openedOk() || ! png.writeImageToStream (img, os))
    {
        std::cerr << "failed to write " << out.getFullPathName() << "\n";
        return 1;
    }
    std::cout << "wrote " << out.getFullPathName() << " ("
              << img.getWidth() << "x" << img.getHeight() << ")\n";
    return 0;
}
