#pragma once

#include <array>
#include <functional>

#include "nmmtypes.h"

#include "mc68k/hdi08.h"
#include "mc68k/peripheralTypes.h"

namespace nmm
{
	// The 2K window at $200000 is decoded into 8 byte HDI08 register blocks, one per DSP slot.
	// Every slot has a host-side HDI08 model so that the 68k always finds a sane register set;
	// slots that have a DSP attached get their callbacks wired up by the DSP object.
	class DspSlots
	{
	public:
		DspSlots();

		static bool isInRange(const uint32_t _addr)
		{
			return _addr >= g_dspWindowAddress && _addr < g_dspWindowAddress + g_dspWindowSize;
		}

		uint8_t read8(uint32_t _addr, uint32_t _pc);
		uint16_t read16(uint32_t _addr, uint32_t _pc);
		void write8(uint32_t _addr, uint8_t _val, uint32_t _pc);
		void write16(uint32_t _addr, uint16_t _val, uint32_t _pc);

		void exec(uint32_t _deltaCycles);

		mc68k::Hdi08& slot(const uint32_t _index) { return m_slots[_index]; }

		// called after the 68k writes the ICR of a slot (host flags HF0/HF1 live there)
		using IcrCallback = std::function<void(uint8_t)>;
		void setIcrCallback(const uint32_t _index, IcrCallback _cb) { m_icrCallbacks[_index] = std::move(_cb); }

		// slot indices the firmware has touched so far, for bring-up analysis
		const std::array<uint32_t, g_dspSlotCount>& accessCounts() const { return m_accessCounts; }

		static uint32_t slotIndex(const uint32_t _addr) { return ((_addr - g_dspWindowAddress) / g_dspSlotSize) % g_dspSlotCount; }
		static mc68k::PeriphAddress localAddr(const uint32_t _addr) { return static_cast<mc68k::PeriphAddress>((_addr - g_dspWindowAddress) % g_dspSlotSize); }

	private:
		std::array<mc68k::Hdi08, g_dspSlotCount> m_slots;
		std::array<uint32_t, g_dspSlotCount> m_accessCounts{};
		std::array<IcrCallback, g_dspSlotCount> m_icrCallbacks;
	};
}
