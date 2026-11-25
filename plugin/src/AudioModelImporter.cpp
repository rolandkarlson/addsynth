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
// A sound counts as pitched when steady detections carry nearly all of its
// energy and cover most of its audible frames. Single voices and instruments
// score above .95 on both; drum breaks, loops with a bassline and full mixes
// score roughly .0 to .75, and following their scattered pitch sounds broken.
constexpr float minimumVoicedEnergy = .85f, minimumVoicedFrames = .75f;

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

// Decides whether a single pitch explains the sound and, if so, tidies the
// track. Drum loops and mixed music only give scattered detections; following
// those would bend every partial around like a random pitch LFO, so anything
// that is not clearly one pitched voice gets a fixed reference instead.
// Returns false (leaving the track untouched) for unpitched material.
bool resolvePitchTrack (std::vector<float>& pitches, const std::vector<float>& energy)
{
    const int count = (int) pitches.size();
    const auto semitones = [] (float a, float b) { return std::abs (12.0f * std::log2 (a / b)); };

    // Median of the voiced neighbours removes single-frame octave slips.
    std::vector<float> track (pitches);
    for (int frame = 0; frame < count; ++frame)
    {
        if (pitches[(size_t) frame] <= 0) continue;
        std::vector<float> nearby;
        for (int at = juce::jmax (0, frame - 2); at <= juce::jmin (count - 1, frame + 2); ++at)
            if (pitches[(size_t) at] > 0) nearby.push_back (pitches[(size_t) at]);
        if (nearby.size() >= 3) track[(size_t) frame] = median (nearby);
    }

    // Keep only detections that hold together for a few frames.
    constexpr int minimumRun = 4;
    for (int begin = 0; begin < count;)
    {
        if (track[(size_t) begin] <= 0) { ++begin; continue; }
        int end = begin + 1;
        while (end < count && track[(size_t) end] > 0
               && semitones (track[(size_t) end], track[(size_t) end - 1]) < 1.0f) ++end;
        if (end - begin < minimumRun) std::fill (track.begin() + begin, track.begin() + end, 0.0f);
        begin = end;
    }

    const float loudest = *std::max_element (energy.begin(), energy.end());
    double total = 0.0, voicedEnergy = 0.0;
    int active = 0, voicedActive = 0, voicedFrames = 0;
    for (int frame = 0; frame < count; ++frame)
    {
        const bool voiced = track[(size_t) frame] > 0;
        total += energy[(size_t) frame];
        if (voiced) { voicedEnergy += energy[(size_t) frame]; ++voicedFrames; }
        if (energy[(size_t) frame] < .0025f * loudest) continue;   // quieter than 5% of the peak level
        ++active;
        if (voiced) ++voicedActive;
    }
    if (voicedFrames < minimumRun || total <= 0.0 || active == 0
        || voicedEnergy < minimumVoicedEnergy * total
        || (float) voicedActive < minimumVoicedFrames * (float) active)
        return false;

    // Bridge short gaps smoothly; across longer ones hold the last pitch and
    // move to the next just before it starts, so silence and noise between
    // notes do not turn into slow glides.
    constexpr int bridge = 4, approach = 2;
    int previous = -1;
    for (int frame = 0; frame < count; ++frame)
    {
        if (track[(size_t) frame] <= 0) continue;
        const float target = track[(size_t) frame];
        const float start = previous < 0 ? target : track[(size_t) previous];
        const int gap = frame - previous - 1;
        const int ramp = previous < 0 ? 0 : (gap <= bridge ? gap : approach);
        for (int at = previous + 1; at < frame; ++at)
        {
            const int intoRamp = at - (frame - ramp) + 1;
            track[(size_t) at] = intoRamp <= 0 ? start
                : start * std::pow (target / start, (float) intoRamp / (float) (ramp + 1));
        }
        previous = frame;
    }
    std::fill (track.begin() + previous + 1, track.end(), track[(size_t) previous]);
    pitches = std::move (track);
    return true;
}

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
    // Visits every spectral peak in [lowHz, highHz) as (frequency, sine amplitude).
    template <typename Visit>
    void peaks (float lowHz, float highHz, Visit&& visit) const
    {
        const float binHz = (float) analysisRate / (float) size;
        const int begin = juce::jmax (1, (int) std::floor (lowHz / binHz));
        const int end = juce::jmin (size / 2 - 2, (int) std::ceil (highHz / binHz));
        for (int bin = begin; bin <= end; ++bin)
        {
            const float middle = work[(size_t) bin];
            if (middle <= work[(size_t) bin - 1] || middle < work[(size_t) bin + 1] || middle < 1e-6f * gain) continue;
            // A parabola through the log magnitudes locates a windowed sine closely.
            const float left = std::log (work[(size_t) bin - 1] + 1e-20f), centre = std::log (middle);
            const float right = std::log (work[(size_t) bin + 1] + 1e-20f);
            const float denominator = left - 2.0f * centre + right;
            const float delta = denominator < -1e-9f ? juce::jlimit (-.5f, .5f, .5f * (left - right) / denominator) : 0.0f;
            const float frequency = ((float) bin + delta) * binHz;
            if (frequency >= lowHz && frequency < highHz)
                visit (frequency, std::exp (centre - .25f * (left - right) * delta) / gain);
        }
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

// ---- fixed bank for sounds without one clear pitch ------------------------
// Breaks, loops and mixes have no single fundamental to hang harmonics on.
// Their partials are spread over the spectrum instead: quarter tones (aligned
// to A440) where pitch is heard precisely, wider steps above. Every spectral
// peak feeds the partial nearest to it, so bass lines, chords and drum
// resonances keep their own pitches and nothing is ever tracked or bent.
constexpr int bankPartials = 192, bankQuarterTones = 144;
constexpr float bankReference = 220.0f, bankTop = 16000.0f;

float hannRms (const std::vector<float>& samples, int centre, int length)
{
    double energy = 0.0, weight = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const int at = centre + i - length / 2;
        const double window = .5 - .5 * std::cos (juce::MathConstants<double>::twoPi * (i + .5) / length);
        weight += window;
        if (juce::isPositiveAndBelow (at, (int) samples.size()))
            energy += window * samples[(size_t) at] * samples[(size_t) at];
    }
    return (float) std::sqrt (energy / weight);
}

