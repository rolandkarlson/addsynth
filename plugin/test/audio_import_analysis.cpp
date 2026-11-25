#include "../src/AudioModelImporter.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void checkImport (bool condition, const char* message)
{
    if (! condition) throw std::runtime_error (message);
}

struct AudioFixture
{
    explicit AudioFixture (const juce::String& extension = ".wav")
        : file (juce::File::getCurrentWorkingDirectory()
                    .getNonexistentChildFile ("audio-import-test", extension)) {}

    ~AudioFixture() { file.deleteFile(); }

    void write (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        juce::WavAudioFormat wav;
        auto stream = file.createOutputStream();
        checkImport (stream != nullptr, "cannot create audio import fixture");
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (
            stream.get(), sampleRate, (unsigned int) audio.getNumChannels(), 24, {}, 0));
        checkImport (writer != nullptr, "cannot create WAV fixture writer");
        stream.release();
        checkImport (writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()),
                     "cannot write audio import fixture");
    }

    juce::File file;
};

juce::AudioBuffer<float> sine (int channels, int sampleRate, double seconds,
                                double frequency, bool antiPhase = false)
{
    juce::AudioBuffer<float> result (channels, (int) (seconds * sampleRate));
    for (int i = 0; i < result.getNumSamples(); ++i)
    {
        const float sample = .4f * (float) std::sin (
            juce::MathConstants<double>::twoPi * frequency * i / sampleRate);
        for (int channel = 0; channel < channels; ++channel)
            result.setSample (channel, i, antiPhase && channel == 1 ? -sample : sample);
    }
    return result;
}

void checkModel (const AddmModel& model)
{
    checkImport (model.nFrames >= 2 && model.nPartials >= 1 && model.nPartials <= 192,
                 "imported model dimensions must be playable");
    checkImport (model.env.size() == (size_t) model.nFrames * (size_t) model.nPartials,
                 "imported envelope dimensions do not match the model");
    checkImport (model.f0Track.size() == (size_t) model.nFrames
              && model.detune.size() == (size_t) model.nPartials,
                 "imported pitch and detune arrays must match the model");
    checkImport (std::isfinite (model.f0Ref) && model.f0Ref > 0
              && std::isfinite (model.controlRate) && model.controlRate > 0,
                 "imported pitch and control rate must be valid");
    for (float amplitude : model.env)
        checkImport (std::isfinite (amplitude) && amplitude >= 0,
                     "imported partial amplitudes must be finite and nonnegative");
    for (float pitch : model.f0Track)
        checkImport (std::isfinite (pitch) && pitch > 0, "imported pitch track must be finite and positive");
    for (float detune : model.detune)
        checkImport (std::isfinite (detune) && detune > 0, "imported detune must be finite and positive");
}

float averageFundamental (const AddmModel& model, float begin, float end)
{
    const int first = (int) (begin * (float) model.nFrames);
    const int last = juce::jmax (first + 1, (int) (end * (float) model.nFrames));
    float total = 0;
    for (int frame = first; frame < last; ++frame) total += model.envAt (frame, 0);
    return total / (float) (last - first);
}

