#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <unordered_map>
#include <cmath>

#include "nmmLib/nmmhardware.h"
#include "nmmLib/nmmromloader.h"
#include "nmmLib/nmmlog.h"
#include "nmmLib/nmmpatch.h"

namespace
{
	bool writeWav16(const std::string& _filename, const std::vector<std::vector<int32_t>>& _channels, const uint32_t _samplerate)
	{
		if(_channels.empty() || _channels[0].empty())
			return false;

		const auto ch = static_cast<uint32_t>(_channels.size());
		size_t frames = _channels[0].size();
		for (const auto& c : _channels)
			frames = std::min(frames, c.size());

		std::vector<uint8_t> d;
		d.reserve(44 + frames * ch * 2);

		auto put16 = [&](const uint32_t v) { d.push_back(v & 0xff); d.push_back((v >> 8) & 0xff); };
		auto put32 = [&](const uint32_t v) { put16(v & 0xffff); put16(v >> 16); };
		auto putTag = [&](const char* t) { for(int i=0; i<4; ++i) d.push_back(static_cast<uint8_t>(t[i])); };

		const uint32_t dataSize = static_cast<uint32_t>(frames * ch * 2);
		putTag("RIFF"); put32(36 + dataSize); putTag("WAVE");
		putTag("fmt "); put32(16); put16(1); put16(ch); put32(_samplerate); put32(_samplerate * ch * 2); put16(ch * 2); put16(16);
		putTag("data"); put32(dataSize);

		for(size_t f=0; f<frames; ++f)
			for(uint32_t c=0; c<ch; ++c)
				put16(static_cast<uint16_t>(static_cast<int16_t>(std::max(-32768, std::min(32767, _channels[c][f])))));

		FILE* fp = std::fopen(_filename.c_str(), "wb");
		if(!fp)
			return false;
		std::fwrite(d.data(), 1, d.size(), fp);
		std::fclose(fp);
		return true;
	}

	void usage()
	{
		std::printf(
			"nmmConsole --os <osfile> [--boot <bootflash.bin>] [--dsp <slot>]... [--seconds <s>]\n"
			"           [--note <sec>:<note>] [--syx <sec>:<file>] [--knob <i>=<v>] [--press <off>=<v>]\n"
			"           [--wav <file>] [--midi-dump] [--profile] [--history] [--dump-ram <f>] [--dump-dsp <f>]\n"
			"           [--trace io,hdi,panel,flash,irq] [--trace-from <sec>] [--watch <hexaddr>:<hexsize>]\n"
			"  --os      Clavia OS update .exe or descrambled 68k image\n"
			"  --boot    512K boot flash dump; without it the OS is started directly in RAM\n"
			"  --dsp     attach a DSP56303 to host port slot n (repeatable, Micro Modular: 0)\n"
			"  --seconds audio seconds to render after boot (default 3)\n"
			"  --pch <sec>:<file> loads a Clavia .pch patch (3.0 or 2.10) and uploads it via the PC port\n"
			"  sysex files are sent to the PC port like the editor does, notes go to MIDI IN\n");
	}

