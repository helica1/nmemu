#include "PluginEditor.h"

#include <cstdlib>

#include "nmmLib/nmmlog.h"

namespace nmm
{
	AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor& _p)
		: AudioProcessorEditor(&_p), m_processor(_p)
	{
		m_status.setText(m_processor.getStatusText(), juce::dontSendNotification);
		m_status.setJustificationType(juce::Justification::topLeft);
		m_status.setFont(juce::Font(12.0f));
		addAndMakeVisible(m_status);

		m_patchLabel.setJustificationType(juce::Justification::centredLeft);
		m_patchLabel.setFont(juce::Font(14.0f));
		addAndMakeVisible(m_patchLabel);

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

		m_loadPatch.onClick = [this]()
		{
			m_chooser = std::make_unique<juce::FileChooser>("Select a Nord Modular patch", juce::File(), "*.pch");
			m_chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& _fc)
			{
				const auto f = _fc.getResult();
				if(!f.existsAsFile())
					return;
				std::string err;
				if(!m_processor.loadPatchFile(f, err))
					m_patchLabel.setText("Failed to load patch: " + err, juce::dontSendNotification);
				timerCallback();
			});
		};
		addAndMakeVisible(m_loadPatch);

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

		m_openEditor.onClick = [this]() { m_processor.openEditorWindow(); };
		m_openEditor.setEnabled(EditorWindow::isAvailable());
		addAndMakeVisible(m_openEditor);
		m_editorStatus.setFont(juce::Font(12.0f));
		addAndMakeVisible(m_editorStatus);

		timerCallback();
		startTimerHz(4);

		// test aid: NMEMU_OPEN_EDITOR=1 opens the patch editor window right away
		if(std::getenv("NMEMU_OPEN_EDITOR"))
			juce::MessageManager::callAsync([this]() { m_processor.openEditorWindow(); });

		setSize(620, 380);
	}

	void AudioPluginAudioProcessorEditor::timerCallback()
	{
		const auto name = m_processor.getPatchName();
		const auto status = m_processor.getPatchStatus();
		m_patchLabel.setText(name.empty() ? juce::String(status) : juce::String("Patch: " + name + "  -  " + status), juce::dontSendNotification);
		const auto editorStatus = m_processor.getEditorStatus();
		if(editorStatus != m_lastEditorStatus)
		{
			m_lastEditorStatus = editorStatus;
			NMMLOG("editor status: %s", editorStatus.c_str());
		}
		m_editorStatus.setText("Editor: " + juce::String(editorStatus), juce::dontSendNotification);
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
		auto buttons = area.removeFromTop(28);
		m_loadPatch.setBounds(buttons.removeFromLeft(200));
		buttons.removeFromLeft(8);
		m_loadSysex.setBounds(buttons.removeFromLeft(200));
		area.removeFromTop(6);
		auto row2 = area.removeFromTop(28);
		m_openEditor.setBounds(row2.removeFromLeft(220));
		row2.removeFromLeft(8);
		m_editorStatus.setBounds(row2);
		area.removeFromTop(6);
		m_patchLabel.setBounds(area.removeFromTop(24));
		area.removeFromTop(8);
		m_status.setBounds(area);
	}
}
