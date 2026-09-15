#pragma once

#include <array>
#include <vector>
#include <cstdint>

#include "nmmtypes.h"
#include "nmmdspslots.h"
#include "nmmpanel.h"
#include "nmmflash.h"
#include "nmmpcport.h"

#include "mc68k/mc68k.h"

#include "hardwareLib/sciMidi.h"

namespace nmm
{
	class Hardware;

	// MC68331 host CPU with the Nord Modular memory map around it
	class Microcontroller final : public mc68k::Mc68k
	{
	public:
		Microcontroller(Hardware& _hardware, const std::vector<uint8_t>& _bootRom);

		uint32_t exec() override;

		uint32_t getResetPC() override { return readImm32(4); }
		uint32_t getResetSP() override { return readImm32(0); }

		uint16_t readImm16(const uint32_t _addr) override
		{
			return mc68k::memoryOps::readU16(m_romRam.data(), _addr & (g_romRamSize - 1));
		}
		uint32_t readImm32(const uint32_t _addr) const
		{
			return mc68k::memoryOps::readU32(m_romRam.data(), _addr & (g_romRamSize - 1));
		}

		uint16_t read16(uint32_t _addr) override;
		uint8_t read8(uint32_t _addr) override;
		void write16(uint32_t _addr, uint16_t _val) override;
		void write8(uint32_t _addr, uint8_t _val) override;

		uint32_t onIllegalInstruction(uint32_t _opcode) override;
		void onBgnd() override;
		uint32_t readIrqUserVector(uint8_t _level) override;

		uint32_t getSR() const;
		uint64_t getIrqCount() const { return m_irqCount; }
		uint64_t getSciTxWrites() const { return m_sciTxWrites; }

		DspSlots& getDspSlots() { return m_dspSlots; }
		Panel& getPanel() { return m_panel; }
		Flash& getFlash() { return m_flash; }
		hwLib::SciMidi& getMidi() { return m_midi; }
		PcPort& getPcPort() { return m_pcPort; }

		// front panel knobs, read by the OS through a QSPI ADC: 4 channels, 8 bit each
		void setKnob(const uint32_t _index, const uint8_t _value) { m_knobs[_index & 3] = _value; }
		uint8_t getKnob(const uint32_t _index) const { return m_knobs[_index & 3]; }

		// direct RAM access for the console / HLE boot
		std::vector<uint8_t>& romRam() { return m_romRam; }
		void writeRam(uint32_t _addr, const std::vector<uint8_t>& _data);

		uint32_t getPrevPC() const { return m_prevPC; }
		uint64_t getInstructionCount() const { return m_instructions; }

		// PC history for post-mortem analysis of hangs
		void enablePcHistory(bool _enable) { m_pcHistoryEnabled = _enable; }
		const std::array<uint32_t, 64>& pcHistory() const { return m_pcHistory; }
		uint32_t pcHistoryPos() const { return m_pcHistoryPos; }

	private:
		Hardware& m_hardware;

		std::vector<uint8_t> m_romRam;

		DspSlots m_dspSlots;
		Panel m_panel;
		Flash m_flash;
		hwLib::SciMidi m_midi;
		PcPort m_pcPort;

		uint32_t m_prevPC = 0;
		uint64_t m_instructions = 0;
		uint64_t m_irqCount = 0;
		uint64_t m_sciTxWrites = 0;
		std::array<uint8_t, 4> m_knobs{0xff, 0xff, 0xff, 0xff};

		bool m_pcHistoryEnabled = false;
		std::array<uint32_t, 64> m_pcHistory{};
		uint32_t m_pcHistoryPos = 0;
	};
}