	std::vector<uint8_t> readFile(const std::string& _file)
	{
		std::vector<uint8_t> bytes;
		if(FILE* f = std::fopen(_file.c_str(), "rb"))
		{
			uint8_t buf[4096]; size_t n;
			while((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
			std::fclose(f);
		}
		return bytes;
	}
}

int main(int _argc, char** _argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::string osFile, bootFile, dumpRam, wavFile, dumpDsp, traceFlags;
	std::vector<uint32_t> dspSlots;
	double seconds = 3.0;
	double traceFrom = -1.0;
	bool history = false, midiDump = false, profile = false;

	// scheduled MIDI: (sample frame, bytes)
	std::vector<std::pair<uint64_t, std::vector<uint8_t>>> midiSchedule;
	std::vector<std::pair<uint32_t,uint8_t>> panelInputs;
	std::vector<std::pair<uint32_t,uint8_t>> knobs;

	auto toFrames = [](const std::string& _s) { return static_cast<uint64_t>(std::stod(_s) * nmm::g_samplerate); };

	for(int i=1; i<_argc; ++i)
	{
		const std::string a = _argv[i];
		auto next = [&]() -> std::string { return i + 1 < _argc ? _argv[++i] : std::string(); };

		if(a == "--os") osFile = next();
		else if(a == "--boot") bootFile = next();
		else if(a == "--dsp") dspSlots.push_back(static_cast<uint32_t>(std::stoul(next(), nullptr, 0)));
		else if(a == "--seconds") seconds = std::stod(next());
		else if(a == "--history") history = true;
		else if(a == "--dump-ram") dumpRam = next();
		else if(a == "--wav") wavFile = next();
		else if(a == "--midi-dump") midiDump = true;
		else if(a == "--profile") profile = true;
		else if(a == "--trace-from") traceFrom = std::stod(next());
		else if(a == "--dump-dsp") dumpDsp = next();
		else if(a == "--trace") traceFlags = next();
		else if(a == "--knob")
		{
			const auto s = next();
			const auto eq = s.find('=');
			if(eq != std::string::npos)
				knobs.emplace_back(static_cast<uint32_t>(std::stoul(s.substr(0,eq))), static_cast<uint8_t>(std::stoul(s.substr(eq+1))));
		}
		else if(a == "--watch")
		{
			const auto s = next();
			const auto colon = s.find(':');
			nmm::Trace::watchAddr = static_cast<uint32_t>(std::stoul(s.substr(0, colon), nullptr, 16));
			nmm::Trace::watchSize = colon == std::string::npos ? 1 : static_cast<uint32_t>(std::stoul(s.substr(colon + 1), nullptr, 16));
		}
		else if(a == "--note")
		{
			// --note <sec>:<note>  note on, note off one second later
			const auto s = next();
			const auto colon = s.find(':');
			const auto at = toFrames(s.substr(0, colon));
			const auto note = static_cast<uint8_t>(std::stoul(s.substr(colon + 1)));
			midiSchedule.emplace_back(at, std::vector<uint8_t>{0x90, note, 0x64});
			midiSchedule.emplace_back(at + nmm::g_samplerate, std::vector<uint8_t>{0x80, note, 0x00});
		}
		else if(a == "--syx")
		{
			// --syx <sec>:<file>  sends the raw bytes of a .syx file to the PC port
			const auto s = next();
			const auto colon = s.find(':');
			const auto at = toFrames(s.substr(0, colon));
			const auto file = s.substr(colon + 1);
			auto bytes = readFile(file);
			if(bytes.empty()) { std::printf("failed to read %s\n", file.c_str()); return 1; }
			midiSchedule.emplace_back(at, std::move(bytes));
		}
		else if(a == "--pch")
		{
			// --pch <sec>:<file>  loads a .pch patch file and schedules the IAm + upload messages
			const auto s = next();
			const auto colon = s.find(':');
			const auto at = toFrames(s.substr(0, colon));
			const auto file = s.substr(colon + 1);
			nmm::Patch patch;
			std::string err;
			if(!nmm::PchFile::load(file, patch, err)) { std::printf("failed to load %s: %s\n", file.c_str(), err.c_str()); return 1; }
			const auto frames = nmm::PatchSysex::upload(patch, 0);
			size_t total = 0; for (const auto& f : frames) total += f.size();
			std::printf("pch: '%s' %zu poly + %zu common modules, %zu + %zu cables, %zu voices -> %zu packets, %zu bytes\n", patch.name.c_str(),
				patch.area(1).modules.size(), patch.area(0).modules.size(), patch.area(1).cables.size(), patch.area(0).cables.size(), static_cast<size_t>(patch.header.voices), frames.size(), total);
			midiSchedule.emplace_back(at, nmm::PatchSysex::iAm());
			for(size_t k=0; k<frames.size(); ++k)
				midiSchedule.emplace_back(at + nmm::g_samplerate / 10 + k * (nmm::g_samplerate / 10), frames[k]);
		}
		else if(a == "--press")
		{
			const auto s = next();
			const auto eq = s.find('=');
			if(eq != std::string::npos)
				panelInputs.emplace_back(static_cast<uint32_t>(std::stoul(s.substr(0,eq), nullptr, 0)), static_cast<uint8_t>(std::stoul(s.substr(eq+1), nullptr, 0)));
		}
		else { usage(); return 1; }
	}

	if(osFile.empty())
	{
		usage();
		return 1;
	}

	std::sort(midiSchedule.begin(), midiSchedule.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

	const auto watchSizeRequested = nmm::Trace::watchSize;
	nmm::Trace::watchSize = 0;
	auto applyTrace = [&]()
	{
		nmm::Trace::watchSize = watchSizeRequested;
		const auto& s = traceFlags;
		if(s.find("io") != std::string::npos) nmm::Trace::io = true;
		if(s.find("hdi") != std::string::npos) nmm::Trace::hdi = true;
		if(s.find("panel") != std::string::npos) nmm::Trace::panel = true;
		if(s.find("flash") != std::string::npos) nmm::Trace::flash = true;
		if(s.find("irq") != std::string::npos) nmm::Trace::irq = true;
	};
	if(traceFrom < 0)
		applyTrace();

	nmm::HardwareConfig cfg;
	cfg.os = nmm::RomLoader::loadOs(osFile);
	if(!cfg.os.isValid())
	{
		std::printf("failed to load OS from '%s'\n", osFile.c_str());
		return 1;
	}
	std::printf("OS: product='%s' v%u.%02u size=%zu\n", cfg.os.product.c_str(), cfg.os.versionMajor, cfg.os.versionMinor, cfg.os.data.size());

	if(!bootFile.empty())
	{
		if(!nmm::RomLoader::loadBootRom(bootFile, cfg.bootRom))
		{
			std::printf("failed to load boot rom '%s'\n", bootFile.c_str());
			return 1;
		}
		std::printf("boot rom: %zu bytes\n", cfg.bootRom.size());
	}
	cfg.dspSlots = dspSlots;

	const auto t0 = std::chrono::steady_clock::now();

	// knobs must be set before construction, the OS reads them during init
	std::unique_ptr<nmm::Hardware> hwPtr;
	{
		// construction boots the machine and blocks until the DSP produces audio
		hwPtr.reset(new nmm::Hardware(cfg));
	}
	auto& hw = *hwPtr;
	if(!hw.isValid())
		return 1;

	auto& uc = hw.getUC();
	uc.enablePcHistory(history);
	hw.enableDspProfile(profile);

	for (const auto& [off, val] : panelInputs)
		uc.getPanel().setInput(off, val);
	for (const auto& [idx, val] : knobs)
		uc.setKnob(idx, val);

	const auto tBoot = std::chrono::steady_clock::now();
	std::printf("boot finished in %lld ms (68k instructions=%llu, os started=%d)\n",
		static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(tBoot - t0).count()),
		static_cast<unsigned long long>(uc.getInstructionCount()), hw.osStarted() ? 1 : 0);

	constexpr uint32_t blockSize = 256;
	const auto totalFrames = static_cast<uint64_t>(seconds * nmm::g_samplerate);

	std::vector<std::vector<int32_t>> capture(2);
	std::vector<float> outL(blockSize), outR(blockSize);
	synthLib::TAudioInputs ins{};
	synthLib::TAudioOutputs outs{};
	outs[0] = outL.data();
	outs[1] = outR.data();

	uint64_t frame = 0;
	uint64_t lastReport = 0;
	bool traceApplied = traceFrom < 0;

	while(frame < totalFrames)
	{
		if(!traceApplied && frame >= static_cast<uint64_t>(traceFrom * nmm::g_samplerate))
		{
			applyTrace();
			traceApplied = true;
		}

		// schedule MIDI that falls into this block
		while(!midiSchedule.empty() && midiSchedule.front().first < frame + blockSize)
		{
			auto& [at, bytes] = midiSchedule.front();
			const bool sysex = !bytes.empty() && bytes[0] == 0xf0;
			std::printf("%s in  @%.3fs:", sysex ? "PC  " : "MIDI", static_cast<double>(at) / nmm::g_samplerate);
			for (size_t k=0; k<bytes.size() && k<24; ++k) std::printf(" %02x", bytes[k]);
			if(bytes.size() > 24) std::printf(" ... (%zu bytes)", bytes.size());
			std::printf("\n");

			synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);
			ev.offset = static_cast<uint32_t>(at);
			if(sysex)
				ev.sysex.assign(bytes.begin(), bytes.end());
			else
			{
				ev.a = bytes[0];
				ev.b = bytes.size() > 1 ? bytes[1] : 0;
				ev.c = bytes.size() > 2 ? bytes[2] : 0;
			}
			hw.sendMidi(ev);
			midiSchedule.erase(midiSchedule.begin());
		}

		hw.processAudio(ins, outs, blockSize, 0);
		frame += blockSize;

		const auto& raw = hw.getAudioOutputs();
		for(uint32_t i=0; i<blockSize; ++i)
		{
			capture[0].push_back(static_cast<int16_t>(raw[0][i] & 0xffff));
			capture[1].push_back(static_cast<int16_t>(raw[1][i] & 0xffff));
		}

		if(midiDump)
		{
			std::vector<uint8_t> midiOut, pcOut;
			hw.readMidiOut(midiOut, pcOut);
			if(!midiOut.empty())
			{
				std::printf("MIDI out @%.3fs:", static_cast<double>(frame) / nmm::g_samplerate);
				for (const auto b : midiOut) std::printf(" %02x", b);
				std::printf("\n");
			}
			if(!pcOut.empty())
			{
				std::printf("PC   out @%.3fs:", static_cast<double>(frame) / nmm::g_samplerate);
				for (const auto b : pcOut) std::printf(" %02x", b);
				std::printf("\n");
			}
		}

		if(frame - lastReport >= nmm::g_samplerate)
		{
			lastReport = frame;
			const auto now = std::chrono::steady_clock::now();
			std::printf("... %.1f s rendered in %.2f s wall, 68k pc=$%06x instr=%llu\n", static_cast<double>(frame) / nmm::g_samplerate,
				std::chrono::duration<double>(now - tBoot).count(), uc.getPC(), static_cast<unsigned long long>(uc.getInstructionCount()));
		}
	}

	const auto t1 = std::chrono::steady_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - tBoot).count();

