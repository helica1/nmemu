#pragma once

#include <memory>

#include "nmmhardware.h"

#include "synthLib/device.h"
#include "synthLib/midiBufferParser.h"

namespace nmm
{
	// synthLib::Device adapter, what gearmulator's plugin layer drives.
	//
	// The device create params carry the OS image (a stock Clavia updater exe or a descrambled
	// image) as romData; the boot flash is looked up next to it by name if present.
	class Device : public synthLib::Device
	{
	public:
		explicit Device(const synthLib::DeviceCreateParams& _params);

		float getSamplerate() const override { return static_cast<float>(g_samplerate); }
		bool isValid() const override { return m_hardware && m_hardware->isValid(); }
		bool getState(std::vector<uint8_t>& _state, synthLib::StateType _type) override;
		bool setState(const std::vector<uint8_t>& _state, synthLib::StateType _type) override;
		uint32_t getChannelCountIn() override { return 2; }
		uint32_t getChannelCountOut() override { return 2; }
		bool setDspClockPercent(uint32_t _percent) override;
		uint32_t getDspClockPercent() const override;
		uint64_t getDspClockHz() const override;

		Hardware* getHardware() { return m_hardware.get(); }

		static bool findBootRom(const std::string& _osFilename, std::vector<uint8_t>& _bootRom);

	protected:
		void readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut) override;
		void processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, size_t _samples) override;
		bool sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response) override;

	private:
		std::unique_ptr<Hardware> m_hardware;
		std::vector<uint8_t> m_midiOutBuffer;
		std::vector<uint8_t> m_pcOutBuffer;
		synthLib::MidiBufferParser m_midiParser;
		synthLib::MidiBufferParser m_pcParser;
		uint32_t m_numSamplesProcessed = 0;
	};
}
