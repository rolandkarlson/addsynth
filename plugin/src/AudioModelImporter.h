#pragma once
#include "AddmModel.h"
#include <functional>

namespace audioImport
{
struct Result
{
    std::shared_ptr<AddmModel> model;
    juce::String error;
    juce::String note;
};

// Runs entirely on the caller's worker thread. Return false to cancel.
using Progress = std::function<bool (float)>;
Result analyze (const juce::File&, const Progress& progress = {});
juce::String supportedWildcards();
bool supports (const juce::File&);
}
