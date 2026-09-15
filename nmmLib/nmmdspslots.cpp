#include "nmmdspslots.h"

#include "nmmlog.h"

namespace nmm
{
	DspSlots::DspSlots()
	{
		for (auto& s : m_slots)
		{
			// a slot without a DSP behind it always looks ready so that boot code does not spin forever
			s.setReadIsrCallback([](const uint8_t _isr)
			{
				return static_cast<uint8_t>(_isr | mc68k::Hdi08::IsrBits::Trdy | mc68k::Hdi08::IsrBits::Txde);
			});
		}
	}

	uint8_t DspSlots::read8(const uint32_t _addr, const uint32_t _pc)
	{
		const auto i = slotIndex(_addr);
		++m_accessCounts[i];
		const auto v = m_slots[i].read8(localAddr(_addr));
		NMMTRACE(hdi, "HDI slot %2u rd8  $%06x -> $%02x  pc=$%06x", i, _addr, v, _pc);
		return v;
	}

	uint16_t DspSlots::read16(const uint32_t _addr, const uint32_t _pc)
	{
		const auto i = slotIndex(_addr);
		++m_accessCounts[i];
		const auto v = m_slots[i].read16(localAddr(_addr));
		NMMTRACE(hdi, "HDI slot %2u rd16 $%06x -> $%04x  pc=$%06x", i, _addr, v, _pc);
		return v;
	}

	void DspSlots::write8(const uint32_t _addr, const uint8_t _val, const uint32_t _pc)
	{
		const auto i = slotIndex(_addr);
		++m_accessCounts[i];
		NMMTRACE(hdi, "HDI slot %2u wr8  $%06x <- $%02x  pc=$%06x", i, _addr, _val, _pc);
		m_slots[i].write8(localAddr(_addr), _val);
		if(localAddr(_addr) == mc68k::PeriphAddress::HdiICR && m_icrCallbacks[i])
			m_icrCallbacks[i](_val);
	}

	void DspSlots::write16(const uint32_t _addr, const uint16_t _val, const uint32_t _pc)
	{
		const auto i = slotIndex(_addr);
		++m_accessCounts[i];
		NMMTRACE(hdi, "HDI slot %2u wr16 $%06x <- $%04x  pc=$%06x", i, _addr, _val, _pc);
		m_slots[i].write16(localAddr(_addr), _val);
		if(localAddr(_addr) == mc68k::PeriphAddress::HdiICR && m_icrCallbacks[i])
			m_icrCallbacks[i](static_cast<uint8_t>(_val >> 8));
	}

	void DspSlots::exec(const uint32_t _deltaCycles)
	{
		for (auto& s : m_slots)
			s.exec(_deltaCycles);
	}
}
