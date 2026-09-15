#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"

namespace nmm
{
	class AudioPluginAudioProcessorEditor : public juce::AudioProcessorEditor
	{
	public:
		explicit AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor&);
		~AudioPluginAudioProcessorEditor() override = default;

		void paint(juce::Graphics&) override;
		void resized() override;

	private:
		AudioPluginAudioProcessor& m_processor;

		juce::Label m_status;
		std::array<juce::Slider, 4> m_knobs;
		std::array<juce::Label, 4> m_knobLabels;
		juce::TextButton m_loadSysex{"Send .syx to PC port..."};
		std::unique_ptr<juce::FileChooser> m_chooser;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
	};
}
