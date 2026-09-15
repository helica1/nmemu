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
				put16(static_cast<uint16_t>(static_cast<int16_t>(std::max(-32768, std::min(32767, _channels[c][f])))));	// DAC words are 16 bit

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
			"nmmConsole --os <osfile> [--boot <bootflash.bin>] [--dsp <slot>]... [--steps <n>]\n"
			"           [--trace io,hdi,panel,flash] [--history] [--dump-ram <file>] [--press <offset>=<value>]\n"
			"  --os     Clavia OS update .exe or descrambled 68k image\n"
			"  --boot   512K boot flash dump; without it the OS is started directly in RAM\n"
			"  --dsp    attach a DSP56303 to host port slot n (repeatable)\n"
			"  --steps  number of 68k instructions to run (default 20 million)\n");
	}
}

int main(int _argc, char** _argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	std::string osFile, bootFile, dumpRam, wavFile;
	std::vector<uint32_t> dspSlots;
	uint64_t steps = 20'000'000;
	bool history = false;
	bool midiDump = false;
	bool profile = false;
	uint64_t traceFrom = 0;
	std::string traceFlags, dumpDsp, rawAudio;
	std::unordered_map<uint32_t, uint64_t> pcHist;
	// scheduled MIDI: (instruction index, bytes)
	std::vector<std::pair<uint64_t, std::vector<uint8_t>>> midiSchedule;
	std::vector<std::pair<uint32_t,uint8_t>> panelInputs;
	std::vector<std::pair<uint32_t,uint8_t>> knobs;

	for(int i=1; i<_argc; ++i)
	{
		const std::string a = _argv[i];
		auto next = [&]() -> std::string { return i + 1 < _argc ? _argv[++i] : std::string(); };

		if(a == "--os") osFile = next();
		else if(a == "--boot") bootFile = next();
		else if(a == "--dsp") dspSlots.push_back(static_cast<uint32_t>(std::stoul(next(), nullptr, 0)));
		else if(a == "--steps") steps = std::stoull(next());
		else if(a == "--history") history = true;
		else if(a == "--dump-ram") dumpRam = next();
		else if(a == "--wav") wavFile = next();
		else if(a == "--midi-dump") midiDump = true;
		else if(a == "--profile") profile = true;
		else if(a == "--trace-from") traceFrom = std::stoull(next());
		else if(a == "--dump-dsp") dumpDsp = next();
		else if(a == "--raw-audio") rawAudio = next();
		else if(a == "--knob")
		{
			// --knob <index>=<value 0-255>
			const auto s = next();
			const auto eq = s.find('=');
			if(eq != std::string::npos)
				knobs.emplace_back(static_cast<uint32_t>(std::stoul(s.substr(0,eq))), static_cast<uint8_t>(std::stoul(s.substr(eq+1))));
		}
		else if(a == "--watch")
		{
			// --watch <addr>:<size>, hex; enabled together with --trace-from
			const auto s = next();
			const auto colon = s.find(':');
			nmm::Trace::watchAddr = static_cast<uint32_t>(std::stoul(s.substr(0, colon), nullptr, 16));
			nmm::Trace::watchSize = colon == std::string::npos ? 1 : static_cast<uint32_t>(std::stoul(s.substr(colon + 1), nullptr, 16));
		}
		else if(a == "--note")
		{
			// --note <instruction>:<note>  sends note on, and note off 2M instructions later
			const auto s = next();
			const auto colon = s.find(':');
			const uint64_t at = std::stoull(s.substr(0, colon));
			const auto note = static_cast<uint8_t>(std::stoul(s.substr(colon + 1)));
			midiSchedule.emplace_back(at, std::vector<uint8_t>{0x90, note, 0x64});
			midiSchedule.emplace_back(at + 2'000'000, std::vector<uint8_t>{0x80, note, 0x00});
		}
		else if(a == "--syx")
		{
			// --syx <instruction>:<file>  sends the raw bytes of a .syx file
			const auto s = next();
			const auto colon = s.find(':');
			const uint64_t at = std::stoull(s.substr(0, colon));
			const auto file = s.substr(colon + 1);
			std::vector<uint8_t> bytes;
			if(FILE* f = std::fopen(file.c_str(), "rb"))
			{
				uint8_t buf[4096]; size_t n;
				while((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
				std::fclose(f);
			}
			if(bytes.empty()) { std::printf("failed to read %s\n", file.c_str()); return 1; }
			midiSchedule.emplace_back(at, std::move(bytes));
		}
		else if(a == "--press")
		{
			const auto s = next();
			const auto eq = s.find('=');
			if(eq != std::string::npos)
				panelInputs.emplace_back(static_cast<uint32_t>(std::stoul(s.substr(0,eq), nullptr, 0)), static_cast<uint8_t>(std::stoul(s.substr(eq+1), nullptr, 0)));
		}
		else if(a == "--trace") traceFlags = next();
		else { usage(); return 1; }
	}

	if(osFile.empty())
	{
		usage();
		return 1;
	}

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
	if(traceFrom == 0)
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

	nmm::Hardware hw(cfg);
	if(!hw.isValid())
		return 1;

	auto& uc = hw.getUC();
	uc.enablePcHistory(history);
	hw.enableDspProfile(profile);

	for (const auto& [off, val] : panelInputs)
		uc.getPanel().setInput(off, val);
	for (const auto& [idx, val] : knobs)
		uc.setKnob(idx, val);

	const auto t0 = std::chrono::steady_clock::now();

	uint64_t lastReport = 0;
	for(uint64_t i=0; i<steps; ++i)
	{
		hw.stepUC();

		if(profile && (i & 63) == 0)
			++pcHist[uc.getPC()];

		if(traceFrom && i == traceFrom)
			applyTrace();

		if((i & 4095) == 0)
		{
			hw.serviceAudio();

			// ~4096 instructions at ~21 MHz / 3.3 cycles per instruction is roughly 650 us, call it 64 samples
			hw.getMidi().process(64);

			for (auto it = midiSchedule.begin(); it != midiSchedule.end();)
			{
				if(it->first <= i)
				{
					// sysex goes to the PC port like the editor would, everything else to MIDI IN
					const bool sysex = !it->second.empty() && it->second[0] == 0xf0;
					std::printf("%s in  @%llu:", sysex ? "PC  " : "MIDI", static_cast<unsigned long long>(i));
					for (size_t k=0; k<it->second.size() && k<24; ++k) std::printf(" %02x", it->second[k]);
					if(it->second.size() > 24) std::printf(" ... (%zu bytes)", it->second.size());
					std::printf("\n");
					if(sysex)
						hw.getPcPort().write(it->second);
					else
						hw.getMidi().write(it->second);
					it = midiSchedule.erase(it);
				}
				else
					++it;
			}

			if(midiDump)
			{
				std::vector<uint8_t> out;
				hw.getMidi().read(out);
				if(!out.empty())
				{
					std::printf("MIDI out @%llu:", static_cast<unsigned long long>(i));
					for (const auto b : out) std::printf(" %02x", b);
					std::printf("\n");
				}
				hw.getPcPort().read(out);
				if(!out.empty())
				{
					std::printf("PC   out @%llu:", static_cast<unsigned long long>(i));
					for (const auto b : out) std::printf(" %02x", b);
					std::printf("\n");
				}
			}
		}

		if(i - lastReport >= 1'000'000)
		{
			lastReport = i;
			std::printf("... %llu instructions, pc=$%06x\n", static_cast<unsigned long long>(i), uc.getPC());
		}
	}

	const auto t1 = std::chrono::steady_clock::now();
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

	std::printf("\n=== done: %llu instructions in %lld ms, final pc=$%06x, 68k cycles=%llu, os started=%d\n",
		static_cast<unsigned long long>(steps), static_cast<long long>(ms), uc.getPC(), static_cast<unsigned long long>(uc.getCycles()), hw.osStarted() ? 1 : 0);

	std::printf("sr=$%04x irqs=%llu sci tx writes=%llu\n", uc.getSR(), static_cast<unsigned long long>(uc.getIrqCount()), static_cast<unsigned long long>(uc.getSciTxWrites()));
	std::printf("registers: ");
	for(uint32_t r=0; r<8; ++r) std::printf("d%u=$%08x ", r, uc.getDReg(r));
	std::printf("\n           ");
	for(uint32_t r=0; r<8; ++r) std::printf("a%u=$%08x ", r, uc.getAReg(r));
	std::printf("\n");

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
			// P memory as big endian 24 bit words, one per 4 bytes
			auto& mem = dsp.dsp().memory();
			std::vector<uint8_t> out;
			for(uint32_t p=0; p<0x10000; ++p)
			{
				const auto w = mem.get(dsp56k::MemArea_P, p);
				out.push_back((w >> 16) & 0xff); out.push_back((w >> 8) & 0xff); out.push_back(w & 0xff);
			}
			if(FILE* f = std::fopen(dumpDsp.c_str(), "wb")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); std::printf("wrote %s (P:0-$ffff)\n", dumpDsp.c_str()); }
			// X and Y memory as text, one word per line
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
		const auto& cap = hw.getCapture();
		std::printf("captured audio: %zu frames;", cap[0].size());
		for(uint32_t c=0; c<cap.size(); ++c)
		{
			int32_t peak = 0; uint64_t nonzero = 0;
			for (const auto v : cap[c]) { peak = std::max(peak, std::abs(v)); if(v) ++nonzero; }
			std::printf(" ch%u peak=%d nonzero=%llu", c, peak, static_cast<unsigned long long>(nonzero));
		}
		std::printf("\n");
		// per channel: dc, rms, zero crossing frequency over the last 2 seconds
		for(uint32_t c=0; c<cap.size(); ++c)
		{
			const auto& s = cap[c];
			if(s.size() < 4096) continue;
			const size_t n = std::min<size_t>(s.size(), nmm::g_samplerate * 2);
			const size_t start = s.size() - n;
			double sum = 0, sq = 0; for(size_t k=start; k<s.size(); ++k) { sum += s[k]; sq += double(s[k]) * s[k]; }
			const double dc = sum / double(n), rms = std::sqrt(sq / double(n));
			size_t zc = 0; for(size_t k=start+1; k<s.size(); ++k) if((s[k-1] - dc < 0) != (s[k] - dc < 0)) ++zc;
			std::printf("  ch%u: dc=%.1f rms=%.1f (%.1f dBFS) zero-crossing freq=%.1f Hz\n", c, dc, rms, 20.0 * std::log10(rms / 8388608.0 + 1e-12), double(zc) / 2.0 / (double(n) / nmm::g_samplerate));
		}

		if(!wavFile.empty())
		{
			// remove the DAC offset trim and write the 16 bit DAC words as they are
			std::vector<std::vector<int32_t>> out(cap.size());
			for(size_t c=0; c<cap.size(); ++c)
			{
				out[c].resize(cap[c].size());
				for(size_t k=0; k<cap[c].size(); ++k) out[c][k] = cap[c][k] - 341;
			}
			if(writeWav16(wavFile, out, nmm::g_samplerate))
				std::printf("wrote %s\n", wavFile.c_str());
		}
		if(!rawAudio.empty() && !cap[0].empty())
		{
			if(FILE* f = std::fopen(rawAudio.c_str(), "wb")) { std::fwrite(cap[0].data(), sizeof(int32_t), cap[0].size(), f); std::fclose(f); std::printf("wrote %s (ch0 int32)\n", rawAudio.c_str()); }
		}
	}

	std::printf("panel latches:");
	for(uint32_t l=0; l<16; ++l) std::printf(" %02x", uc.getPanel().getLatch(l));
	std::printf("\n");

	if(profile)
	{
		std::vector<std::pair<uint32_t,uint64_t>> v(pcHist.begin(), pcHist.end());
		std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.second > b.second; });
		uint64_t total = 0; for (auto& e : v) total += e.second;
		std::printf("pc profile (top 40 of %zu, %llu samples):\n", v.size(), static_cast<unsigned long long>(total));
		for(size_t k=0; k<v.size() && k<40; ++k)
			std::printf("  $%06x %5.2f%%\n", v[k].first, 100.0 * static_cast<double>(v[k].second) / static_cast<double>(total));
	}

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

	return 0;
}