void testPitchedAudio()
{
    AudioFixture mono;
    mono.write (sine (1, 44100, .6, 440), 44100);
    checkImport (audioImport::supports (mono.file), "WAV files must be accepted for audio import");
    checkImport (audioImport::supportedWildcards().containsIgnoreCase ("*.wav"),
                 "file chooser must advertise WAV support");
    float previousProgress = -1;
    int notifications = 0;
    const auto result = audioImport::analyze (mono.file, [&] (float progress)
    {
        checkImport (std::isfinite (progress) && progress >= previousProgress
                      && progress >= 0 && progress <= 1,
                     "audio import progress must be finite, bounded and monotonic");
        previousProgress = progress;
        ++notifications;
        return true;
    });
    checkImport (result.model != nullptr && result.error.isEmpty(), "pitched WAV must produce a model");
    checkModel (*result.model);
    checkImport (std::abs (result.model->f0Ref - 440.0f) < 8.0f,
                 "440 Hz sine must import near its fundamental pitch");
    const float level = averageFundamental (*result.model, .25f, .75f);
    checkImport (level > .2f && level < .65f,
                 "sine import must preserve a useful fundamental amplitude");
    checkImport (notifications > 1 && previousProgress > .95f, "completed import must report progress");

    AudioFixture stereo;
    stereo.write (sine (2, 48000, .6, 440, true), 48000);
    const auto stereoResult = audioImport::analyze (stereo.file, {});
    checkImport (stereoResult.model != nullptr, "opposite-phase stereo channels must not cancel the import");
    checkModel (*stereoResult.model);
    checkImport (std::abs (stereoResult.model->f0Ref - 440.0f) < 8.0f,
                 "stereo import must preserve pitch when resampling");
    checkImport (averageFundamental (*stereoResult.model, .25f, .75f) > level * .5f,
                 "opposite-phase stereo import must retain audible signal");

    AudioFixture harmonics;
    auto harmonicAudio = sine (1, 44100, .6, 220);
    for (int i = 0; i < harmonicAudio.getNumSamples(); ++i)
    {
        const double phase = juce::MathConstants<double>::twoPi * 220 * i / 44100.0;
        harmonicAudio.setSample (0, i, (float) (.1 * std::sin (phase)
            + .3 * std::sin (2 * phase) + .2 * std::sin (3 * phase)));
    }
    harmonics.write (harmonicAudio, 44100);
    const auto harmonicResult = audioImport::analyze (harmonics.file, {});
    checkImport (harmonicResult.model != nullptr, "harmonic tone must import");
    checkImport (std::abs (harmonicResult.model->f0Ref - 220.0f) < 6.0f,
                 "strong second harmonic must not double the imported root pitch");
}

void testChangingEnvelope()
{
    AudioFixture fixture;
    auto audio = sine (1, 44100, 1.0, 440);
    for (int i = 0; i < audio.getNumSamples() / 2; ++i)
        audio.setSample (0, i, audio.getSample (0, i) * .125f);
    fixture.write (audio, 44100);
    const auto result = audioImport::analyze (fixture.file, {});
    checkImport (result.model != nullptr, "audio with changing amplitude must import");
    const auto& model = *result.model;
    checkImport (averageFundamental (model, .65f, .85f)
                    > 4.0f * averageFundamental (model, .15f, .35f),
                 "imported partial envelopes must follow the source dynamics");
}

void testUnpitchedAndShortAudio()
{
    AudioFixture noise;
    juce::AudioBuffer<float> noiseAudio (1, 22050);
    juce::Random random (0x5add1);
    for (int i = 0; i < noiseAudio.getNumSamples(); ++i)
        noiseAudio.setSample (0, i, .3f * (2.0f * random.nextFloat() - 1.0f));
    noise.write (noiseAudio, 44100);
    const auto noiseResult = audioImport::analyze (noise.file, {});
    checkImport (noiseResult.model != nullptr && noiseResult.error.isEmpty(),
                 "unpitched audio must produce a usable model with a reference pitch");
    checkModel (*noiseResult.model);
    checkImport (std::abs (noiseResult.model->f0Ref - 220.0f) < .001f
                  && noiseResult.note.containsIgnoreCase ("reference")
                  && noiseResult.note.contains ("220"),
                 "unpitched import must disclose the 220 Hz reference pitch");
    const float peak = *std::max_element (noiseResult.model->env.begin(), noiseResult.model->env.end());
    checkImport (peak > 1e-5f, "unpitched import must preserve audible spectral energy");

    AudioFixture shortAudio;
    juce::AudioBuffer<float> oneSample (1, 1);
    oneSample.setSample (0, 0, .25f);
    shortAudio.write (oneSample, 44100);
    const auto shortResult = audioImport::analyze (shortAudio.file, {});
    if (shortResult.model != nullptr)
    {
        checkModel (*shortResult.model);
        checkImport (shortResult.error.isEmpty(), "valid short audio model must not include an error");
    }
    else
        checkImport (shortResult.error.isNotEmpty(), "unsupported short audio must explain why it failed");
}

