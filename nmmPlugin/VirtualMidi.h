#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <juce_audio_devices/juce_audio_devices.h>

#include "synthLib/midiTypes.h"

namespace nmm
{
	/*
	A virtual MIDI port pair that shows up in the system like a Nord Modular's PC port, so that the
	Nord Modular editors (Animatek NME, nomad, ...) can connect to the emulator as if it were the
	hardware. Whatever arrives on the input is handed to the receive callback, whatever the synth
	sends out of its PC port is sent on the output. Only macOS and Linux can create such ports.
	*/
	class VirtualMidi : private juce::MidiInputCallback
	{
	public:
		using ReceiveCallback = std::function<void(const synthLib::SMidiEvent&)>;

		explicit VirtualMidi(ReceiveCallback _receive);
		~VirtualMidi() override;

		bool isValid() const { return m_in != nullptr && m_out != nullptr; }
		const std::string& getName() const { return m_name; }

		// synth -> editor, called from the audio thread
		void send(const synthLib::SMidiEvent& _ev);

		static bool isSupported();

	private:
		void handleIncomingMidiMessage(juce::MidiInput* _source, const juce::MidiMessage& _message) override;

		ReceiveCallback m_receive;
		std::string m_name;
		std::unique_ptr<juce::MidiInput> m_in;
		std::unique_ptr<juce::MidiOutput> m_out;
	};
}
