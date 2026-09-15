// Disassemble a P memory dump written by nmmConsole --dump-dsp:
//   nmmDspTest disasm <p.bin> <startpc> <endpc>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/opcodes.h"

int runDisasm(int argc, char** argv)
{
	if(argc < 5) { std::fprintf(stderr, "usage: nmmDspTest disasm <p.bin> <startpc> <endpc>\n"); return 1; }
	std::ifstream f(argv[2], std::ios::binary);
	if(!f) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
	std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	const auto start = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 0));
	const auto end = static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 0));
	std::vector<uint32_t> mem;
	for(uint32_t a=start; a<end && (a*3+2) < d.size(); ++a)
		mem.push_back((static_cast<uint32_t>(d[a*3]) << 16) | (static_cast<uint32_t>(d[a*3+1]) << 8) | d[a*3+2]);
	dsp56k::Opcodes opcodes;
	dsp56k::Disassembler dis(opcodes);
	std::string out;
	dis.disassembleMemoryBlock(out, mem, start, false, true, true);
	std::fputs(out.c_str(), stdout);
	return 0;
}
