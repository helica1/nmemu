#include "RomFinder.h"

#include <cstdlib>

#include <juce_core/juce_core.h>

#include "nmmLib/nmmromloader.h"
#include "nmmLib/nmmdevice.h"

namespace nmm
{
	std::vector<std::string> RomFinder::getSearchDirectories()
	{
		std::vector<std::string> dirs;

		if(const auto* env = std::getenv("NMEMU_ROM_DIR"))
			if(*env) dirs.emplace_back(env);

		const auto docs = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("nmemu");
		dirs.push_back(docs.getFullPathName().toStdString());

		const auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("nmemu");
		dirs.push_back(appData.getFullPathName().toStdString());

		const auto module = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
		dirs.push_back(module.getFullPathName().toStdString());

		return dirs;
	}

	RomFinder::Result RomFinder::find()
	{
		Result r;

		for (const auto& dir : getSearchDirectories())
		{
			r.searched.push_back(dir);

			const juce::File d(dir);
			if(!d.isDirectory())
				continue;

			for (const auto& f : d.findChildFiles(juce::File::findFiles, false))
			{
				const auto ext = f.getFileExtension().toLowerCase();
				if(ext != ".exe" && ext != ".bin" && ext != ".os" && ext != ".rom")
					continue;
				if(f.getSize() < 64 * 1024 || f.getSize() > 4 * 1024 * 1024)
					continue;

				juce::MemoryBlock mb;
				if(!f.loadFileAsData(mb))
					continue;

				std::vector<uint8_t> data(static_cast<const uint8_t*>(mb.getData()), static_cast<const uint8_t*>(mb.getData()) + mb.getSize());

				if(r.osFile.empty())
				{
					const auto os = RomLoader::parseOs(data);
					if(os.isValid())
					{
						r.osFile = f.getFullPathName().toStdString();
						r.osData = std::move(data);
						continue;
					}
				}
				if(r.bootRomFile.empty() && ext == ".bin")
				{
					std::vector<uint8_t> boot;
					if(RomLoader::loadBootRom(f.getFullPathName().toStdString(), boot))
						r.bootRomFile = f.getFullPathName().toStdString();
				}
			}

			if(r.valid())
				break;
		}
		return r;
	}
}
