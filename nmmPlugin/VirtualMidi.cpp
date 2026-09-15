#include "VirtualMidi.h"

#include "nmmLib/nmmlog.h"

#include "synthLib/midiBufferParser.h"

namespace nmm
{
	namespace
	{
		std::atomic<int> g_instances{0};
	}

	bool VirtualMidi::isSupported()
	{
#if JUCE_MAC || JUCE_LINUX || JUCE_IOS
		return true;
#else
		return false;
#endif
	}

	VirtualMidi::VirtualMidi(ReceiveCallback _receive) : m_receive(std::move(_receive))
	{
		if(!isSupported())
			return;

		const auto instance = ++g_instances;
		m_name = "Nord Micro Modular (emulator)";
		if(instance > 1)
			m_name += " " + std::to_string(instance);

		// the input is what other applications send to, the output is what they receive from
		m_in = juce::MidiInput::createNewDevice(m_name, this);
		m_out = juce::MidiOutput::createNewDevice(m_name);

		if(m_in)
			m_in->start();

		if(isValid())
			NMMLOG("virtual MIDI ports '%s' created", m_name.c_str());
		else
			NMMLOG("failed to create virtual MIDI ports '%s'", m_name.c_str());
	}

	VirtualMidi::~VirtualMidi()
	{
		if(m_in)
			m_in->stop();
		m_in.reset();
		m_out.reset();
		--g_instances;
	}

	void VirtualMidi::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& _message)
	{
		if(!m_receive)
			return;

		synthLib::SMidiEvent ev(synthLib::MidiEventSource::Editor);

		if(_message.isSysEx() || _message.getRawDataSize() > 3)
		{
			ev.sysex.resize(static_cast<size_t>(_message.getRawDataSize()));
			memcpy(ev.sysex.data(), _message.getRawData(), ev.sysex.size());
		}
		else
		{
			synthLib::setShortMessage(ev, _message.getRawData(), static_cast<size_t>(_message.getRawDataSize()));
		}
		m_receive(ev);
	}

	void VirtualMidi::send(const synthLib::SMidiEvent& _ev)
	{
		if(!m_out)
			return;

		if(!_ev.sysex.empty())
		{
			if(_ev.sysex.size() < 2)
				return;
			m_out->sendMessageNow(juce::MidiMessage::createSysExMessage(_ev.sysex.data() + 1, static_cast<int>(_ev.sysex.size()) - 2));
		}
		else
		{
			const auto len = synthLib::MidiBufferParser::lengthFromStatusByte(_ev.a);
			if(len == 1) m_out->sendMessageNow(juce::MidiMessage(_ev.a));
			else if(len == 2) m_out->sendMessageNow(juce::MidiMessage(_ev.a, _ev.b));
			else m_out->sendMessageNow(juce::MidiMessage(_ev.a, _ev.b, _ev.c));
		}
	}
}
