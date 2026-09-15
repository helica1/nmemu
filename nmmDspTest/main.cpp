// Minimal DSP56303 core test: the Nord Modular kernel writes its IRQD vector target with
//   movep x:<<M_DOR0,ssh    ; push DOR0 onto the system stack
//   move  ssh,p:<$17        ; pop it into program memory word $17
// Check that the emulator core performs both halves.
#include <cstdio>
#include <cstdlib>

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

int main()
{
	dsp56k::DefaultMemoryValidator validator;
	dsp56k::Memory mem(validator, 0x4000, 0x4000, 0x800);
	dsp56k::PeripheralsNop periphNop;
	dsp56k::Peripherals56303 periphX;
	dsp56k::DSP dsp(mem, &periphX, &periphNop);

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

	return mem.get(dsp56k::MemArea_P, 0x17) == 0x175 ? 0 : 1;
}
