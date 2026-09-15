// Differential test: run the same DSP code region on the interpreter and on the JIT starting
// from identical memory dumps and compare the outcome.
//   nmmDspTest diff <p.bin> <x.txt> <y.txt> <startpc> <endpc> [maxsteps]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"
#include "dsp56kEmu/interrupts.h"
#include "dsp56kEmu/jitblockchain.h"
#include "dsp56kEmu/jitblockruntimedata.h"

namespace
{
	bool loadP(dsp56k::Memory& _mem, dsp56k::DSP& _dsp, const char* _file)
	{
		std::ifstream f(_file, std::ios::binary);
		if(!f) return false;
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		for(size_t i=0; i+2<d.size(); i+=3)
		{
			const auto addr = static_cast<uint32_t>(i / 3);
			const uint32_t w = (static_cast<uint32_t>(d[i]) << 16) | (static_cast<uint32_t>(d[i+1]) << 8) | d[i+2];
			_mem.set(dsp56k::MemArea_P, addr, w);
			_dsp.getJit().notifyProgramMemWrite(addr);
		}
		return true;
	}
	bool loadXY(dsp56k::Memory& _mem, const dsp56k::EMemArea _area, const char* _file)
	{
		std::ifstream f(_file);
		if(!f) return false;
		std::string a, v;
		while(f >> a >> v)
			_mem.set(_area, static_cast<uint32_t>(std::stoul(a, nullptr, 16)), static_cast<uint32_t>(std::stoul(v, nullptr, 16)));
		return true;
	}

	struct Engine
	{
		dsp56k::DefaultMemoryValidator validator;
		dsp56k::Memory mem;
		dsp56k::PeripheralsNop periphNop;
		dsp56k::Peripherals56303 periphX;
		dsp56k::DSP dsp;
		Engine() : mem(validator, 0x40000, 0x40000, 0x800), dsp(mem, &periphX, &periphNop)
		{
			auto config = dsp.getJit().getConfig();
			config.dynamicPeripheralAddressing = true;
			config.aguSupportBitreverse = true;
			config.support16BitSCMode = true;
			config.dynamicFastInterrupts = true;
			if(std::getenv("NMM_JIT_DIAG")) config.asmjitDiagnostics = true;
			if(std::getenv("NMM_JIT_MAXOPS")) config.maxInstructionsPerBlock = static_cast<uint32_t>(std::atoi(std::getenv("NMM_JIT_MAXOPS")));
			dsp.getJit().setConfig(config);
		}
	};

	void dumpRegs(Engine& e, const char* _name)
	{
		const auto& r = e.dsp.regs();
		std::printf("%s: pc=$%06x sr=$%06x a=$%02x%06x%06x b=$%02x%06x%06x x1=$%06x x0=$%06x y1=$%06x y0=$%06x\n", _name, r.pc.var, r.sr.var,
			static_cast<uint32_t>((r.a.var >> 48) & 0xff), static_cast<uint32_t>((r.a.var >> 24) & 0xffffff), static_cast<uint32_t>(r.a.var & 0xffffff),
			static_cast<uint32_t>((r.b.var >> 48) & 0xff), static_cast<uint32_t>((r.b.var >> 24) & 0xffffff), static_cast<uint32_t>(r.b.var & 0xffffff),
			static_cast<uint32_t>((r.x.var >> 24) & 0xffffff), static_cast<uint32_t>(r.x.var & 0xffffff), static_cast<uint32_t>((r.y.var >> 24) & 0xffffff), static_cast<uint32_t>(r.y.var & 0xffffff));
		std::printf("    r0-7:"); for(int i=0; i<8; ++i) std::printf(" $%06x", r.r[i].var); std::printf("\n    n0-7:"); for(int i=0; i<8; ++i) std::printf(" $%06x", r.n[i].var); std::printf("\n");
	}
}

