#pragma once

#include <functional>
#include <memory>
#include <string>

#include <juce_gui_extra/juce_gui_extra.h>

#include "synthLib/midiTypes.h"

class MainComponent;

namespace nmm
{
	/*
	The Animatek NME editor in its own window. The editor connects to the synth through the
	emulator's virtual MIDI ports, the same way it connects to the hardware, so it does not know
	it is talking to an emulation. Its settings live in a properties file of our own so that the
	user's standalone editor keeps its own configuration.
	*/
	class EditorWindow : public juce::DocumentWindow
	{
	public:
		using SendToSynth = std::function<void(const synthLib::SMidiEvent&)>;

		EditorWindow(const std::string& _synthName, SendToSynth _sendToSynth, std::function<void()> _onClose);
		~EditorWindow() override;

		// synth -> editor, called from the audio thread with everything the PC port emits
		void onSynthMidiOut(const synthLib::SMidiEvent& _ev);

		void closeButtonPressed() override;

		bool isConnected() const;
		std::string getStatus() const;

		static bool isAvailable();

	private:
		class Content;
		class Synth;

		std::function<void()> m_onClose;
		std::unique_ptr<Synth> m_synth;
		juce::ApplicationProperties m_properties;
		std::unique_ptr<Content> m_content;
	};
}
