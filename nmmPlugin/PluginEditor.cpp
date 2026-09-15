#include "PluginEditor.h"

namespace nmm
{
	AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor& _p)
		: AudioProcessorEditor(&_p), m_processor(_p)
	{
		m_status.setText(m_processor.getStatusText(), juce::dontSendNotification);
		m_status.setJustificationType(juce::Justification::topLeft);
		m_status.setFont(juce::Font(12.0f));
		addAndMakeVisible(m_status);

		static const char* names[4] = {"Master Level", "Knob 1", "Knob 2", "Knob 3"};

		for(uint32_t i=0; i<4; ++i)
		{
			auto& s = m_knobs[i];
			s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
			s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 60, 18);
			s.setRange(0, 255, 1);
			s.setValue(m_processor.getKnob(i), juce::dontSendNotification);
			s.onValueChange = [this, i]() { m_processor.setKnob(i, static_cast<uint8_t>(m_knobs[i].getValue())); };
			addAndMakeVisible(s);

			m_knobLabels[i].setText(names[i], juce::dontSendNotification);
			m_knobLabels[i].setJustificationType(juce::Justification::centred);
			addAndMakeVisible(m_knobLabels[i]);
		}

		m_loadSysex.onClick = [this]()
		{
			m_chooser = std::make_unique<juce::FileChooser>("Select a .syx file", juce::File(), "*.syx");
			m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& _fc)
			{
				const auto f = _fc.getResult();
				if(f.existsAsFile())
					m_processor.sendSysexFile(f);
			});
		};
		addAndMakeVisible(m_loadSysex);

		setSize(560, 300);
	}

	void AudioPluginAudioProcessorEditor::paint(juce::Graphics& g)
	{
		g.fillAll(juce::Colour(0xff202428));
		g.setColour(juce::Colours::white);
		g.setFont(juce::Font(18.0f));
		g.drawText("Nord Micro Modular emulator", 12, 8, getWidth() - 24, 24, juce::Justification::left);
	}

	void AudioPluginAudioProcessorEditor::resized()
	{
		auto area = getLocalBounds().reduced(12);
		area.removeFromTop(32);

		auto knobRow = area.removeFromTop(120);
		const auto w = knobRow.getWidth() / 4;
		for(uint32_t i=0; i<4; ++i)
		{
			auto cell = knobRow.removeFromLeft(w);
			m_knobLabels[i].setBounds(cell.removeFromTop(20));
			m_knobs[i].setBounds(cell);
		}

		area.removeFromTop(8);
		m_loadSysex.setBounds(area.removeFromTop(28).removeFromLeft(220));
		area.removeFromTop(8);
		m_status.setBounds(area);
	}
}