int runDiff(int argc, char** argv)
{
	const uint32_t cmpWords = std::getenv("NMM_DIFF_WORDS") ? static_cast<uint32_t>(std::strtoul(std::getenv("NMM_DIFF_WORDS"), nullptr, 0)) : 0x4000;
	if(argc < 7) { std::printf("usage: nmmDspTest diff <p.bin> <x.txt> <y.txt> <startpc> <endpc> [maxsteps]\n"); return 1; }
	const auto startPc = static_cast<uint32_t>(std::stoul(argv[5], nullptr, 16));
	const auto endPc = static_cast<uint32_t>(std::stoul(argv[6], nullptr, 16));
	const auto maxSteps = (argc > 7 && std::atoi(argv[7]) > 0) ? std::atoi(argv[7]) : 100000;

	Engine interp, jit;
	for(auto* e : {&interp, &jit})
	{
		if(!loadP(e->mem, e->dsp, argv[2]) || !loadXY(e->mem, dsp56k::MemArea_X, argv[3]) || !loadXY(e->mem, dsp56k::MemArea_Y, argv[4]))
		{
			std::printf("failed to load dumps\n");
			return 1;
		}
		// park the engine at endpc: jmp endpc
		e->mem.set(dsp56k::MemArea_P, endPc, 0x0af080); e->dsp.getJit().notifyProgramMemWrite(endPc);
		e->mem.set(dsp56k::MemArea_P, endPc + 1, endPc); e->dsp.getJit().notifyProgramMemWrite(endPc + 1);
		e->dsp.setPC(startPc);
		e->dsp.regs().sp.var = 3;
		e->dsp.regs().sr.var = 0xc00000;	// note: caller may want the real SR
		e->dsp.regs().r[6].var = 0x620;
	}

	// snapshot of the initial memory to report what the code writes
	std::vector<uint32_t> x0(0x4000), y0(0x4000);
	for(uint32_t a=0; a<0x4000; ++a) { x0[a] = interp.mem.get(dsp56k::MemArea_X, a); y0[a] = interp.mem.get(dsp56k::MemArea_Y, a); }

	// "loop:N" mode: both engines start at <startpc>, which is expected to be the code that opens the
	// idle DO FOREVER loop. N frames of ~600 instructions with one IRQD each are run, and after every
	// frame the loop registers and the memory are compared.
	if(argc > 7 && std::string(argv[7]).rfind("loop:", 0) == 0)
	{
		const int frames = std::atoi(argv[7] + 5);
		for(auto* e : {&interp, &jit})
		{
			e->dsp.setPC(startPc);
			e->dsp.regs().sp.var = 0;
			e->dsp.regs().sr.var = 0xc00300;
			e->dsp.regs().r[6].var = 0x620;
			e->mem.set(dsp56k::MemArea_X, 1, 0);
		}
		for(int f=0; f<frames; ++f)
		{
			for(auto* e : {&interp, &jit})
			{
				const bool isInterp = e == &interp;
				const auto target = e->dsp.getInstructionCounter() + 600;
				e->dsp.injectInterrupt(dsp56k::Vba_IRQD);
				while(e->dsp.getInstructionCounter() < target)
				{
					if(isInterp) e->dsp.execInterpreter(); else e->dsp.exec();
				}
			}
			int diffs = 0;
			for (const auto area : {dsp56k::MemArea_X, dsp56k::MemArea_Y})
				for(uint32_t a=0; a<cmpWords; ++a)
					if(interp.mem.get(area, a) != jit.mem.get(area, a))
					{
						if(diffs < 8) std::printf("  frame %d: %c:$%06x interp=$%06x jit=$%06x\n", f, area == dsp56k::MemArea_X ? 'X' : 'Y', a, interp.mem.get(area, a), jit.mem.get(area, a));
						++diffs;
					}
			const auto& ri = interp.dsp.regs(); const auto& rj = jit.dsp.regs();
			std::printf("frame %2d: interp pc=$%06x la=$%06x sr=$%06x sp=%u X:$1=%u X:$10=$%06x | jit pc=$%06x la=$%06x sr=$%06x sp=%u X:$1=%u X:$10=$%06x | %d diffs\n", f,
				ri.pc.var, ri.la.var, ri.sr.var, ri.sp.var, interp.mem.get(dsp56k::MemArea_X, 1), interp.mem.get(dsp56k::MemArea_X, 0x10),
				rj.pc.var, rj.la.var, rj.sr.var, rj.sp.var, jit.mem.get(dsp56k::MemArea_X, 1), jit.mem.get(dsp56k::MemArea_X, 0x10), diffs);
		}
		return 0;
	}

	// "irq:N" mode: park both engines at endpc and deliver N IRQD interrupts, the handler runs
	// through its jsr/rti each time. Memory is compared after every frame.
	if(argc > 7 && std::string(argv[7]).rfind("irq:", 0) == 0)
	{
		const int frames = std::atoi(argv[7] + 4);
		for(auto* e : {&interp, &jit})
		{
			e->dsp.setPC(endPc);
			e->dsp.regs().sp.var = 2;
			e->dsp.regs().sr.var = 0xc18010;	// as observed in the idle loop: LF+FV set, mask 0
			e->dsp.regs().la.var = 0x174;
		}
		for(int f=0; f<frames; ++f)
		{
			for(auto* e : {&interp, &jit})
			{
				const bool isInterp = e == &interp;
				e->dsp.injectInterrupt(dsp56k::Vba_IRQD);
				int n = 0;
				const bool trace = std::getenv("NMM_DIFF_TRACE") != nullptr && f == 0;
				do
				{
					if(isInterp) e->dsp.execInterpreter(); else e->dsp.exec();
					++n;
					if(trace && n <= 80) std::fprintf(stderr, "  %s step %d: pc=$%06x sp=%u mode=%d\n", isInterp ? "interp" : "jit   ", n, e->dsp.getPC().var, e->dsp.regs().sp.var, static_cast<int>(e->dsp.getProcessingMode()));
					if(trace && !isInterp && n <= 3)
					{
						if(const auto* chain = e->dsp.getJit().getCurrentChain())
						{
							if(const auto* b = chain->getBlock(e->dsp.getPC().var))
							{
								const auto& bi = b->getInfo();
								std::fprintf(stderr, "    block: first=$%06x pmemsize=%u ops=%u term=%d flags=%#x branch=$%06x cond=%d\n%s\n", b->getPCFirst(), b->getPMemSize(), 0u,
									static_cast<int>(bi.terminationReason), bi.flags, bi.branchTarget, bi.branchIsConditional ? 1 : 0, b->getDisasm().c_str());
							}
							else std::fprintf(stderr, "    no block at pc\n");
						}
					}
				}
				while((e->dsp.hasPendingInterrupts() || e->dsp.getPC().var != endPc) && n < maxSteps);
			}
			int diffs = 0;
			for (const auto area : {dsp56k::MemArea_X, dsp56k::MemArea_Y})
				for(uint32_t a=0; a<cmpWords; ++a)
					if(interp.mem.get(area, a) != jit.mem.get(area, a))
					{
						if(diffs < 12) std::printf("  frame %d: %c:$%06x interp=$%06x jit=$%06x\n", f, area == dsp56k::MemArea_X ? 'X' : 'Y', a, interp.mem.get(area, a), jit.mem.get(area, a));
						++diffs;
					}
			std::printf("frame %d: P:$17 interp=$%06x jit=$%06x, X:$10 interp=$%06x jit=$%06x, X:$1 interp=$%06x jit=$%06x, sp interp=%u jit=%u, %d diffs\n", f,
				interp.mem.get(dsp56k::MemArea_P, 0x17), jit.mem.get(dsp56k::MemArea_P, 0x17),
				interp.mem.get(dsp56k::MemArea_X, 0x10), jit.mem.get(dsp56k::MemArea_X, 0x10),
				interp.mem.get(dsp56k::MemArea_X, 0x1), jit.mem.get(dsp56k::MemArea_X, 0x1),
				interp.dsp.regs().sp.var, jit.dsp.regs().sp.var, diffs);
			if(diffs) { dumpRegs(interp, "interp"); dumpRegs(jit, "jit   "); return 1; }
		}
		return 0;
	}

	// NMM_DIFF_REGS="a=ff80002b000000,b=...,x0=...,x1=...,y0=...,y1=...,r3=...,r4=...,sr=..." presets registers in both engines
	if(const auto* presets = std::getenv("NMM_DIFF_REGS"))
	{
		std::string str(presets);
		size_t pos = 0;
		while(pos < str.size())
		{
			const auto comma = str.find(',', pos);
			const auto item = str.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
			pos = comma == std::string::npos ? str.size() : comma + 1;
			const auto eq = item.find('=');
			if(eq == std::string::npos) continue;
			const auto name = item.substr(0, eq);
			const auto v = std::strtoull(item.substr(eq + 1).c_str(), nullptr, 16);
			for(auto* e : {&interp, &jit})
			{
				auto& r = e->dsp.regs();
				if(name == "a") r.a.var = v; else if(name == "b") r.b.var = v;
				else if(name == "x0") r.x.var = (r.x.var & ~0xffffffull) | (v & 0xffffff); else if(name == "x1") r.x.var = (r.x.var & 0xffffff) | ((v & 0xffffff) << 24);
				else if(name == "y0") r.y.var = (r.y.var & ~0xffffffull) | (v & 0xffffff); else if(name == "y1") r.y.var = (r.y.var & 0xffffff) | ((v & 0xffffff) << 24);
				else if(name == "sr") r.sr.var = static_cast<uint32_t>(v);
				else if(name.size() == 2 && name[0] == 'r') r.r[name[1] - '0'].var = static_cast<uint32_t>(v);
				else if(name.size() == 2 && name[0] == 'n') r.n[name[1] - '0'].var = static_cast<uint32_t>(v);
			}
		}
	}

	int steps = 0;
	while(interp.dsp.getPC().var != endPc && steps < maxSteps) { interp.dsp.execInterpreter(); ++steps; }

	{
		int changed = 0;
		std::printf("interpreter writes:");
		for(uint32_t a=0; a<0x4000; ++a)
		{
			const auto vx = interp.mem.get(dsp56k::MemArea_X, a), vy = interp.mem.get(dsp56k::MemArea_Y, a);
			if(vx != x0[a]) { if(changed < 80) std::printf(" X:$%x=$%06x", a, vx); ++changed; }
			if(vy != y0[a]) { if(changed < 80) std::printf(" Y:$%x=$%06x", a, vy); ++changed; }
		}
		std::printf(" (%d words)\n", changed);
	}
	std::printf("interpreter: %d steps\n", steps);
	steps = 0;
	const bool trace = std::getenv("NMM_DIFF_TRACE") != nullptr;
	std::fflush(stdout);
	while(jit.dsp.getPC().var != endPc && steps < maxSteps)
	{
		jit.dsp.exec(); ++steps;
		if(trace && steps <= 60) std::fprintf(stderr, "  jit step %d: pc=$%06x sp=%u mode=%d\n", steps, jit.dsp.getPC().var, jit.dsp.regs().sp.var, static_cast<int>(jit.dsp.getProcessingMode()));
	}
	std::printf("jit: %d block runs\n", steps);

	dumpRegs(interp, "interp");
	dumpRegs(jit, "jit   ");

	int diffs = 0;
	for (const auto area : {dsp56k::MemArea_X, dsp56k::MemArea_Y})
	{
		for(uint32_t a=0; a<0x4000; ++a)
		{
			const auto vi = interp.mem.get(area, a), vj = jit.mem.get(area, a);
			if(vi != vj)
			{
				if(diffs < 40) std::printf("  %c:$%06x interp=$%06x jit=$%06x\n", area == dsp56k::MemArea_X ? 'X' : 'Y', a, vi, vj);
				++diffs;
			}
		}
	}
	std::printf("%d memory differences\n", diffs);
	return diffs ? 1 : 0;
}
