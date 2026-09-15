#include "nmmmc.h"

#include <cassert>
#include <algorithm>

#include "nmmhardware.h"
#include "nmmlog.h"

#define MC68K_CLASS nmm::Microcontroller
#include "mc68k/musashiEntry.h"

namespace nmm
{
	Microcontroller::Microcontroller(Hardware& _hardware, const std::vector<uint8_t>& _bootRom)
		: m_hardware(_hardware)
		, m_romRam(g_romRamSize, 0)
		, m_midi(getQSM(), static_cast<float>(g_samplerateHost))
		, m_pcPort(*this)
	{
		if(!_bootRom.empty())
			std::copy(_bootRom.begin(), _bootRom.begin() + std::min<size_t>(_bootRom.size(), g_bootRomSize), m_romRam.begin());

		m_midi.setSysexDelay(0.05f, 1024);

		// The OS queues four QSPI transfers to an ADC and reads the results as 16 bit words that it
		// shifts right once and keeps the low byte of. Answer each queued word with the knob value.
		getQSM().setSpiWriteCallback([this](const uint16_t _data, const uint8_t _queue)
		{
			const uint16_t v = static_cast<uint16_t>(m_knobs[_queue & 3]) << 1;
			NMMTRACE(io, "QSPI queue %u tx $%04x -> adc $%04x", _queue, _data, v);
			getQSM().write16(static_cast<mc68k::PeriphAddress>(static_cast<uint32_t>(mc68k::PeriphAddress::ReceiveRam0) + 2 * (_queue & 0xf)), v);
		});

		reset();
	}

	void Microcontroller::writeRam(const uint32_t _addr, const std::vector<uint8_t>& _data)
	{
		assert(_addr >= g_ramAddress && _addr + _data.size() <= g_romRamSize);
		std::copy(_data.begin(), _data.end(), m_romRam.begin() + _addr);
	}

	uint32_t Microcontroller::exec()
	{
		const auto pc = getPC();
		m_prevPC = pc;
		++m_instructions;

		if(m_pcHistoryEnabled)
		{
			m_pcHistory[m_pcHistoryPos] = pc;
			m_pcHistoryPos = (m_pcHistoryPos + 1) & 63;
		}

		const auto cycles = Mc68k::exec();

		m_dspSlots.exec(cycles);
		m_pcPort.exec(cycles);

		return cycles;
	}

	uint32_t Microcontroller::onIllegalInstruction(const uint32_t _opcode)
	{
		NMMLOG("68k: illegal instruction $%04x at pc=$%06x", _opcode, m_prevPC);
		return 0;
	}

	void Microcontroller::onBgnd()
	{
		NMMLOG("68k: BGND at pc=$%06x", m_prevPC);
	}

	uint32_t Microcontroller::readIrqUserVector(const uint8_t _level)
	{
		const auto v = Mc68k::readIrqUserVector(_level);
		++m_irqCount;
		NMMTRACE(irq, "68k: irq level %u vector $%02x (%s) at pc=$%06x", _level, v, v == 0xffffffff ? "autovector" : "user", m_prevPC);
		return v;
	}

	uint32_t Microcontroller::getSR() const
	{
		return m68k_get_reg(const_cast<mc68k::CpuState*>(getCpuState()), M68K_REG_SR);
	}

	static inline bool watched(const uint32_t _addr, const uint32_t _size)
	{
		return Trace::watchSize && _addr + _size > Trace::watchAddr && _addr < Trace::watchAddr + Trace::watchSize;
	}

	uint8_t Microcontroller::read8(const uint32_t _addr)
	{
		if(_addr < g_romRamSize)
		{
			if(watched(_addr, 1))
				NMMLOG("WATCH rd8  $%06x -> $%02x pc=$%06x", _addr, m_romRam[_addr], m_prevPC);
			return m_romRam[_addr];
		}

		if(DspSlots::isInRange(_addr))	return m_dspSlots.read8(_addr, m_prevPC);
		if(Panel::isInRange(_addr))		return m_panel.read8(_addr, m_prevPC);
		if(Flash::isInRange(_addr))		return m_flash.read8(_addr);

		if(_addr < 0xf00000)
		{
			NMMTRACE(io, "68k: unmapped rd8  $%06x pc=$%06x", _addr, m_prevPC);
			return 0xff;
		}
		return Mc68k::read8(_addr);
	}

