#include "../src/AddmModel.h"
#include <stdexcept>

namespace
{
void requireModel (bool condition, const char* message)
{
    if (! condition) throw std::runtime_error (message);
}

AddmModel makeModel (bool withNoise)
{
    AddmModel model;
    model.name = juce::String::fromUTF8 ("Imported h\xc3\xa4\xc3\xa4l");
    model.nFrames = 3;
    model.nPartials = 2;
    model.nNoiseBands = withNoise ? 1 : 0;
    model.f0Ref = 261.6256f;
    model.controlRate = 187.5f;
    model.inharmonicity = .0002f;
    model.env = { 0, .02f, .4f, .1f, .01f, .003f };
    model.f0Track = { 1, 1.01f, .99f };
    model.detune = { 1, 1.003f };
    if (withNoise)
    {
        model.noiseEnv = { .001f, .002f, .003f };
        model.bandFreqs = { 1500 };
    }
    return model;
}

std::shared_ptr<AddmModel> readModel (const juce::MemoryBlock& bytes)
{
    juce::MemoryInputStream input (bytes, false);
    juce::String error;
    auto model = AddmModel::read (input, error);
    if (model == nullptr) throw std::runtime_error (error.toStdString());
    requireModel (error.isEmpty(), "valid model retained a stale error");
    return model;
}

void expectInvalid (const juce::MemoryBlock& bytes)
{
    juce::MemoryInputStream input (bytes, false);
    juce::String error;
    requireModel (AddmModel::read (input, error) == nullptr, "invalid ADDM bytes were accepted");
    requireModel (error.isNotEmpty(), "invalid ADDM bytes need an error message");
}

class FailedOutput : public juce::OutputStream
{
public:
    void flush() override {}
    juce::int64 getPosition() override { return 0; }
    bool setPosition (juce::int64) override { return false; }
    bool write (const void*, size_t) override { return false; }
};
}

void testModelSerialization()
{
    for (bool withNoise : { false, true })
    {
        const auto original = makeModel (withNoise);
        juce::MemoryOutputStream output;
        requireModel (original.write (output), "could not encode valid model");
        const auto bytes = output.getMemoryBlock();
        const auto restored = readModel (bytes);
        requireModel (restored->name == original.name, "UTF-8 model name changed");
        requireModel (restored->nFrames == original.nFrames && restored->nPartials == original.nPartials
                      && restored->nNoiseBands == original.nNoiseBands, "model dimensions changed");
        requireModel (restored->f0Ref == original.f0Ref && restored->controlRate == original.controlRate
                      && restored->inharmonicity == original.inharmonicity, "model tuning changed");
        requireModel (restored->env == original.env && restored->f0Track == original.f0Track
                      && restored->noiseEnv == original.noiseEnv && restored->bandFreqs == original.bandFreqs
                      && restored->detune == original.detune, "model arrays changed during roundtrip");

        for (size_t size : { (size_t) 0, (size_t) 4, (size_t) 11, bytes.getSize() - 1 })
            expectInvalid (juce::MemoryBlock (bytes.getData(), size));
        auto invalidSamples = bytes;
        const auto headerLength = juce::ByteOrder::littleEndianInt (static_cast<const char*> (bytes.getData()) + 8);
        const juce::uint32 nanBits = juce::ByteOrder::swapIfBigEndian ((juce::uint32) 0x7fc00000);
        invalidSamples.copyFrom (&nanBits, 12 + headerLength, sizeof (nanBits));
        expectInvalid (invalidSamples);

        FailedOutput failed;
        requireModel (! original.write (failed), "write must report an output failure");
    }

    const juce::StringArray badHeaders {
        "[]",
        R"({"n_frames":2,"n_partials":-1})",
        R"({"n_frames":2147483647,"n_partials":2147483647,"n_noise_bands":2147483647})",
        R"({"n_frames":1e100,"n_partials":1})",
        R"({"n_frames":2,"n_partials":1,"control_rate":0})"
    };
    for (const auto& header : badHeaders)
    {
        juce::MemoryOutputStream output;
        output.write ("ADDM", 4); output.writeInt (2);
        output.writeInt ((int) header.getNumBytesAsUTF8());
        output.write (header.toRawUTF8(), header.getNumBytesAsUTF8());
        expectInvalid (output.getMemoryBlock());
    }
    juce::MemoryOutputStream oversizedHeader;
    oversizedHeader.write ("ADDM", 4); oversizedHeader.writeInt (2); oversizedHeader.writeInt (0x7fffffff);
    expectInvalid (oversizedHeader.getMemoryBlock());

    auto badModel = makeModel (false);
    badModel.detune[0] = std::numeric_limits<float>::infinity();
    juce::MemoryOutputStream invalidOutput;
    requireModel (! badModel.write (invalidOutput) && invalidOutput.getDataSize() == 0,
                  "writer must reject invalid spectral data before writing");

    // Locate the Python-generated fixtures whether CTest runs from build/ or the repo root.
    auto root = juce::File::getCurrentWorkingDirectory();
    while (! root.getChildFile ("models").isDirectory() && root != root.getParentDirectory())
        root = root.getParentDirectory();
    const auto fixtures = root.getChildFile ("models").findChildFiles (juce::File::findFiles, true, "*.addm");
    requireModel (! fixtures.isEmpty(), "could not locate Python-generated ADDM fixtures");
    for (const auto& file : fixtures)
    {
        juce::String error;
        const auto model = AddmModel::load (file, error);
        if (model == nullptr)
            throw std::runtime_error ((file.getFileName() + ": " + error).toStdString());
    }
}
