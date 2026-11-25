#include "AudioModelImporter.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <numeric>

namespace audioImport
{
namespace
{
constexpr double analysisRate = 44100.0;
constexpr double pitchRate = analysisRate / 4.0;
constexpr int hop = 256;
constexpr float minimumPitch = 45.0f, maximumPitch = 2000.0f;
constexpr double maximumDuration = 60.0;

bool report (const Progress& progress, float amount)
{
    return ! progress || progress (juce::jlimit (0.0f, 1.0f, amount));
}

Result failed (const juce::String& message) { return { nullptr, message, {} }; }
Result cancelled() { return failed ("Audio import cancelled."); }

float median (std::vector<float> values)
{
    const auto middle = values.begin() + (std::ptrdiff_t) (values.size() / 2);
    std::nth_element (values.begin(), middle, values.end());
    return *middle;
}

// A windowed-sinc resampler with a rate-dependent low-pass cutoff. The kernel
// is centred on the source position, so importing does not add playback delay.
bool resample (const std::vector<float>& input, double inputRate,
               std::vector<float>& output, const Progress& progress)
{
    const double ratio = inputRate / analysisRate;
    const double cutoff = .94 * juce::jmin (1.0, 1.0 / ratio);
    const int radius = (int) std::ceil (24.0 / cutoff);
    constexpr int phaseCount = 512;
    const int taps = radius * 2;
    std::vector<float> kernels ((size_t) phaseCount * (size_t) taps);
    for (int phase = 0; phase < phaseCount; ++phase)
    {
        const double fraction = (double) phase / phaseCount;
        double total = 0.0;
        for (int tap = 0; tap < taps; ++tap)
        {
            const double distance = tap - radius + 1 - fraction;
            const double angle = juce::MathConstants<double>::pi * distance * cutoff;
            const double sinc = std::abs (angle) < 1e-12 ? 1.0 : std::sin (angle) / angle;
            const double window = .5 + .5 * std::cos (juce::MathConstants<double>::pi * distance / radius);
            const float weight = (float) (cutoff * sinc * window);
            kernels[(size_t) phase * (size_t) taps + (size_t) tap] = weight;
            total += weight;
        }
        for (int tap = 0; tap < taps; ++tap)
            kernels[(size_t) phase * (size_t) taps + (size_t) tap] /= (float) total;
    }
    const int count = juce::jmax (1, (int) std::ceil ((double) input.size() / ratio));
    output.resize ((size_t) count);
    for (int sample = 0; sample < count; ++sample)
    {
        if (sample % 4096 == 0 && ! report (progress, .24f + .1f * (float) sample / (float) count)) return false;
        const double position = sample * ratio;
        const int centre = (int) position;
        const int phase = juce::jlimit (0, phaseCount - 1, (int) ((position - centre) * phaseCount));
        const auto* kernel = kernels.data() + (size_t) phase * (size_t) taps;
        double value = 0.0;
        for (int tap = 0; tap < taps; ++tap)
        {
            const int index = centre - radius + 1 + tap;
            if (juce::isPositiveAndBelow (index, (int) input.size()))
                value += input[(size_t) index] * kernel[tap];
        }
        output[(size_t) sample] = (float) value;
    }
    return report (progress, .34f);
}

class PitchDetector
{
public:
    float estimate (const std::vector<float>& samples, int centre)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        float mean = 0.0f;
        for (int index = 0; index < windowSize; ++index)
        {
            const int at = centre + index - windowSize / 2;
            work[(size_t) index] = juce::isPositiveAndBelow (at, (int) samples.size()) ? samples[(size_t) at] : 0.0f;
            mean += work[(size_t) index] / windowSize;
        }
        energy[0] = 0.0;
        for (int index = 0; index < windowSize; ++index)
        {
            work[(size_t) index] -= mean;
            energy[(size_t) index + 1] = energy[(size_t) index] + work[(size_t) index] * work[(size_t) index];
        }
        if (energy[windowSize] < 1e-10) return 0.0f;
        fft.performRealOnlyForwardTransform (work.data(), true);
        for (int bin = 0; bin <= fftSize / 2; ++bin)
        {
            const float real = work[(size_t) bin * 2], imaginary = work[(size_t) bin * 2 + 1];
            work[(size_t) bin * 2] = real * real + imaginary * imaginary;
            work[(size_t) bin * 2 + 1] = 0.0f;
        }
        fft.performRealOnlyInverseTransform (work.data());