	uint16_t Microcontroller::read16(const uint32_t _addr)
	{
		if(_addr < g_romRamSize)
		{
			if(watched(_addr, 2))
				NMMLOG("WATCH rd16 $%06x -> $%04x pc=$%06x", _addr, mc68k::memoryOps::readU16(m_romRam.data(), _addr), m_prevPC);
			return mc68k::memoryOps::readU16(m_romRam.data(), _addr);
		}

		if(DspSlots::isInRange(_addr))	return m_dspSlots.read16(_addr, m_prevPC);
		if(Panel::isInRange(_addr))		return m_panel.read16(_addr, m_prevPC);
		if(Flash::isInRange(_addr))		return m_flash.read16(_addr);

		if(_addr < 0xf00000)
		{
			NMMTRACE(io, "68k: unmapped rd16 $%06x pc=$%06x", _addr, m_prevPC);
			return 0xffff;
		}
		return Mc68k::read16(_addr);
	}

	void Microcontroller::write8(const uint32_t _addr, const uint8_t _val)
	{
		if(_addr < g_romRamSize)
		{
			if(_addr < g_ramAddress)
			{
				NMMTRACE(io, "68k: write to boot rom ignored $%06x <- $%02x pc=$%06x", _addr, _val, m_prevPC);
				return;
			}
			if(watched(_addr, 1))
				NMMLOG("WATCH wr8  $%06x <- $%02x pc=$%06x", _addr, _val, m_prevPC);
			m_romRam[_addr] = _val;
			return;
		}

		if(DspSlots::isInRange(_addr))	{ m_dspSlots.write8(_addr, _val, m_prevPC); return; }
		if(Panel::isInRange(_addr))		{ m_panel.write8(_addr, _val, m_prevPC); return; }
		if(Flash::isInRange(_addr))		{ m_flash.write8(_addr, _val, m_prevPC); return; }

		if(_addr < 0xf00000)
		{
			NMMTRACE(io, "68k: unmapped wr8  $%06x <- $%02x pc=$%06x", _addr, _val, m_prevPC);
			return;
		}
		if((_addr & 0xfffff) == static_cast<uint32_t>(mc68k::PeriphAddress::SciDataLSB))
		{
			++m_sciTxWrites;
			NMMTRACE(io, "68k: SCI TX $%02x pc=$%06x", _val, m_prevPC);
		}
		Mc68k::write8(_addr, _val);
	}

	void Microcontroller::write16(const uint32_t _addr, const uint16_t _val)
	{
		if(_addr < g_romRamSize)
		{
			if(_addr < g_ramAddress)
			{
				NMMTRACE(io, "68k: write to boot rom ignored $%06x <- $%04x pc=$%06x", _addr, _val, m_prevPC);
				return;
			}
			if(watched(_addr, 2))
				NMMLOG("WATCH wr16 $%06x <- $%04x pc=$%06x", _addr, _val, m_prevPC);
			mc68k::memoryOps::writeU16(m_romRam.data(), _addr, _val);
			return;
		}

		if(DspSlots::isInRange(_addr))	{ m_dspSlots.write16(_addr, _val, m_prevPC); return; }
		if(Panel::isInRange(_addr))		{ m_panel.write16(_addr, _val, m_prevPC); return; }
		if(Flash::isInRange(_addr))		{ m_flash.write16(_addr, _val, m_prevPC); return; }

		if(_addr < 0xf00000)
		{
			NMMTRACE(io, "68k: unmapped wr16 $%06x <- $%04x pc=$%06x", _addr, _val, m_prevPC);
			return;
		}
		if((_addr & 0xfffff) == static_cast<uint32_t>(mc68k::PeriphAddress::SciData))
		{
			++m_sciTxWrites;
			NMMTRACE(io, "68k: SCI TX $%02x pc=$%06x", _val & 0xff, m_prevPC);
		}
		Mc68k::write16(_addr, _val);
	}
}
