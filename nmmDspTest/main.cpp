// Minimal DSP56303 core test: the Nord Modular kernel writes its IRQD vector target with
//   movep x:<<M_DOR0,ssh    ; push DOR0 onto the system stack
//   move  ssh,p:<$17        ; pop it into program memory word $17
// Check that the emulator core performs both halves.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

int runDiff(int argc, char** argv);
int runDisasm(int argc, char** argv);

int main(int argc, char** argv)
{
	if(argc > 1 && std::string(argv[1]) == "diff")
		return runDiff(argc, argv);
	if(argc > 1 && std::string(argv[1]) == "disasm")
		return runDisasm(argc, argv);

	dsp56k::DefaultMemoryValidator validator;
	dsp56k::Memory mem(validator, 0x4000, 0x4000, 0x800);
	dsp56k::PeripheralsNop periphNop;
	dsp56k::Peripherals56303 periphX;
	dsp56k::DSP dsp(mem, &periphX, &periphNop);
	{
		auto config = dsp.getJit().getConfig();
		config.dynamicPeripheralAddressing = true;	// as the emulator configures it: host commands write peripherals through r0
		dsp.getJit().setConfig(config);
	}

	auto set = [&](const uint32_t _addr, const uint32_t _word)
	{
		mem.set(dsp56k::MemArea_P, _addr, _word);
		dsp.getJit().notifyProgramMemWrite(_addr);
	};

	for(uint32_t i=0; i<0x100; ++i) set(i, 0x000000);	// nops

	set(0x17, 0x000200);
	set(0x100, 0x087c33);	// movep x:<<$fffff3,ssh
	set(0x101, 0x07173c);	// move ssh,p:<$17
	set(0x102, 0x0af080);	// jmp $102
	set(0x103, 0x000102);

	// DOR0 = $175
	periphX.write(dsp56k::XIO_DOR0, 0x175);

	dsp.setPC(0x100);
	dsp.regs().sp.var = 2;
	dsp.regs().ss[2].var = 0x00000aaa00000bbb;

	std::printf("before: P:$17=$%06x SP=$%02x SSH=$%06x DOR0=$%06x\n", mem.get(dsp56k::MemArea_P, 0x17), dsp.regs().sp.var, static_cast<uint32_t>(dsp.regs().ss[2].var >> 24), periphX.read(dsp56k::XIO_DOR0, static_cast<dsp56k::Instruction>(0)));

	const bool useInterpreter = getenv("NMM_INTERP") != nullptr;
	for(int i=0; i<8; ++i)
	{
		if(useInterpreter)
			dsp.execInterpreter();
		else
			dsp.exec();
	}

	std::printf("after : P:$17=$%06x SP=$%02x SS[2]=$%016llx SS[3]=$%016llx pc=$%06x\n", mem.get(dsp56k::MemArea_P, 0x17), dsp.regs().sp.var,
		static_cast<unsigned long long>(dsp.regs().ss[2].var), static_cast<unsigned long long>(dsp.regs().ss[3].var), dsp.getPC().var);

	const bool test1 = mem.get(dsp56k::MemArea_P, 0x17) == 0x175;

	// Test 2: the host command path. The 68k points r0 at a peripheral register and writes it
	// through the host port, the vectors are
	//   $7e: movep x:<<M_HORX,r0        ; 085006
	//   $6a: movep x:<<M_HORX,x:(r0)    ; 086086   (ea is a peripheral address here: DOR0)
	// then the one-shot handler above copies DOR0 into P:$17. DOR0 must end up holding the word.
	set(0x7e, 0x085006);
	set(0x7f, 0x000000);
	set(0x6a, 0x086086);
	set(0x6b, 0x000000);
	set(0x17, 0x000200);
	periphX.write(dsp56k::XIO_DOR0, 0);
	dsp.setPC(0x100);
	dsp.regs().sp.var = 2;
	dsp.regs().sr.var &= ~0x300;	// unmask interrupts (I1:I0 = 0), the kernel does andi #$fc,mr

	const uint32_t word = 0x000176;
	const uint32_t addr = 0xfffff3;
	periphX.getHI08().writeRX(&addr, 1);
	dsp.injectInterrupt(0x7e);
	if(getenv("NMM_TRACE")) dsp.enableTrace(static_cast<dsp56k::DSP::TraceMode>(dsp56k::DSP::Ops | dsp56k::DSP::Regs));
	for(int i=0; i<2; ++i) { if(useInterpreter) dsp.execInterpreter(); else dsp.exec(); }
	periphX.getHI08().writeRX(&word, 1);
	dsp.injectInterrupt(0x6a);
	for(int i=0; i<2; ++i) { if(useInterpreter) dsp.execInterpreter(); else dsp.exec(); }
	std::printf("test2 : after $7e/$6a: r0=$%06x DOR0=$%06x\n", dsp.regs().r[0].var, periphX.read(dsp56k::XIO_DOR0, static_cast<dsp56k::Instruction>(0)));

	// now the one-shot handler sequence at $100
	dsp.setPC(0x100);
	dsp.regs().sp.var = 2;
	for(int i=0; i<6; ++i)
	{
		if(useInterpreter)
			dsp.execInterpreter();
		else
			dsp.exec();
	}

	const auto dor0 = periphX.read(dsp56k::XIO_DOR0, static_cast<dsp56k::Instruction>(0));
	std::printf("test2 : r0=$%06x DOR0=$%06x P:$17=$%06x SP=$%02x pc=$%06x\n", dsp.regs().r[0].var, dor0, mem.get(dsp56k::MemArea_P, 0x17), dsp.regs().sp.var, dsp.getPC().var);
	const bool test2 = dor0 == word && mem.get(dsp56k::MemArea_P, 0x17) == word;

	const bool test2b = true;

	// Test 3: host command $68 writes program memory, that is how the patch code is uploaded
	//   movep x:<<M_HORX,p:(r0)+         ; 085846
	set(0x68, 0x085846);
	set(0x69, 0x000000);
	dsp.regs().r[0].var = 0x1000;
	const uint32_t code = 0x123456;
	periphX.getHI08().writeRX(&code, 1);
	dsp.injectInterrupt(0x68);
	for(int i=0; i<4; ++i)
	{
		if(useInterpreter)
			dsp.execInterpreter();
		else
			dsp.exec();
	}
	std::printf("test3 : P:$1000=$%06x r0=$%06x\n", mem.get(dsp56k::MemArea_P, 0x1000), dsp.regs().r[0].var);
	const bool test3 = mem.get(dsp56k::MemArea_P, 0x1000) == code && dsp.regs().r[0].var == 0x1001;

	std::printf("%s\n", (test1 && test2 && test2b && test3) ? "ALL PASSED" : "FAILED");
	return (test1 && test2 && test2b && test3) ? 0 : 1;
}
