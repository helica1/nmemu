#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "RomFinder.h"

#include "synthLib/deviceException.h"
#include "synthLib/midiToSysex.h"

#include "nmmLib/nmmlog.h"

namespace nmm
{
	namespace
	{
		constexpr double g_ackTimeoutSeconds = 1.0;
		constexpr double g_iAmRetrySeconds = 2.0;
		constexpr int g_iAmMaxRetries = 10;
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor()
		: AudioProcessor(BusesProperties()
			.withInput("Input", juce::AudioChannelSet::stereo(), true)
			.withOutput("Output", juce::AudioChannelSet::stereo(), true))
	{
		// The DSP's full scale (where it limits) maps to 0 dBFS. A single oscillator at default
		// level sits some 26 dB below that, so a makeup gain is applied, adjustable by the host.
		addParameter(m_outputGain = new juce::AudioParameterFloat(juce::ParameterID("outputGain", 1), "Output Gain",
			juce::NormalisableRange<float>(-24.0f, 36.0f, 0.1f), 18.0f, juce::AudioParameterFloatAttributes().withLabel("dB")));
		createDevice();
	}

	AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
	{
		m_editorWindowForAudio.store(nullptr);
		m_editorWindow.reset();
		m_virtualMidi.reset();
		if(m_device)
			m_device->saveFlash();
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

		// the PC port as a pair of virtual MIDI ports for the Nord Modular editors
		m_virtualMidi.reset(new VirtualMidi([this](const synthLib::SMidiEvent& _ev)
		{
			if(m_plugin)
				m_plugin->addMidiEvent(_ev);
		}));
	}

	void AudioPluginAudioProcessor::openEditorWindow()
	{
		if(m_editorWindow)
		{
			m_editorWindow->toFront(true);
			return;
		}
		if(!m_plugin)
			return;
		auto* w = new EditorWindow("Nord Micro Modular (emulator)",
			[this](const synthLib::SMidiEvent& _ev) { if(m_plugin) m_plugin->addMidiEvent(_ev); },
			[this]() { closeEditorWindow(); });
		m_editorWindow.reset(w);
		m_editorWindowForAudio.store(w);
	}

	void AudioPluginAudioProcessor::closeEditorWindow()
	{
		// closing from within the window's own callback: defer the destruction
		if(!m_editorWindow)
			return;
		m_editorWindowForAudio.store(nullptr);
		auto* w = m_editorWindow.release();
		juce::MessageManager::callAsync([w]() { delete w; });
	}

	void AudioPluginAudioProcessor::syncPatchFromEditor()
	{
		if(!m_editorWindow || !m_editorWindow->isConnected())
			return;
		std::string name;
		const auto text = m_editorWindow->getCurrentPatchText(name);
		if(text.empty())
			return;
		std::lock_guard lock(m_patchMutex);
		if(text == m_patchText)
			return;
		// the editor already put this into the synth, remember it without uploading it again
		m_patchText = text;
		m_patchName = name;
		m_patchFile.clear();
		m_hasPatch = true;
		m_upload = PatchUpload();
		m_upload.state = PatchUpload::State::Done;
	}

	std::string AudioPluginAudioProcessor::getEditorStatus() const
	{
		if(!EditorWindow::isAvailable())
			return "editor not built in";
		if(!m_editorWindow)
			return "editor closed";
		return m_editorWindow->getStatus();
	}

