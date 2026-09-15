#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nmm
{
	struct OsImage
	{
		std::vector<uint8_t> data;	// descrambled 68k image, to be placed at $100000
		std::string product;		// "Micro Modular" or "Modular"
		uint8_t versionMajor = 0;
		uint8_t versionMinor = 0;
		bool isValid() const { return !data.empty(); }
	};

	class RomLoader
	{
	public:
		// Accepts either a stock Clavia "OS Update.exe" (the payload is extracted and descrambled)
		// or an already descrambled raw image.
		static OsImage loadOs(const std::string& _filename);
		static OsImage parseOs(const std::vector<uint8_t>& _file);

		// Undo Clavia's update payload obfuscation: byte i is XORed with 0x11*(i+1), then adjacent
		// bit pairs are swapped in every byte.
		static void descramble(std::vector<uint8_t>& _body);

		static bool looksLike68kImage(const std::vector<uint8_t>& _data);

		static bool loadBootRom(const std::string& _filename, std::vector<uint8_t>& _out);
	};
}
