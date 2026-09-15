#include "nmmromloader.h"

#include <algorithm>
#include <cstring>

#include "nmmtypes.h"
#include "nmmlog.h"

#include "baseLib/filesystem.h"

namespace nmm
{
	namespace
	{
		constexpr char g_updateHeader[] = "Clavia OS update file V1.0";
	}

	OsImage RomLoader::loadOs(const std::string& _filename)
	{
		std::vector<uint8_t> f;
		if(!baseLib::filesystem::readFile(f, _filename))
			return {};
		return parseOs(f);
	}

	OsImage RomLoader::parseOs(const std::vector<uint8_t>& _file)
	{
		OsImage img;

		// raw descrambled image?
		if(looksLike68kImage(_file))
		{
			img.data = _file;
			img.product = "raw";
			return img;
		}

		// search for the update payload: header string, product name, version, LE length, body
		const auto hdrLen = std::strlen(g_updateHeader) + 1;
		size_t pos = 0;

		while(true)
		{
			const auto it = std::search(_file.begin() + static_cast<std::ptrdiff_t>(pos), _file.end(), g_updateHeader, g_updateHeader + hdrLen);
			if(it == _file.end())
				break;

			const auto start = static_cast<size_t>(it - _file.begin());
			pos = start + 1;

			auto p = start + hdrLen;
			// product name, zero terminated
			const auto nameStart = p;
			while(p < _file.size() && _file[p] != 0 && p - nameStart < 64)
				++p;
			if(p >= _file.size() || _file[p] != 0 || p == nameStart)
				continue;
			const std::string product(reinterpret_cast<const char*>(&_file[nameStart]), p - nameStart);
			++p;

			if(p + 8 > _file.size())
				continue;

			const uint8_t major = _file[p];
			const uint8_t minor = _file[p + 1];
			p += 4;

			const uint32_t len = _file[p] | (_file[p+1] << 8) | (_file[p+2] << 16) | (static_cast<uint32_t>(_file[p+3]) << 24);
			p += 4;

			if(len < 0x10000 || len > 0x100000 || p + len > _file.size())
				continue;

			std::vector<uint8_t> body(_file.begin() + static_cast<std::ptrdiff_t>(p), _file.begin() + static_cast<std::ptrdiff_t>(p + len));
			descramble(body);

			if(!looksLike68kImage(body))
			{
				NMMLOG("RomLoader: payload for '%s' did not descramble to a 68k image", product.c_str());
				continue;
			}

			img.data = std::move(body);
			img.product = product;
			img.versionMajor = major;
			img.versionMinor = minor;
			NMMLOG("RomLoader: found OS update '%s' v%u.%02u, %u bytes", product.c_str(), major, minor, len);
			return img;
		}

		return {};
	}

	void RomLoader::descramble(std::vector<uint8_t>& _body)
	{
		for(size_t i=0; i<_body.size(); ++i)
		{
			const auto key = static_cast<uint8_t>(0x11 * (i + 1));
			auto v = static_cast<uint8_t>(_body[i] ^ key);
			v = static_cast<uint8_t>(((v & 0x55) << 1) | ((v & 0xaa) >> 1));
			_body[i] = v;
		}
	}

	bool RomLoader::looksLike68kImage(const std::vector<uint8_t>& _data)
	{
		// both OS variants start with: move.w #$2700,sr ; movea.l #$200000,a7
		static constexpr uint8_t prefix[] = {0x46, 0xfc, 0x27, 0x00, 0x2e, 0x7c, 0x00, 0x20, 0x00, 0x00};
		if(_data.size() < 0x10000)
			return false;
		return std::equal(std::begin(prefix), std::end(prefix), _data.begin());
	}

	bool RomLoader::loadBootRom(const std::string& _filename, std::vector<uint8_t>& _out)
	{
		if(!baseLib::filesystem::readFile(_out, _filename))
			return false;
		if(_out.size() != g_bootRomSize)
		{
			NMMLOG("RomLoader: boot rom '%s' has unexpected size %zu", _filename.c_str(), _out.size());
			_out.clear();
			return false;
		}
		// reset vector sanity: initial SP must point into RAM, PC into the boot rom
		const uint32_t sp = (_out[0] << 24) | (_out[1] << 16) | (_out[2] << 8) | _out[3];
		const uint32_t pc = (_out[4] << 24) | (_out[5] << 16) | (_out[6] << 8) | _out[7];
		if(sp < g_ramAddress || sp > g_ramAddress + g_ramSize || pc >= g_bootRomSize)
		{
			NMMLOG("RomLoader: boot rom '%s' has implausible reset vector sp=$%08x pc=$%08x", _filename.c_str(), sp, pc);
			_out.clear();
			return false;
		}
		return true;
	}
}
