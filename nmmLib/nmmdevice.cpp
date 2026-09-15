#include "nmmdevice.h"

#include "nmmlog.h"
#include "nmmromloader.h"

#include "baseLib/filesystem.h"

#include "synthLib/deviceException.h"
#include "synthLib/os.h"

namespace nmm
{
	namespace
	{
		HardwareConfig makeConfig(const synthLib::DeviceCreateParams& _params)
		{
			HardwareConfig cfg;

			cfg.os = RomLoader::parseOs(_params.romData);
			if(!cfg.os.isValid() && !_params.romName.empty())
				cfg.os = RomLoader::loadOs(_params.romName);

			Device::findBootRom(_params.romName, cfg.bootRom);

			cfg.dspSlots = {0};	// Micro Modular: one DSP at host port slot 0
			return cfg;
		}
	}

	bool Device::findBootRom(const std::string& _osFilename, std::vector<uint8_t>& _bootRom)
	{
		// any 512K .bin with a plausible reset vector in the folder of the OS file
		const auto dir = baseLib::filesystem::getPath(_osFilename);
		std::vector<std::string> files;
		baseLib::filesystem::getDirectoryEntries(files, dir.empty() ? "." : dir);
		for (const auto& f : files)
		{
			if(!baseLib::filesystem::hasExtension(f, ".bin"))
				continue;
			if(RomLoader::loadBootRom(f, _bootRom))
				return true;
		}
		return false;
	}

	Device::Device(const synthLib::DeviceCreateParams& _params)
		: synthLib::Device(_params)
		, m_midiParser(synthLib::MidiEventSource::Device)
		, m_pcParser(synthLib::MidiEventSource::Device)
	{
		const auto cfg = makeConfig(_params);

		if(!cfg.os.isValid())
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing, "No Nord Modular OS found, expected a Clavia OS update .exe or a descrambled OS image");

		if(cfg.bootRom.empty())
			NMMLOG("Device: no boot flash image found next to the OS, using HLE boot");

		m_hardware.reset(new Hardware(cfg));
	}

	bool Device::getState(std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		return false;
	}

	bool Device::setState(const std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		return false;
	}

	bool Device::setDspClockPercent(const uint32_t _percent)
	{
		auto* dsp = m_hardware->getMasterDSP();
		return dsp ? dsp->getPeriph().getEssiClock().setSpeedPercent(_percent) : false;
	}

	uint32_t Device::getDspClockPercent() const
	{
		auto* dsp = const_cast<Hardware&>(*m_hardware).getMasterDSP();
		return dsp ? dsp->getPeriph().getEssiClock().getSpeedPercent() : 100;
	}

	uint64_t Device::getDspClockHz() const
	{
		auto* dsp = const_cast<Hardware&>(*m_hardware).getMasterDSP();
		return dsp ? dsp->getPeriph().getEssiClock().getSpeedInHz() : 0;
	}

	void Device::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		m_hardware->readMidiOut(m_midiOutBuffer, m_pcOutBuffer);

		m_midiParser.write(m_midiOutBuffer);
		m_midiOutBuffer.clear();
		m_midiParser.getEvents(_midiOut);

		m_pcParser.write(m_pcOutBuffer);
		m_pcOutBuffer.clear();
		m_pcParser.getEvents(_midiOut);
	}

	void Device::processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, const size_t _samples)
	{
		m_hardware->processAudio(_inputs, _outputs, static_cast<uint32_t>(_samples), getExtraLatencySamples());
		m_numSamplesProcessed += static_cast<uint32_t>(_samples);
	}

	bool Device::sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response)
	{
		auto e = _ev;
		e.offset += m_numSamplesProcessed + getExtraLatencySamples();
		m_hardware->sendMidi(e);
		return true;
	}
}