	std::string AudioPluginAudioProcessor::getStatusText() const
	{
		std::string s;
		if(m_device && m_device->isValid())
		{
			s += "OS: " + m_osFile + "\n";
			s += "Boot flash: " + (m_bootRomFile.empty() ? std::string("none (HLE boot)") : m_bootRomFile) + "\n";
			s += "DSP clock: " + std::to_string(m_device->getDspClockHz() / 1000000) + " MHz, 96 kHz\n";
			s += "Patch flash: " + (m_device->getFlashFile().empty() ? std::string("not persisted") : m_device->getFlashFile()) + "\n";
			if(m_virtualMidi && m_virtualMidi->isValid())
				s += "Virtual MIDI port: '" + m_virtualMidi->getName() + "', connect the Nord Modular editor (Animatek NME, nomad) to it\n";
			else
				s += "Virtual MIDI port: not available on this platform\n";
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

	// ---------------------------------------------------------------------------------------------
	// patches
	// ---------------------------------------------------------------------------------------------

	bool AudioPluginAudioProcessor::loadPatchFile(const juce::File& _file, std::string& _error)
	{
		const auto text = _file.loadFileAsString().toStdString();
		if(text.empty())
		{
			_error = "cannot read " + _file.getFullPathName().toStdString();
			return false;
		}
		if(!loadPatchText(text, PchFile::nameFromFilename(_file.getFullPathName().toStdString()), _error))
			return false;
		std::lock_guard lock(m_patchMutex);
		m_patchFile = _file.getFullPathName().toStdString();
		return true;
	}

	bool AudioPluginAudioProcessor::loadPatchText(const std::string& _text, const std::string& _name, std::string& _error)
	{
		Patch patch;
		if(!PchFile::parse(_text, patch, _error, _name))
			return false;

		// a Micro Modular has outputs 1/2 only, keep patches made for 3/4 audible
		const auto rerouted = patch.routeOutputsToMain();

		auto frames = PatchSysex::upload(patch, 0);

		std::lock_guard lock(m_patchMutex);
		m_patch = patch;
		m_patchText = _text;
		m_patchName = patch.name;
		m_patchFile.clear();
		m_hasPatch = true;
		m_patchRerouted = rerouted;

		m_upload = PatchUpload();
		m_upload.frames = std::move(frames);
		m_upload.state = m_plugin ? PatchUpload::State::WaitIAm : PatchUpload::State::Failed;
		m_upload.retrySample = 0;	// send the first "I am" right away
		return true;
	}

	std::string AudioPluginAudioProcessor::getPatchName() const
	{
		std::lock_guard lock(m_patchMutex);
		return m_hasPatch ? m_patchName : std::string();
	}

	std::string AudioPluginAudioProcessor::getPatchStatus() const
	{
		std::lock_guard lock(m_patchMutex);
		if(!m_hasPatch)
			return "no patch loaded";
		switch(m_upload.state)
		{
		case PatchUpload::State::Idle:		return "";
		case PatchUpload::State::WaitIAm:	return "waiting for the synth (OS boot takes a few seconds)...";
		case PatchUpload::State::Sending:	return "uploading packet " + std::to_string(m_upload.next) + "/" + std::to_string(m_upload.frames.size());
		case PatchUpload::State::Done:		return m_upload.frames.empty() ? "from the editor" : "loaded, " + std::to_string(m_upload.acks) + " packets acknowledged" + (m_patchRerouted ? " (outputs 3/4 routed to 1/2)" : "");
		case PatchUpload::State::Failed:	return "upload failed, the synth did not answer";
		}
		return {};
	}

	void AudioPluginAudioProcessor::onMidiOut(const synthLib::SMidiEvent& _ev)
	{
		if(_ev.sysex.size() < 5 || _ev.sysex[1] != 0x33)
			return;

		const auto cc = (_ev.sysex[2] >> 2) & 0x1f;

		std::lock_guard lock(m_patchMutex);

		if(cc == 0x00 && _ev.sysex.size() >= 5 && _ev.sysex[4] == 0x01 && m_upload.state == PatchUpload::State::WaitIAm)
		{
			// the synth introduced itself, it is ready for the upload
			m_upload.state = PatchUpload::State::Sending;
			m_upload.next = 0;
			m_upload.nextSendSample = m_hostSamplesProcessed;
		}
		else if(cc == 0x16 && m_upload.state != PatchUpload::State::Idle)
		{
			++m_upload.acks;
		}
	}

	void AudioPluginAudioProcessor::servicePatchUpload(const int _numHostSamples)
	{
		std::unique_lock lock(m_patchMutex, std::try_to_lock);
		if(!lock.owns_lock())
			return;

		auto& u = m_upload;
		const auto now = m_hostSamplesProcessed;

		auto send = [&](const std::vector<uint8_t>& _frame)
		{
			synthLib::SMidiEvent ev(synthLib::MidiEventSource::Editor);
			ev.sysex.assign(_frame.begin(), _frame.end());
			m_plugin->addMidiEvent(ev);
		};

		switch(u.state)
		{
		case PatchUpload::State::WaitIAm:
			if(now >= u.retrySample)
			{
				if(u.retries++ >= g_iAmMaxRetries)
				{
					u.state = PatchUpload::State::Failed;
					break;
				}
				send(PatchSysex::iAm());
				u.retrySample = now + g_iAmRetrySeconds * m_hostSamplerate;
			}
			break;
		case PatchUpload::State::Sending:
			// one packet at a time: the next one goes out once the synth acknowledged the previous
			// one (the OS drops packets that arrive while it is still busy), or after a timeout
			if(u.next < u.frames.size() && (u.next == 0 || u.acks >= static_cast<int>(u.next) || now >= u.nextSendSample))
			{
				send(u.frames[u.next++]);
				u.nextSendSample = now + g_ackTimeoutSeconds * m_hostSamplerate;
				if(u.next >= u.frames.size())
					u.state = PatchUpload::State::Done;
			}
			break;
		default:
			break;
		}
		(void)_numHostSamples;
	}

	// ---------------------------------------------------------------------------------------------

	void AudioPluginAudioProcessor::prepareToPlay(const double _sampleRate, const int _samplesPerBlock)
	{
		m_hostSamplerate = _sampleRate;
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

		servicePatchUpload(numSamples);

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

		{
			const auto gain = juce::Decibels::decibelsToGain(m_outputGain->get());
			for(int c=0; c<numOut; ++c)
				_buffer.applyGain(c, 0, numSamples, gain);
		}

		m_hostSamplesProcessed += numSamples;

		// MIDI OUT and PC port replies both go to the host's MIDI output
		m_midiOut.clear();
		m_plugin->getMidiOut(m_midiOut);
		for (const auto& e : m_midiOut)
		{
			if(m_virtualMidi)
				m_virtualMidi->send(e);
			if(auto* w = m_editorWindowForAudio.load())
				w->onSynthMidiOut(e);

			if(!e.sysex.empty())
			{
				onMidiOut(e);
				_midiMessages.addEvent(juce::MidiMessage::createSysExMessage(e.sysex.data() + 1, static_cast<int>(e.sysex.size()) - 2), 0);
			}
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
		// a project save is a good moment to persist the synth's flash (patch memory and settings) too
		if(m_device)
			m_device->saveFlash();

		juce::ValueTree v("nmm");
		for(uint32_t i=0; i<4; ++i)
			v.setProperty(juce::String("knob") + juce::String(i), static_cast<int>(m_knobs[i]), nullptr);
		{
			std::lock_guard lock(m_patchMutex);
			if(m_hasPatch)
			{
				v.setProperty("patchName", juce::String(m_patchName), nullptr);
				v.setProperty("patchFile", juce::String(m_patchFile), nullptr);
				v.setProperty("patchText", juce::String(m_patchText), nullptr);
			}
		}
		v.setProperty("outputGain", static_cast<double>(m_outputGain->get()), nullptr);
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
		if(v.hasProperty("outputGain"))
			*m_outputGain = static_cast<float>(static_cast<double>(v.getProperty("outputGain")));
		if(v.hasProperty("patchText"))
		{
			std::string err;
			if(loadPatchText(v.getProperty("patchText").toString().toStdString(), v.getProperty("patchName").toString().toStdString(), err))
			{
				std::lock_guard lock(m_patchMutex);
				m_patchFile = v.getProperty("patchFile").toString().toStdString();
			}
		}
	}
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new nmm::AudioPluginAudioProcessor();
}
