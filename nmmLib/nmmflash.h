#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nmmtypes.h"

namespace nmm
{
	// The second flash at $300000: OS image + patch storage. The OS identifies the chip through the
	// JEDEC autoselect command and halts unless it finds manufacturer $01 / device $D5, an AMD
	// Am29F080 (1 MB, sixteen 64K sectors, byte wide). The boot ROM reads an OS length at +8 and
	// copies the image at +$20 into RAM.
	class Flash
	{
	public:
		static constexpr uint8_t ManufacturerId = 0x01;
		static constexpr uint8_t DeviceId = 0xd5;
		static constexpr uint32_t SectorSize = 0x10000;

		Flash();

		static bool isInRange(const uint32_t _addr)
		{
			return _addr >= g_flashAddress && _addr < g_flashAddress + g_flashWindowSize;
		}

		// build a fresh flash holding the given OS image
		void createFromOs(const std::vector<uint8_t>& _os, uint8_t _versionMajor, uint8_t _versionMinor);

		bool load(const std::string& _filename);
		bool save(const std::string& _filename) const;

		uint8_t read8(uint32_t _addr);
		uint16_t read16(uint32_t _addr);
		void write8(uint32_t _addr, uint8_t _val, uint32_t _pc);
		void write16(uint32_t _addr, uint16_t _val, uint32_t _pc);

		const std::vector<uint8_t>& data() const { return m_data; }
		std::vector<uint8_t>& data() { return m_data; }

		uint32_t getProgramCount() const { return m_programCount; }
		uint32_t getEraseCount() const { return m_eraseCount; }
		bool isDirty() const { return m_dirty; }
		void clearDirty() { m_dirty = false; }

	private:
		static uint32_t off(const uint32_t _addr) { return (_addr - g_flashAddress) & (g_flashChipSize - 1); }
		void command(uint32_t _off, uint8_t _val, uint32_t _pc);

		enum class State
		{
			Idle,
			Unlock1,		// AA written to 555
			Unlock2,		// 55 written to 2AA
			Program,		// A0: next write is the data
			EraseUnlock1,	// 80 written
			EraseUnlock2,	// AA
			EraseUnlock3,	// 55: next write selects chip (10) or sector (30) erase
		};

		std::vector<uint8_t> m_data;
		State m_state = State::Idle;
		bool m_autoselect = false;
		bool m_dirty = false;
		uint32_t m_programCount = 0;
		uint32_t m_eraseCount = 0;
	};
}