	std::printf("\n=== done: %.2f s audio in %lld ms (%.2fx realtime), 68k instructions=%llu cycles=%llu pc=$%06x\n",
		seconds, static_cast<long long>(ms), seconds * 1000.0 / std::max<double>(1.0, static_cast<double>(ms)),
		static_cast<unsigned long long>(uc.getInstructionCount()), static_cast<unsigned long long>(uc.getCycles()), uc.getPC());

	std::printf("sr=$%04x irqs=%llu sci tx writes=%llu\n", uc.getSR(), static_cast<unsigned long long>(uc.getIrqCount()), static_cast<unsigned long long>(uc.getSciTxWrites()));

	std::printf("host port slot access counts:");
	const auto& counts = uc.getDspSlots().accessCounts();
	for(uint32_t s=0; s<counts.size(); ++s)
		if(counts[s]) std::printf(" [%u]=%u", s, counts[s]);
	std::printf("\n");

	for(size_t d=0; d<hw.getDspCount(); ++d)
	{
		auto& dsp = hw.getDSP(d);
		std::printf("%s: booted=%d words host->dsp=%llu dsp->host=%llu", dsp.getName().c_str(), dsp.isBooted() ? 1 : 0,
			static_cast<unsigned long long>(dsp.getHostWordsToDsp()), static_cast<unsigned long long>(dsp.getDspWordsToHost()));
		if(dsp.isBooted())
			std::printf(" dsp pc=$%06x instructions=%llu hcr=$%06x hsr=$%06x", dsp.dsp().getPC().var, static_cast<unsigned long long>(dsp.dsp().getInstructionCounter()),
				dsp.hdi08().readControlRegister(), dsp.hdi08().readStatusRegister());
		std::printf("\n");

		if(!dumpDsp.empty() && d == 0)
		{
			auto& mem = dsp.dsp().memory();
			std::vector<uint8_t> out;
			for(uint32_t p=0; p<0x10000; ++p)
			{
				const auto w = mem.get(dsp56k::MemArea_P, p);
				out.push_back((w >> 16) & 0xff); out.push_back((w >> 8) & 0xff); out.push_back(w & 0xff);
			}
			if(FILE* f = std::fopen(dumpDsp.c_str(), "wb")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); std::printf("wrote %s (P:0-$ffff)\n", dumpDsp.c_str()); }
			for (const auto area : {dsp56k::MemArea_X, dsp56k::MemArea_Y})
			{
				const auto name = dumpDsp + (area == dsp56k::MemArea_X ? ".x.txt" : ".y.txt");
				if(FILE* f = std::fopen(name.c_str(), "w"))
				{
					for(uint32_t a=0; a<0x4000; ++a)
						std::fprintf(f, "%06x %06x\n", a, mem.get(area, a));
					std::fclose(f);
				}
			}
		}
	}
	std::printf("essi frames: %llu, irqd injected=%llu masked=%llu\n", static_cast<unsigned long long>(hw.getEssiFrameCount()), static_cast<unsigned long long>(hw.getIrqdInjected()), static_cast<unsigned long long>(hw.getIrqdMasked()));

	{
		std::printf("captured audio: %zu frames;", capture[0].size());
		for(uint32_t c=0; c<capture.size(); ++c)
		{
			int32_t peak = 0; uint64_t nonzero = 0;
			for (const auto v : capture[c]) { peak = std::max(peak, std::abs(v - 341)); if(v != 341) ++nonzero; }
			std::printf(" ch%u peak=%d nonzero=%llu", c, peak, static_cast<unsigned long long>(nonzero));
		}
		std::printf("\n");
		// per channel: dc, rms, autocorrelation pitch over the last second
		for(uint32_t c=0; c<capture.size(); ++c)
		{
			const auto& s = capture[c];
			if(s.size() < 4096) continue;
			const size_t n = std::min<size_t>(s.size(), nmm::g_samplerate);
			const size_t start = s.size() - n;
			double sum = 0, sq = 0; for(size_t k=start; k<s.size(); ++k) { sum += s[k]; sq += double(s[k]) * s[k]; }
			const double dc = sum / double(n), rms = std::sqrt(sq / double(n) - dc * dc);
			// autocorrelation over 20 Hz .. 4 kHz
			double best = -1; size_t bestLag = 0;
			const size_t win = std::min<size_t>(n / 2, 8192);
			for(size_t lag = nmm::g_samplerate / 4000; lag < nmm::g_samplerate / 20 && lag < n - win; ++lag)
			{
				double acc = 0;
				for(size_t k=0; k<win; ++k) acc += (s[start+k] - dc) * (s[start+k+lag] - dc);
				if(acc > best) { best = acc; bestLag = lag; }
			}
			std::printf("  ch%u: dc=%.1f rms=%.1f (%.1f dBFS 16 bit) pitch~%.1f Hz\n", c, dc, rms, 20.0 * std::log10(rms / 32768.0 + 1e-12), bestLag ? double(nmm::g_samplerate) / double(bestLag) : 0.0);
		}

		if(!wavFile.empty())
		{
			std::vector<std::vector<int32_t>> out(capture.size());
			for(size_t c=0; c<capture.size(); ++c)
			{
				out[c].resize(capture[c].size());
				for(size_t k=0; k<capture[c].size(); ++k) out[c][k] = capture[c][k] - 341;
			}
			if(writeWav16(wavFile, out, nmm::g_samplerate))
				std::printf("wrote %s\n", wavFile.c_str());
		}
	}

	std::printf("panel latches:");
	for(uint32_t l=0; l<16; ++l) std::printf(" %02x", uc.getPanel().getLatch(l));
	std::printf("\n");

	if(profile && hw.getDspCount())
	{
		std::vector<std::pair<uint32_t,uint64_t>> v(hw.dspPcHistogram().begin(), hw.dspPcHistogram().end());
		std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.second > b.second; });
		uint64_t total = 0; for (auto& e : v) total += e.second;
		std::printf("dsp pc profile (top 24 of %zu, %llu samples):\n", v.size(), static_cast<unsigned long long>(total));
		for(size_t k=0; k<v.size() && k<24; ++k)
			std::printf("  $%06x %5.2f%%\n", v[k].first, 100.0 * static_cast<double>(v[k].second) / static_cast<double>(total));
	}

	if(history)
	{
		std::printf("pc history (oldest first):");
		const auto& h = uc.pcHistory();
		for(uint32_t k=0; k<h.size(); ++k)
		{
			if((k & 7) == 0) std::printf("\n  ");
			std::printf("$%06x ", h[(uc.pcHistoryPos() + k) & 63]);
		}
		std::printf("\n");
	}

	if(!dumpRam.empty())
	{
		FILE* f = std::fopen(dumpRam.c_str(), "wb");
		if(f)
		{
			std::fwrite(uc.romRam().data(), 1, uc.romRam().size(), f);
			std::fclose(f);
			std::printf("wrote %s\n", dumpRam.c_str());
		}
	}

	hwPtr.reset();
	std::printf("shutdown ok\n");
	return 0;
}
