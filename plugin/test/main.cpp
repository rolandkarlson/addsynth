// Headless smoke test for the morph-field voice engine.
// usage: addm_test model1.addm [model2.addm] [out.wav]
// With one model: plays it at cursor center. With two: places them at the
// pad's left/right edges and renders three notes at cursor x=0, 0.5, 1.
#include <juce_audio_utils/juce_audio_utils.h>
#include "../src/AddmModel.h"
#include "../src/MorphField.h"
#include "../src/Voice.h"

static std::shared_ptr<const AddmModel> loadOrDie (const char* arg)
{
    juce::String err;
    auto f = juce::File::getCurrentWorkingDirectory().getChildFile (arg);
    auto m = AddmModel::load (f, err);
    if (m == nullptr)
    {
        std::cerr << "load failed for " << arg << ": " << err << "\n";
        exit (1);
    }
    std::cout << "loaded '" << m->name << "': f0=" << m->f0Ref << " Hz, "
              << m->nFrames << " frames x " << m->nPartials << " partials\n";
    return m;
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: addm_test model1.addm [model2.addm] [out.wav]\n";
        return 2;
    }

    juce::String outPath;
    std::vector<std::shared_ptr<const AddmModel>> models;
    AddVoice::Params params;
    bool chord = false;
    double lfo1Hz = 0.0, lfo2Hz = 0.0;
    for (int i = 1; i < argc; ++i)
    {
        juce::String arg (argv[i]);
        if (arg.endsWith (".wav")) { outPath = arg; continue; }
        if (arg.contains ("="))
        {
            auto key = arg.upToFirstOccurrenceOf ("=", false, false);
            auto valStr = arg.fromFirstOccurrenceOf ("=", false, false);
            if (key.startsWith ("mod") && key.length() == 4)
            {
                int idx = key.substring (3).getIntValue();
                auto t = juce::StringArray::fromTokens (valStr, ",", "");
                if (idx >= 0 && idx < AddVoice::Params::nModSlots && t.size() == 3)
                {
                    params.modSrc[idx] = t[0].getIntValue();
                    params.modDst[idx] = t[1].getIntValue();
                    params.modAmt[idx] = t[2].getFloatValue();
                    continue;
                }
            }
            auto val = valStr.getFloatValue();
            if      (key == "chord")    chord = val > 0.5f;
            else if (key == "lfo1")     lfo1Hz = val;
            else if (key == "lfo2")     lfo2Hz = val;
            else if (key == "keys")     params.keyMask = (juce::uint16) val;
            else if (key == "bend")     params.bend = val;
            else if (key == "width")    params.width = val;
            else if (key == "loopstart") params.loopStart = val;
            else if (key == "looplen")  params.loopLen = val;
            else if (key == "modal")    params.modal = val > 0.5f;
            else if (key == "ring")     params.ring = val;
            else if (key == "damp")     params.damp = val;
            else if (key == "sustain")  params.sustain = val;
            else if (key == "decay")    params.decay = val;
            else if (key == "syncloop") params.syncLoop = val > 0.5f;
            else if (key == "syncspeed") params.syncSpeed = val > 0.5f;
            else if (key == "bpm")      params.bpm = val;
            else if (key == "spreadx")  params.spreadX = val;
            else if (key == "spready")  params.spreadY = val;
            else if (key == "spreadn")  params.spreadN = (int) val;
            else if (key == "noise")    params.noise = val;
            else if (key == "release")  params.release = val;
            else if (key == "attack")   params.attack = val;
            else if (key == "speed")    params.speed = val;
            else if (key == "blur")     params.blur = val;
            else if (key == "tilt")     params.tiltDbOct = val;
            else if (key == "oddeven")  params.oddEven = val;
            else if (key == "stretch")  params.stretchB = val;
            else if (key == "partials") params.partials = val;
            else if (key == "drift")    params.drift = val;
            else if (key == "pitchenv") params.pitchEnvAmt = val;
            else { std::cerr << "unknown param " << key << "\n"; return 2; }
            continue;
        }
        models.push_back (loadOrDie (argv[i]));
    }

    std::vector<MorphField::Placement> placements;
    if (models.size() == 1)
        placements.push_back ({ models[0], 0.5f, 0.5f });
    else
        for (size_t i = 0; i < models.size(); ++i)
            placements.push_back ({ models[i],
                (float) i / (float) (models.size() - 1), 0.5f });

    auto fieldPtr = MorphField::build (placements);
    std::cout << "field: " << fieldPtr->layers.size() << " layers, "
              << fieldPtr->nFrames << " frames x " << fieldPtr->nPartials
              << " partials\n";

    const double sr = 44100.0;
    std::atomic<int> noteCounter { 0 };
    juce::Synthesiser synth;
    synth.addSound (new AddSound());
    std::vector<AddVoice*> voices;
    for (int i = 0; i < 6; ++i)
    {
        auto* v = new AddVoice();
        v->setNoteCounter (&noteCounter);
        synth.addVoice (v);
        voices.push_back (v);
    }
    synth.setCurrentPlaybackSampleRate (sr);

    // event schedule: (time, midiNote, isOn)
    struct Ev { double t; int note; bool on; };
    std::vector<Ev> evs;
    int notes = 1;
    if (chord)
    {
        // staggered chord so successive voices pick up spread offsets
        const int chordNotes[] = { 48, 55, 60, 64, 67 };
        for (int k = 0; k < 5; ++k)
        {
            evs.push_back ({ 0.2 + k * 0.4, chordNotes[k], true });
            evs.push_back ({ 5.0, chordNotes[k], false });
        }
    }
    else
    {
        notes = models.size() > 1 ? 3 : 1;
        for (int k = 0; k < notes; ++k)
        {
            evs.push_back ({ k * 2.5, 60, true });
            evs.push_back ({ k * 2.5 + 2.0, 60, false });
        }
    }
    double endT = 0;
    for (auto& e : evs) endT = std::max (endT, e.t);
    const int total = (int) (sr * (endT + 1.5)), block = 512;
    juce::AudioBuffer<float> out (2, total);
    out.clear();

    for (int pos = 0; pos < total; pos += block)
    {
        int n = std::min (block, total - pos);
        double tBlockStart = pos / sr;

        auto p = params;
        p.lfo1 = (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                   * lfo1Hz * tBlockStart);
        p.lfo2 = (float) std::sin (2.0 * juce::MathConstants<double>::pi
                                   * lfo2Hz * tBlockStart);
        // sequential-notes mode steps the cursor left->right per note
        int noteIdx = (int) (tBlockStart / 2.5);
        p.cursorX = (chord || notes == 1) ? 0.5f
                  : (float) juce::jmin (noteIdx, notes - 1) / (float) (notes - 1);
        p.cursorY = 0.5f;
        for (auto* v : voices)
        {
            v->setField (fieldPtr);
            v->setParams (p);
        }

        juce::MidiBuffer slice;
        for (auto& e : evs)
            if (e.t >= tBlockStart && e.t < tBlockStart + n / sr)
                slice.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.note, 0.8f)
                                     : juce::MidiMessage::noteOff (1, e.note),
                                (int) ((e.t - tBlockStart) * sr));
        juce::AudioBuffer<float> sub (out.getArrayOfWritePointers(), 2, pos, n);
        synth.renderNextBlock (sub, slice, 0, n);
    }

    float peak = out.getMagnitude (0, 0, total);
    float rms = out.getRMSLevel (0, 0, total);
    std::cout << "rendered " << total / sr << " s: peak=" << peak
              << " rms=" << rms << "\n";

    if (outPath.isNotEmpty())
    {
        auto f = juce::File::getCurrentWorkingDirectory().getChildFile (outPath);
        f.deleteFile();
        juce::WavAudioFormat wav;
        if (auto stream = f.createOutputStream())
        {
            std::unique_ptr<juce::AudioFormatWriter> w (
                wav.createWriterFor (stream.release(), sr, 2, 16, {}, 0));
            if (w != nullptr)
            {
                w->writeFromAudioSampleBuffer (out, 0, total);
                std::cout << "wrote " << f.getFullPathName() << "\n";
            }
        }
    }

    if (peak <= 1e-5f) { std::cerr << "FAIL: silence\n"; return 1; }
    if (peak > 4.0f)   { std::cerr << "FAIL: blowing up\n"; return 1; }
    std::cout << "OK\n";
    return 0;
}
