// Renders the plugin editor offscreen to a PNG (UI review without a DAW).
// usage: ui_snapshot [model1.addm model2.addm ...] out.png
#include <juce_gui_basics/juce_gui_basics.h>
#include "../src/PluginProcessor.h"
#include "../src/PluginEditor.h"

int main (int argc, char** argv)
{
    if (argc < 2) { std::cerr << "usage: ui_snapshot [models...] out.png\n"; return 2; }
    juce::ScopedJuceInitialiser_GUI init;

    AddSynthProcessor proc;
    proc.prepareToPlay (44100.0, 512);

    static const float spots[][2] = { { 0.2f, 0.72f }, { 0.78f, 0.8f },
                                      { 0.5f, 0.25f }, { 0.85f, 0.3f } };
    int mi = 0;
    for (int i = 1; i < argc - 1 && mi < 4; ++i)
    {
        juce::String err;
        auto f = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i]);
        if (proc.addModel (f, spots[mi][0], spots[mi][1], err))
            ++mi;
        else
            std::cerr << "skip " << argv[i] << ": " << err << "\n";
    }

    // dress the UI a little: C minor pentatonic + cursor off-center
    for (int pc : { 0, 3, 5, 7, 10 })
        proc.apvts.getParameter ("key" + juce::String (pc))
            ->setValueNotifyingHost (1.0f);
    proc.apvts.getParameter ("morphX")->setValueNotifyingHost (0.42f);
    proc.apvts.getParameter ("morphY")->setValueNotifyingHost (0.58f);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
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
