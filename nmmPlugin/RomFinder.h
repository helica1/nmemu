#pragma once

#include <string>
#include <vector>

namespace nmm
{
	// Where the plugin looks for the Nord Modular OS (a Clavia "OS Update.exe" or a descrambled
	// image) and the optional 512K boot flash dump. Search order:
	//   $NMEMU_ROM_DIR, ~/Documents/nmemu, ~/Library/Application Support/nmemu, the plugin's own folder
	class RomFinder
	{
	public:
		struct Result
		{
			std::string osFile;
			std::vector<uint8_t> osData;		// raw file, parsed by RomLoader
			std::string bootRomFile;
			std::vector<std::string> searched;
			bool valid() const { return !osFile.empty(); }
		};

		static std::vector<std::string> getSearchDirectories();
		static Result find();
	};
}