void testRejectedAndCancelledAudio()
{
    AudioFixture silence;
    juce::AudioBuffer<float> empty (1, 22050);
    empty.clear();
    silence.write (empty, 44100);
    const auto silentResult = audioImport::analyze (silence.file, {});
    checkImport (silentResult.model == nullptr && silentResult.error.isNotEmpty(),
                 "silent audio must return an error rather than an empty instrument");

    AudioFixture invalid;
    checkImport (invalid.file.replaceWithText ("This is not a WAV file."), "cannot write invalid fixture");
    const auto invalidResult = audioImport::analyze (invalid.file, {});
    checkImport (invalidResult.model == nullptr && invalidResult.error.isNotEmpty(),
                 "invalid audio must report a decoding error");

    AudioFixture nonfinite;
    {
        // IEEE-float WAV retains NaN, which integer fixture writers cannot encode.
        constexpr int sampleCount = 22050;
        juce::FileOutputStream output (nonfinite.file);
        checkImport (output.openedOk(), "cannot create nonfinite audio fixture");
        output.write ("RIFF", 4); output.writeInt (36 + sampleCount * 4);
        output.write ("WAVEfmt ", 8); output.writeInt (16);
        output.writeShort (3); output.writeShort (1);
        output.writeInt (44100); output.writeInt (44100 * 4);
        output.writeShort (4); output.writeShort (32);
        output.write ("data", 4); output.writeInt (sampleCount * 4);
        for (int i = 0; i < sampleCount; ++i)
            output.writeFloat (i == 10000 ? std::numeric_limits<float>::quiet_NaN()
                : .4f * (float) std::sin (juce::MathConstants<double>::twoPi * 440 * i / 44100.0));
    }
    const auto nonfiniteResult = audioImport::analyze (nonfinite.file, {});
    checkImport (nonfiniteResult.model == nullptr && nonfiniteResult.error.isNotEmpty(),
                 "nonfinite source samples must be rejected before spectral analysis");

    AudioFixture longAudio;
    juce::AudioBuffer<float> longBuffer (1, 61 * 8000);
    longBuffer.clear();
    longAudio.write (longBuffer, 8000);
    const auto longResult = audioImport::analyze (longAudio.file, {});
    checkImport (longResult.model == nullptr && longResult.error.isNotEmpty(),
                 "overlong audio must be rejected rather than silently truncated");
    checkImport (longResult.error.containsIgnoreCase ("60")
              || longResult.error.containsIgnoreCase ("long")
              || longResult.error.containsIgnoreCase ("duration"),
                 "overlong audio must explain the duration limit");

    AudioFixture cancelled;
    cancelled.write (sine (1, 44100, .5, 440), 44100);
    int notifications = 0;
    const auto cancelledResult = audioImport::analyze (cancelled.file, [&] (float progress)
    {
        ++notifications;
        return progress < .01f;
    });
    checkImport (notifications > 0 && cancelledResult.model == nullptr
                  && cancelledResult.error.containsIgnoreCase ("cancel"),
                 "cancelled import must stop and report cancellation");

    float lastProgress = 0;
    const auto lateCancelledResult = audioImport::analyze (cancelled.file, [&] (float progress)
    {
        lastProgress = progress;
        return progress <= .6f;
    });
    checkImport (lastProgress > .6f && lateCancelledResult.model == nullptr
                  && lateCancelledResult.error.containsIgnoreCase ("cancel"),
                 "cancellation during spectral analysis must discard the partial model");
}
}

void testAudioImportAnalysis()
{
    testPitchedAudio();
    testChangingEnvelope();
    testUnpitchedAndShortAudio();
    testRejectedAndCancelledAudio();
    std::cout << "PASS: native audio pitch, envelopes, stereo, invalid input and cancellation\n";
}