bool analyzeBank (const std::vector<float>& samples, AddmModel& model, const Progress& progress)
{
    std::array<float, bankPartials> centres {};
    for (int k = 0; k < bankQuarterTones; ++k)
        centres[(size_t) k] = 440.0f * std::exp2 ((float) (k - 84) / 24.0f);   // from 38.9 Hz
    const float last = centres[bankQuarterTones - 1];
    const float step = std::pow (bankTop / last, 1.0f / (float) (bankPartials - bankQuarterTones));
    for (int k = bankQuarterTones; k < bankPartials; ++k)
        centres[(size_t) k] = centres[(size_t) k - 1] * step;
    // Each partial owns the band up to the midpoints towards its neighbours.
    std::array<float, bankPartials + 1> edges {};
    edges[0] = centres[0] * std::exp2 (-1.0f / 48.0f);
    for (int k = 1; k < bankPartials; ++k)
        edges[(size_t) k] = std::sqrt (centres[(size_t) k - 1] * centres[(size_t) k]);
    edges[bankPartials] = centres[bankPartials - 1] * std::sqrt (step);

    model.nPartials = bankPartials;
    model.f0Ref = bankReference;
    model.env.assign ((size_t) model.nFrames * bankPartials, 0.0f);
    model.f0Track.assign ((size_t) model.nFrames, 1.0f);
    model.detune.assign (bankPartials, 1.0f);

    // Long windows resolve close low notes, short ones keep high transients tight.
    constexpr float lowSplit = 700.0f, highSplit = 1400.0f;
    Spectrum longSpectrum (12), middleSpectrum (11), shortSpectrum (10);
    std::array<double, bankPartials> power {}, tuning {}, tuningWeight {};
    const auto add = [&] (float frequency, float amplitude)
    {
        const auto band = std::upper_bound (edges.begin(), edges.end(), frequency) - edges.begin() - 1;
        if (band < 0 || band >= bankPartials) return;
        const double energy = (double) amplitude * amplitude;
        power[(size_t) band] += energy;
        tuning[(size_t) band] += energy * std::log2 (frequency / centres[(size_t) band]);
        tuningWeight[(size_t) band] += energy;
    };
    for (int frame = 0; frame < model.nFrames; ++frame)
    {
        if (! report (progress, .55f + .43f * (float) frame / (float) model.nFrames)) return false;
        const int centre = frame * hop;
        power.fill (0.0);
        longSpectrum.calculate (samples, centre);
        longSpectrum.peaks (edges.front(), lowSplit, add);
        middleSpectrum.calculate (samples, centre);
        middleSpectrum.peaks (lowSplit, highSplit, add);
        shortSpectrum.calculate (samples, centre);
        shortSpectrum.peaks (highSplit, edges.back(), add);

        // Match the level of the source over a span short enough to keep
        // hits sharp but long enough not to follow single bass cycles.
        double modeledEnergy = 0.0;
        for (double value : power) modeledEnergy += .5 * value;
        const float correction = modeledEnergy > 1e-16
            ? juce::jlimit (0.0f, 4.0f, hannRms (samples, centre, 1024) / (float) std::sqrt (modeledEnergy)) : 0.0f;
        for (int k = 0; k < bankPartials; ++k)
            model.env[(size_t) frame * bankPartials + (size_t) k] = correction * (float) std::sqrt (power[(size_t) k]);
    }

    // Slide each partial onto the energy it actually carried, so sources that
    // are not tuned to A440 keep their intonation. The engine multiplies the
    // harmonic number by detune, which lets partials sit anywhere.
    for (int k = 0; k < bankPartials; ++k)
    {
        float frequency = centres[(size_t) k];
        if (tuningWeight[(size_t) k] > 0.0)
            frequency *= std::exp2 ((float) (tuning[(size_t) k] / tuningWeight[(size_t) k]));
        frequency = juce::jlimit (edges[(size_t) k], edges[(size_t) k + 1], frequency);
        model.detune[(size_t) k] = frequency / (bankReference * (float) (k + 1));
    }
    return true;
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
    std::vector<float> pitches ((size_t) pitchFrames), pitchEnergy ((size_t) pitchFrames);
    for (int frame = 0; frame < pitchFrames; ++frame)
    {
        if (! report (progress, .38f + .17f * (float) frame / (float) pitchFrames)) return cancelled();
        pitches[(size_t) frame] = detector.estimate (pitchSamples, frame * hop);
        // Full-band energy under the same span the detector looked at.
        const int centre = frame * hop * 4;
        const int begin = juce::jmax (0, centre - 2048), end = juce::jmin ((int) samples.size(), centre + 2048);
        double sum = 0.0;
        for (int i = begin; i < end; ++i) sum += samples[(size_t) i] * samples[(size_t) i];
        pitchEnergy[(size_t) frame] = (float) sum;
    }
    const bool pitched = resolvePitchTrack (pitches, pitchEnergy);

    auto model = std::make_shared<AddmModel>();
    model->name = file.getFileNameWithoutExtension();
    model->controlRate = (float) analysisRate / hop;
    model->nFrames = juce::jmax (2, 1 + (int) (samples.size() - 1) / hop);
    if (! pitched)
    {
        if (! analyzeBank (samples, *model, progress)) return cancelled();
    }
    else
    {
        const float reference = median (pitches);
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
            double total = 0.0, weight = 0.0;
            for (int harmonic = 1; harmonic <= 3; ++harmonic)
            {
                const auto candidate = longSpectrum.peak (frequency * (float) harmonic, frequency);
                total += candidate.frequency / harmonic * candidate.amplitude;
                weight += candidate.amplitude;
            }
            if (weight > 1e-8) frequency = juce::jlimit (frequency * .944f, frequency * 1.059f, (float) (total / weight));
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
        model->f0Ref = median (model->f0Track);
        for (auto& frequency : model->f0Track) frequency /= model->f0Ref;
        for (int partial = 0; partial < model->nPartials; ++partial)
        {
            std::vector<float> ratios;
            for (const auto& value : strongestPeaks[(size_t) partial])
                if (value.amplitude > 1e-6f) ratios.push_back (value.frequency);
            if (! ratios.empty()) model->detune[(size_t) partial] = juce::jlimit (.94f, 1.06f, median (ratios));
        }
    }
    if (std::none_of (model->env.begin(), model->env.end(), [] (float value) { return value > 1e-10f; }))
        return failed ("No usable spectral content was found. Try a different sound or a pitched selection.");
    if (! report (progress, 1.0f)) return cancelled();
    juce::String note;
    if (! pitched) note = "No single pitch detected, so its notes were kept as a fixed bank of partials. "
                          "A3 (220 Hz) is the reference key that plays it at the original pitch.";
    if (channels == 2 && ! useMid) note += (note.isEmpty() ? "" : " ") + juce::String ("Used the stronger channel to avoid stereo cancellation.");
    return { std::move (model), {}, note };
}
}