        // YIN's cumulative-mean difference, using FFT autocorrelation to avoid
        // a window-size by lag-size inner loop for every analysis frame.
        constexpr int maxLag = (int) (pitchRate / minimumPitch);
        constexpr int minLag = (int) (pitchRate / maximumPitch);
        double cumulative = 0.0;
        difference[0] = 1.0f;
        for (int lag = 1; lag <= maxLag + 1; ++lag)
        {
            const double value = juce::jmax (0.0, energy[windowSize - lag]
                + energy[windowSize] - energy[(size_t) lag] - 2.0 * work[(size_t) lag]);
            cumulative += value;
            difference[(size_t) lag] = cumulative > 1e-20 ? (float) (value * lag / cumulative) : 1.0f;
        }
        int selected = -1;
        for (int lag = minLag; lag <= maxLag; ++lag)
            if (difference[(size_t) lag] < .16f
                && difference[(size_t) lag] <= difference[(size_t) lag - 1]
                && difference[(size_t) lag] <= difference[(size_t) lag + 1])
            { selected = lag; break; }
        if (selected < 0) return 0.0f;
        const float left = difference[(size_t) selected - 1], middle = difference[(size_t) selected];
        const float right = difference[(size_t) selected + 1];
        const float denominator = left - 2.0f * middle + right;
        const float offset = std::abs (denominator) > 1e-12f
            ? juce::jlimit (-.5f, .5f, .5f * (left - right) / denominator) : 0.0f;
        const float frequency = (float) pitchRate / ((float) selected + offset);
        return frequency >= minimumPitch && frequency <= maximumPitch ? frequency : 0.0f;
    }
private:
    static constexpr int windowSize = 1024, fftSize = 2048;
    juce::dsp::FFT fft { 11 };
    std::array<float, fftSize * 2> work {};
    std::array<double, windowSize + 1> energy {};
    std::array<float, 512> difference {};
};

struct SpectralPeak { float amplitude = 0.0f, frequency = 0.0f; };

class Spectrum
{
public:
    explicit Spectrum (int order) : fft (order), size (1 << order),
        work ((size_t) size * 2), window ((size_t) size)
    {
        for (int i = 0; i < size; ++i)
        {
            window[(size_t) i] = .5f - .5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (size - 1));
            gain += window[(size_t) i] * .5f;
        }
    }

    void calculate (const std::vector<float>& samples, int centre)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        for (int i = 0; i < size; ++i)
        {
            const int at = centre + i - size / 2;
            if (juce::isPositiveAndBelow (at, (int) samples.size()))
                work[(size_t) i] = samples[(size_t) at] * window[(size_t) i];
        }
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
    }

    SpectralPeak peak (float frequency, float fundamental) const
    {
        const float binHz = (float) analysisRate / (float) size;
        const float position = frequency / binHz;
        if (position >= (float) size / 2 - 2) return {};
        const int radius = juce::jmax (1, (int) (.4f * fundamental / binHz));
        const int begin = juce::jmax (1, (int) position - radius);
        const int end = juce::jmin (size / 2 - 1, (int) std::ceil (position) + radius);
        int index = begin;
        for (int bin = begin + 1; bin <= end; ++bin)
            if (work[(size_t) bin] > work[(size_t) index]) index = bin;
        const float left = work[(size_t) index - 1], middle = work[(size_t) index], right = work[(size_t) index + 1];
        const float denominator = left - 2.0f * middle + right;
        const float delta = std::abs (denominator) > 1e-12f
            ? juce::jlimit (-.5f, .5f, .5f * (left - right) / denominator) : 0.0f;
        const float found = ((float) index + delta) * binHz;
        if (middle >= left && middle >= right && std::abs (found / frequency - 1.0f) < .06f)
            return { juce::jmax (0.0f, middle - .25f * (left - right) * delta) / gain, found };
        const int first = juce::jlimit (0, size / 2 - 1, (int) position);
        const float mix = position - (float) first;
        return { juce::jmax (0.0f, work[(size_t) first] * (1.0f - mix) + work[(size_t) first + 1] * mix) / gain, frequency };
    }
