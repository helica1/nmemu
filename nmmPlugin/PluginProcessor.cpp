#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "RomFinder.h"

#include "synthLib/deviceException.h"
#include "synthLib/midiToSysex.h"

#include "nmmLib/nmmlog.h"

namespace nmm
{
	AudioPluginAudioProcessor::AudioPluginAudioProcessor()
		: AudioProcessor(BusesProperties()
			.withInput("Input", juce::AudioChannelSet::stereo(), true)
			.withOutput("Output", juce::AudioChannelSet::stereo(), true))
	{
		createDevice();
	}

	AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
	{
		m_plugin.reset();
		m_device.reset();
	}

	void AudioPluginAudioProcessor::createDevice()
	{
		const auto roms = RomFinder::find();
		m_searchedDirs = roms.searched;

		if(!roms.valid())
		{
			m_deviceError = "No Nord Modular OS found";
			return;
		}

		m_osFile = roms.osFile;
		m_bootRomFile = roms.bootRomFile;

		synthLib::DeviceCreateParams params;
		params.romName = roms.osFile;
		params.romData = roms.osData;

		try
		{
			m_device.reset(new Device(params));
		}
		catch(const synthLib::DeviceException& e)
		{
			m_deviceError = e.what();
			m_device.reset();
			return;
		}

		m_plugin.reset(new synthLib::Plugin(m_device.get(), [](synthLib::Device* _d) { return _d; }));
	}

	std::string AudioPluginAudioProcessor::getStatusText() const
	{
		std::string s;
		if(m_device && m_device->isValid())
		{
			s += "OS: " + m_osFile + "\n";
			s += "Boot flash: " + (m_bootRomFile.empty() ? std::string("none (HLE boot)") : m_bootRomFile) + "\n";
			s += "DSP clock: " + std::to_string(m_device->getDspClockHz() / 1000000) + " MHz, 96 kHz\n";
		}
		else
		{
			s += "Device not running: " + m_deviceError + "\n";
			s += "Put the Micro Modular OS updater (e.g. MicroModularUpdate303b.exe) and optionally a 512K boot flash .bin into one of:\n";
			for (const auto& d : m_searchedDirs)
				s += "  " + d + "\n";
		}
		return s;
	}

	void AudioPluginAudioProcessor::setKnob(const uint32_t _index, const uint8_t _value)
	{
		m_knobs[_index & 3] = _value;
		if(m_device && m_device->getHardware())
			m_device->getHardware()->setKnob(_index, _value);
	}

	void AudioPluginAudioProcessor::sendSysexFile(const juce::File& _file)
	{
		juce::MemoryBlock mb;
		if(!_file.loadFileAsData(mb))
			return;
		std::vector<uint8_t> bytes(static_cast<const uint8_t*>(mb.getData()), static_cast<const uint8_t*>(mb.getData()) + mb.getSize());
		sendSysex(bytes);
	}

	void AudioPluginAudioProcessor::sendSysex(const std::vector<uint8_t>& _bytes)
	{
		if(!m_plugin)
			return;

		// a file may hold several messages back to back
		size_t start = 0;
		while(start < _bytes.size())
		{
			if(_bytes[start] != 0xf0) { ++start; continue; }
			size_t end = start + 1;
			while(end < _bytes.size() && _bytes[end] != 0xf7) ++end;
			if(end >= _bytes.size()) break;

			synthLib::SMidiEvent ev(synthLib::MidiEventSource::Editor);
			ev.sysex.assign(_bytes.begin() + static_cast<long>(start), _bytes.begin() + static_cast<long>(end) + 1);
			m_plugin->addMidiEvent(ev);
			start = end + 1;
		}
	}

	void AudioPluginAudioProcessor::prepareToPlay(const double _sampleRate, const int _samplesPerBlock)
	{
		if(!m_plugin)
			return;
		m_plugin->setHostSamplerate(static_cast<float>(_sampleRate), 0.0f);
		m_plugin->setBlockSize(static_cast<uint32_t>(_samplesPerBlock));
		setLatencySamples(static_cast<int>(m_plugin->getLatencyMidiToOutput()));
	}

