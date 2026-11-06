#include "PluginProcessor.h"

juce::AudioProcessorValueTreeState::ParameterLayout AutoTuneProcessor::createLayout()
{
    using namespace juce;

    juce::StringArray scaleNames;
    for (int i = 0; i < at::kNumScales; ++i)
        scaleNames.add (at::kScaleNames[i]);

    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "key", 1 }, "Tonalidad",
        StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));

    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { "scale", 1 }, "Escala", scaleNames, 0));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "speed", 1 }, "Velocidad de correccion",
        NormalisableRange<float> (0.0f, 400.0f, 0.1f, 0.4f), 20.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { "amount", 1 }, "Cantidad",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
        AudioParameterFloatAttributes().withLabel ("%")));

    return { params.begin(), params.end() };
}

AutoTuneProcessor::AutoTuneProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    keyParam = apvts.getRawParameterValue ("key");
    scaleParam = apvts.getRawParameterValue ("scale");
    speedParam = apvts.getRawParameterValue ("speed");
    amountParam = apvts.getRawParameterValue ("amount");
}

void AutoTuneProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate, juce::jmax (1, getTotalNumInputChannels()));
    setLatencySamples (engine.latencySamples());
}

bool AutoTuneProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void AutoTuneProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    engine.setParams ((int) std::lround (keyParam->load()),
                      (int) std::lround (scaleParam->load()),
                      speedParam->load(),
                      amountParam->load() / 100.0f);

    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
}

juce::AudioProcessorEditor* AutoTuneProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

void AutoTuneProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AutoTuneProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AutoTuneProcessor();
}