private:
    juce::dsp::FFT fft;
    int size;
    std::vector<float> work, window;
    float gain = 0.0f;
};

float localRms (const std::vector<float>& samples, int centre, float frequency)
{
    const int half = juce::jmax (hop / 2, (int) (analysisRate / frequency));
    const int begin = juce::jmax (0, centre - half), end = juce::jmin ((int) samples.size(), centre + half);
    double energy = 0.0;
    for (int i = begin; i < end; ++i) energy += samples[(size_t) i] * samples[(size_t) i];
    return (float) std::sqrt (energy / juce::jmax (1, end - begin));
}
}

juce::String supportedWildcards()
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return formats.getWildcardForAllFormats();
}

bool supports (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    return formats.findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

Result analyze (const juce::File& file, const Progress& progress)
{
    if (! report (progress, 0.0f)) return cancelled();
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr) return failed ("Could not read this audio file. Try WAV, AIFF or FLAC.");
    if (! std::isfinite (reader->sampleRate) || reader->sampleRate < 8000 || reader->sampleRate > 192000)
        return failed ("Use audio with a sample rate between 8 and 192 kHz.");
    if (reader->numChannels < 1 || reader->numChannels > 2)
        return failed ("Use a mono or stereo audio file.");
    if (reader->lengthInSamples < 1) return failed ("This audio file is empty.");
    if ((double) reader->lengthInSamples / reader->sampleRate > maximumDuration)
        return failed ("Audio import supports up to 60 seconds. Export a shorter selection and drop it here.");

    const int length = (int) reader->lengthInSamples, channels = (int) reader->numChannels;
    constexpr int blockSize = 8192;
    juce::AudioBuffer<float> block (channels, blockSize);
    double energy[3] {}, sum[3] {};
    for (int offset = 0; offset < length; offset += blockSize)
    {
        if (! report (progress, .12f * (float) offset / (float) length)) return cancelled();
        const int count = juce::jmin (blockSize, length - offset);
        if (! reader->read (&block, 0, count, offset, true, true)) return failed ("Audio decoding failed.");
        for (int i = 0; i < count; ++i)
        {
            const float left = block.getSample (0, i), right = block.getSample (channels - 1, i);
            if (! std::isfinite (left) || ! std::isfinite (right)
                || std::abs (left) > 1.0e6f || std::abs (right) > 1.0e6f)
                return failed ("The audio file contains invalid sample values.");
            const double values[] = { left, right, .5 * ((double) left + right) };
            for (int channel = 0; channel < 3; ++channel)
            { sum[channel] += values[channel]; energy[channel] += values[channel] * values[channel]; }
        }
    }
    for (int channel = 0; channel < 3; ++channel)
        energy[channel] = juce::jmax (0.0, energy[channel] - sum[channel] * sum[channel] / length);
    const int strongest = energy[0] >= energy[1] ? 0 : 1;
    if (energy[strongest] / length < 1e-14) return failed ("The audio file is silent or contains only DC.");
    const bool useMid = channels == 2 && energy[2] >= .2 * energy[strongest];
    const int selected = useMid ? 2 : strongest;
    const float mean = (float) (sum[selected] / length);
    std::vector<float> source ((size_t) length);
    float peak = 0.0f;
    for (int offset = 0; offset < length; offset += blockSize)
    {
        if (! report (progress, .12f + .12f * (float) offset / (float) length)) return cancelled();
        const int count = juce::jmin (blockSize, length - offset);
        if (! reader->read (&block, 0, count, offset, true, true)) return failed ("Audio decoding failed.");
        for (int i = 0; i < count; ++i)
        {
            const float value = useMid ? .5f * (block.getSample (0, i) + block.getSample (1, i))
                                       : block.getSample (juce::jmin (strongest, channels - 1), i);
            source[(size_t) offset + (size_t) i] = value - mean;
            peak = juce::jmax (peak, std::abs (value - mean));
        }
    }
    if (peak > 1.0f) for (auto& value : source) value /= peak;
    std::vector<float> samples;
    if (std::abs (reader->sampleRate - analysisRate) < .01)
    { samples = std::move (source); if (! report (progress, .34f)) return cancelled(); }
    else if (! resample (source, reader->sampleRate, samples, progress)) return cancelled();
    source.clear(); source.shrink_to_fit();

    // Low-pass before decimating for the pitch detector. Spectral analysis
    // continues to use the full 44.1 kHz signal.
    std::array<juce::dsp::IIR::Filter<float>, 3> filters;
    const double q[] = { .5176380902, .7071067812, 1.931851653 };
    for (size_t i = 0; i < filters.size(); ++i)
        filters[i].coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (analysisRate, 2200.0f, (float) q[i]);
    std::vector<float> pitchSamples;
    pitchSamples.reserve ((samples.size() + 3) / 4);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        if (i % 8192 == 0 && ! report (progress, .34f + .04f * (float) i / (float) samples.size())) return cancelled();
        float value = samples[i];
        for (auto& filter : filters) value = filter.processSample (value);
        if (i % 4 == 0) pitchSamples.push_back (value);
    }
    PitchDetector detector;
    const int pitchFrames = juce::jmax (2, 1 + (int) pitchSamples.size() / hop);
    std::vector<float> pitches ((size_t) pitchFrames), voiced;
    for (int frame = 0; frame < pitchFrames; ++frame)
    {
        if (! report (progress, .38f + .17f * (float) frame / (float) pitchFrames)) return cancelled();
        pitches[(size_t) frame] = detector.estimate (pitchSamples, frame * hop);
        if (pitches[(size_t) frame] > 0) voiced.push_back (pitches[(size_t) frame]);
    }
    const bool pitched = voiced.size() >= (size_t) juce::jmin (3, pitchFrames);
    const float reference = pitched ? median (voiced) : 220.0f;
    if (! pitched) std::fill (pitches.begin(), pitches.end(), reference);
    else
    {
        int previous = -1;
        for (int frame = 0; frame < pitchFrames; ++frame)
            if (pitches[(size_t) frame] > 0)
            {
                for (int gap = previous + 1; gap < frame; ++gap)
                {
                    const float mix = previous < 0 ? 1.0f : (float) (gap - previous) / (float) (frame - previous);
                    const float start = previous < 0 ? pitches[(size_t) frame] : pitches[(size_t) previous];
                    pitches[(size_t) gap] = start + mix * (pitches[(size_t) frame] - start);
                }
                previous = frame;
            }
        std::fill (pitches.begin() + previous + 1, pitches.end(), pitches[(size_t) previous]);
    }

    auto model = std::make_shared<AddmModel>();
    model->name = file.getFileNameWithoutExtension();
    model->controlRate = (float) analysisRate / hop;
    model->nFrames = juce::jmax (2, 1 + (int) (samples.size() - 1) / hop);
    model->nPartials = juce::jlimit (24, 192, (int) std::lround (16000.0f / reference));
    model->f0Ref = reference;
    model->env.resize ((size_t) model->nFrames * (size_t) model->nPartials);
    model->f0Track.resize ((size_t) model->nFrames);
    model->detune.assign ((size_t) model->nPartials, 1.0f);
    std::vector<std::array<SpectralPeak, 8>> strongestPeaks ((size_t) model->nPartials);
    Spectrum longSpectrum (12), shortSpectrum (10);
    float previousRms = 0.0f;
    int lastOnset = 0;
    for (int frame = 0; frame < model->nFrames; ++frame)
    {
        if (! report (progress, .55f + .43f * (float) frame / (float) model->nFrames)) return cancelled();
        const int coarse = juce::jmin (frame / 4, pitchFrames - 1);
        const int next = juce::jmin (coarse + 1, pitchFrames - 1);
        const float mix = (float) (frame % 4) / 4.0f;
        float frequency = pitches[(size_t) coarse] * (1.0f - mix) + pitches[(size_t) next] * mix;
        const int centre = frame * hop;
        longSpectrum.calculate (samples, centre);
        if (pitched)
        {
            double total = 0.0, weight = 0.0;
            for (int harmonic = 1; harmonic <= 3; ++harmonic)
            {
                const auto candidate = longSpectrum.peak (frequency * (float) harmonic, frequency);
                total += candidate.frequency / harmonic * candidate.amplitude;
                weight += candidate.amplitude;
            }
            if (weight > 1e-8) frequency = juce::jlimit (frequency * .944f, frequency * 1.059f, (float) (total / weight));
        }
        model->f0Track[(size_t) frame] = frequency;
        const float rms = localRms (samples, centre, frequency);
        if (rms > previousRms * 1.8f && rms > .001f) lastOnset = frame;
        previousRms = rms;
        const float sinceOnset = (float) (frame - lastOnset) / model->controlRate;
        const float shortWeight = frequency >= 181.0f ? juce::jlimit (0.0f, 1.0f, (.18f - sinceOnset) / .1f) : 0.0f;
        if (shortWeight > 0) shortSpectrum.calculate (samples, centre);
        double modeledEnergy = 0.0;
        for (int partial = 0; partial < model->nPartials; ++partial)
        {
            const float expected = frequency * (float) (partial + 1);
            const auto candidate = longSpectrum.peak (expected, frequency);
            float amplitude = candidate.amplitude;
            if (shortWeight > 0)
                amplitude = amplitude * (1.0f - shortWeight) + shortSpectrum.peak (expected, frequency).amplitude * shortWeight;
            model->env[(size_t) frame * (size_t) model->nPartials + (size_t) partial] = amplitude;
            modeledEnergy += .5 * amplitude * amplitude;
            auto& best = strongestPeaks[(size_t) partial];
            auto weakest = std::min_element (best.begin(), best.end(), [] (auto a, auto b) { return a.amplitude < b.amplitude; });
            if (candidate.amplitude > weakest->amplitude)
                *weakest = { candidate.amplitude, expected > 0 ? candidate.frequency / expected : 1.0f };
        }
        const float correction = modeledEnergy > 1e-16 ? juce::jlimit (0.0f, 4.0f, rms / (float) std::sqrt (modeledEnergy)) : 0.0f;
        for (int partial = 0; partial < model->nPartials; ++partial)
            model->env[(size_t) frame * (size_t) model->nPartials + (size_t) partial] *= correction;
    }
    if (pitched) model->f0Ref = median (model->f0Track);
    for (auto& frequency : model->f0Track) frequency /= model->f0Ref;
    for (int partial = 0; partial < model->nPartials; ++partial)
    {
        std::vector<float> ratios;
        for (const auto& value : strongestPeaks[(size_t) partial])
            if (value.amplitude > 1e-6f) ratios.push_back (value.frequency);
        if (! ratios.empty()) model->detune[(size_t) partial] = juce::jlimit (.94f, 1.06f, median (ratios));
    }
    if (std::none_of (model->env.begin(), model->env.end(), [] (float value) { return value > 1e-10f; }))
        return failed ("No usable spectral content was found. Try a different sound or a pitched selection.");
    if (! report (progress, 1.0f)) return cancelled();
    juce::String note;
    if (! pitched) note = "No stable pitch detected. Used A3 (220 Hz) as the reference.";
    if (channels == 2 && ! useMid) note += (note.isEmpty() ? "" : " ") + juce::String ("Used the stronger channel to avoid stereo cancellation.");
    return { std::move (model), {}, note };
}
}