	bool AudioPluginAudioProcessor::isBusesLayoutSupported(const BusesLayout& _layouts) const
	{
		if(_layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
			return false;
		const auto in = _layouts.getMainInputChannelSet();
		return in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::disabled();
	}

	void AudioPluginAudioProcessor::processBlock(juce::AudioBuffer<float>& _buffer, juce::MidiBuffer& _midiMessages)
	{
		juce::ScopedNoDenormals noDenormals;

		const auto numSamples = _buffer.getNumSamples();
		const auto numIn = getTotalNumInputChannels();
		const auto numOut = getTotalNumOutputChannels();

		if(!m_plugin)
		{
			_buffer.clear();
			_midiMessages.clear();
			return;
		}

		synthLib::TAudioInputs inputs{};
		synthLib::TAudioOutputs outputs{};

		for(int c=0; c<numIn && c<static_cast<int>(inputs.size()); ++c)
			inputs[static_cast<size_t>(c)] = _buffer.getReadPointer(c);
		for(int c=0; c<numOut && c<static_cast<int>(outputs.size()); ++c)
			outputs[static_cast<size_t>(c)] = _buffer.getWritePointer(c);

		for(const auto metadata : _midiMessages)
		{
			const auto message = metadata.getMessage();

			synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);

			if(message.isSysEx() || message.getRawDataSize() > 3)
			{
				ev.sysex.resize(static_cast<size_t>(message.getRawDataSize()));
				memcpy(ev.sysex.data(), message.getRawData(), ev.sysex.size());
				synthLib::MidiToSysex::removeDuplicateFraming(ev.sysex);
			}
			else
			{
				synthLib::setShortMessage(ev, message.getRawData(), static_cast<size_t>(message.getRawDataSize()));
			}

			ev.offset = static_cast<uint32_t>(std::max(0, metadata.samplePosition));
			m_plugin->addMidiEvent(ev);
		}
		_midiMessages.clear();

		bool isPlaying = true;
		bool hasPpqPosition = false;
		float bpm = 0.0f;
		float ppqPos = 0.0f;

		if(const auto* playHead = getPlayHead())
		{
			if(const auto pos = playHead->getPosition())
			{
				isPlaying = pos->getIsPlaying();
				if(pos->getBpm()) bpm = static_cast<float>(*pos->getBpm());
				if(pos->getPpqPosition()) { ppqPos = static_cast<float>(*pos->getPpqPosition()); hasPpqPosition = true; }
			}
		}

		m_plugin->process(inputs, outputs, static_cast<size_t>(numSamples), bpm, ppqPos, isPlaying, hasPpqPosition);

		// MIDI OUT and PC port replies both go to the host's MIDI output
		m_midiOut.clear();
		m_plugin->getMidiOut(m_midiOut);
		for (const auto& e : m_midiOut)
		{
			if(!e.sysex.empty())
				_midiMessages.addEvent(juce::MidiMessage::createSysExMessage(e.sysex.data() + 1, static_cast<int>(e.sysex.size()) - 2), 0);
			else
			{
				const auto len = synthLib::MidiBufferParser::lengthFromStatusByte(e.a);
				if(len == 1) _midiMessages.addEvent(juce::MidiMessage(e.a), 0);
				else if(len == 2) _midiMessages.addEvent(juce::MidiMessage(e.a, e.b), 0);
				else _midiMessages.addEvent(juce::MidiMessage(e.a, e.b, e.c), 0);
			}
		}
	}

	juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
	{
		return new AudioPluginAudioProcessorEditor(*this);
	}

	void AudioPluginAudioProcessor::getStateInformation(juce::MemoryBlock& _destData)
	{
		// knob positions only for now; patch state follows once .pch loading exists
		juce::ValueTree v("nmm");
		for(uint32_t i=0; i<4; ++i)
			v.setProperty(juce::String("knob") + juce::String(i), static_cast<int>(m_knobs[i]), nullptr);
		juce::MemoryOutputStream os(_destData, false);
		v.writeToStream(os);
	}

	void AudioPluginAudioProcessor::setStateInformation(const void* _data, const int _sizeInBytes)
	{
		const auto v = juce::ValueTree::readFromData(_data, static_cast<size_t>(_sizeInBytes));
		if(!v.isValid())
			return;
		for(uint32_t i=0; i<4; ++i)
		{
			const auto id = juce::String("knob") + juce::String(i);
			if(v.hasProperty(id))
				setKnob(i, static_cast<uint8_t>(static_cast<int>(v.getProperty(id))));
		}
	}
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new nmm::AudioPluginAudioProcessor();
}
