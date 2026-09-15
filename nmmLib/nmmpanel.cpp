#include "nmmpanel.h"

#include "nmmlog.h"

namespace nmm
{
	Panel::Panel()
	{
		m_inputs.fill(0xff);	// active low buttons, nothing pressed
		m_latches.fill(0);
	}

	uint8_t Panel::read8(const uint32_t _addr, const uint32_t _pc)
	{
		uint8_t v = 0xff;
		if(_addr >= g_panelInAddress && _addr < g_panelInAddress + g_panelInSize)
			v = m_inputs[(_addr - g_panelInAddress) & 0xff];
		else if(_addr >= g_panelOutAddress && _addr < g_panelOutAddress + g_panelOutSize)
			v = m_latches[(_addr - g_panelOutAddress) & 0xff];
		else
			v = m_latches[0x80 + ((_addr - g_panelAuxAddress) & 0x7f)];
		NMMTRACE(panel, "PANEL rd8  $%06x -> $%02x  pc=$%06x", _addr, v, _pc);
		return v;
	}

	uint16_t Panel::read16(const uint32_t _addr, const uint32_t _pc)
	{
		const uint16_t v = (static_cast<uint16_t>(read8(_addr, _pc)) << 8) | read8(_addr + 1, _pc);
		return v;
	}

	void Panel::write8(const uint32_t _addr, const uint8_t _val, const uint32_t _pc)
	{
		NMMTRACE(panel, "PANEL wr8  $%06x <- $%02x  pc=$%06x", _addr, _val, _pc);
		if(_addr >= g_panelOutAddress && _addr < g_panelOutAddress + g_panelOutSize)
			m_latches[(_addr - g_panelOutAddress) & 0xff] = _val;
		else if(_addr >= g_panelAuxAddress)
			m_latches[0x80 + ((_addr - g_panelAuxAddress) & 0x7f)] = _val;
		else
			m_selectedInput = _val;
	}

	void Panel::write16(const uint32_t _addr, const uint16_t _val, const uint32_t _pc)
	{
		write8(_addr, _val >> 8, _pc);
		write8(_addr + 1, _val & 0xff, _pc);
	}
}
