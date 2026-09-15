#include "nmmflash.h"

#include <algorithm>

#include "nmmlog.h"

#include "baseLib/filesystem.h"

namespace nmm
{
	Flash::Flash()
		: m_data(g_flashChipSize, 0xff)
	{
	}

	void Flash::createFromOs(const std::vector<uint8_t>& _os, const uint8_t _versionMajor, const uint8_t _versionMinor)
	{
		std::fill(m_data.begin(), m_data.end(), 0xff);

		// header: everything unknown is zero
		for(uint32_t i=0; i<g_flashOsDataOffset; ++i)
			m_data[i] = 0;

		m_data[g_flashOsVersionOffset]		= _versionMajor;
		m_data[g_flashOsVersionOffset + 1]	= _versionMinor;

		const auto len = static_cast<uint32_t>(_os.size());
		m_data[g_flashOsLengthOffset    ] = static_cast<uint8_t>(len >> 24);
		m_data[g_flashOsLengthOffset + 1] = static_cast<uint8_t>(len >> 16);
		m_data[g_flashOsLengthOffset + 2] = static_cast<uint8_t>(len >> 8);
		m_data[g_flashOsLengthOffset + 3] = static_cast<uint8_t>(len);

		std::copy(_os.begin(), _os.end(), m_data.begin() + g_flashOsDataOffset);
		m_dirty = false;
	}

	bool Flash::load(const std::string& _filename)
	{
		std::vector<uint8_t> d;
		if(!baseLib::filesystem::readFile(d, _filename))
			return false;
		if(d.size() != g_flashChipSize)
			return false;
		m_data = d;
		m_dirty = false;
		return true;
	}

	bool Flash::save(const std::string& _filename) const
	{
		return baseLib::filesystem::writeFile(_filename, m_data);
	}

	uint8_t Flash::read8(const uint32_t _addr)
	{
		const auto o = off(_addr);

		if(m_autoselect)
		{
			switch (o & 3)
			{
			case 0:		return ManufacturerId;
			case 1:		return DeviceId;
			default:	return 0;	// sector protection: unprotected
			}
		}
		return m_data[o];
	}

	uint16_t Flash::read16(const uint32_t _addr)
	{
		return static_cast<uint16_t>(read8(_addr) << 8) | read8(_addr + 1);
	}

	void Flash::write8(const uint32_t _addr, const uint8_t _val, const uint32_t _pc)
	{
		NMMTRACE(flash, "FLASH wr8  $%06x <- $%02x  pc=$%06x", _addr, _val, _pc);
		command(off(_addr), _val, _pc);
	}

	void Flash::write16(const uint32_t _addr, const uint16_t _val, const uint32_t _pc)
	{
		NMMTRACE(flash, "FLASH wr16 $%06x <- $%04x  pc=$%06x", _addr, _val, _pc);
		// byte wide chip on the low data lanes, a word write ends up as the low byte
		command(off(_addr), static_cast<uint8_t>(_val & 0xff), _pc);
	}

	void Flash::command(const uint32_t _off, const uint8_t _val, const uint32_t _pc)
	{
		const auto a = _off & 0x7ff;

		// reset command works in any state
		if(_val == 0xf0 && m_state != State::Program)
		{
			m_state = State::Idle;
			m_autoselect = false;
			return;
		}

		switch (m_state)
		{
		case State::Idle:
			if(a == 0x555 && _val == 0xaa)
				m_state = State::Unlock1;
			return;

		case State::Unlock1:
			m_state = (a == 0x2aa && _val == 0x55) ? State::Unlock2 : State::Idle;
			return;

		case State::Unlock2:
			if(a != 0x555)
			{
				m_state = State::Idle;
				return;
			}
			switch (_val)
			{
			case 0x90:	m_autoselect = true; m_state = State::Idle; NMMTRACE(flash, "FLASH autoselect pc=$%06x", _pc); return;
			case 0xa0:	m_state = State::Program; return;
			case 0x80:	m_state = State::EraseUnlock1; return;
			default:	m_state = State::Idle; return;
			}

		case State::Program:
			// programming can only clear bits
			m_data[_off] &= _val;
			++m_programCount;
			m_dirty = true;
			m_state = State::Idle;
			return;

		case State::EraseUnlock1:
			m_state = (a == 0x555 && _val == 0xaa) ? State::EraseUnlock2 : State::Idle;
			return;

		case State::EraseUnlock2:
			m_state = (a == 0x2aa && _val == 0x55) ? State::EraseUnlock3 : State::Idle;
			return;

		case State::EraseUnlock3:
			if(_val == 0x10 && a == 0x555)
			{
				NMMLOG("FLASH chip erase pc=$%06x", _pc);
				std::fill(m_data.begin(), m_data.end(), 0xff);
				++m_eraseCount;
				m_dirty = true;
			}
			else if(_val == 0x30)
			{
				const auto sector = _off & ~(SectorSize - 1);
				NMMLOG("FLASH sector erase $%05x pc=$%06x", sector, _pc);
				std::fill(m_data.begin() + sector, m_data.begin() + sector + SectorSize, 0xff);
				++m_eraseCount;
				m_dirty = true;
			}
			m_state = State::Idle;
			return;
		}
	}
}
