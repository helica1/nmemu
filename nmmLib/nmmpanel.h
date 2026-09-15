#pragma once

#include <array>
#include <cstdint>

#include "nmmtypes.h"

namespace nmm
{
	// Front panel glue: an input port (buttons, dial) and output latches (LEDs, display).
	// Until the panel protocol is understood this stores and traces everything and lets the
	// host set raw input values.
	class Panel
	{
	public:
		Panel();

		static bool isInRange(const uint32_t _addr)
		{
			return _addr >= g_panelInAddress && _addr < g_panelAuxAddress + g_panelAuxSize;
		}

		uint8_t read8(uint32_t _addr, uint32_t _pc);
		uint16_t read16(uint32_t _addr, uint32_t _pc);
		void write8(uint32_t _addr, uint8_t _val, uint32_t _pc);
		void write16(uint32_t _addr, uint16_t _val, uint32_t _pc);

		// raw input byte returned for reads of $201800+n
		void setInput(const uint32_t _offset, const uint8_t _val) { m_inputs[_offset & 0xff] = _val; }
		uint8_t getInput(const uint32_t _offset) const { return m_inputs[_offset & 0xff]; }

		uint8_t getLatch(const uint32_t _offset) const { return m_latches[_offset & 0xff]; }

	private:
		std::array<uint8_t, 256> m_inputs;
		std::array<uint8_t, 256> m_latches;
		uint8_t m_selectedInput = 0;
	};
}
