#include "nmmhardware.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "nmmlog.h"
#include "nmmpatch.h"

#include "dsp56kEmu/interrupts.h"
#include "dsp56kBase/threadtools.h"

namespace nmm
{
	namespace
	{
		constexpr uint32_t g_syncEssiFrameRate = 16;
		constexpr uint32_t g_syncHaltDspEssiThreshold = 32;

		// frames of silence handed to the DSP receivers up front. The kernel enables both receivers
		// during init and blocks on an empty input ring; from then on the host pushes one input frame
		// per output frame so this stays constant (it is the audio input latency).
		constexpr uint32_t g_essiInputPrefill = 512;

		static_assert((g_syncEssiFrameRate & (g_syncEssiFrameRate - 1)) == 0, "frame sync rate must be a power of two");
		static_assert(g_syncHaltDspEssiThreshold >= g_syncEssiFrameRate * 2, "DSP halt threshold must be greater than two times the sync rate");
	}

	Hardware::Hardware(const HardwareConfig& _config)
		: m_samplerateInv(1.0 / g_samplerate)
	{
		if(const auto* t = std::getenv("NMM_DSPTRACE_AT"))
		{
			m_dspTraceAtFrame = static_cast<uint64_t>(std::atof(t) * g_samplerate);
			const auto* colon = std::strchr(t, ':');
			m_dspTraceAtCount = colon ? static_cast<uint32_t>(std::atoi(colon + 1)) : 2;
		}

		if(!_config.os.isValid())
		{
			NMMLOG("Hardware: no OS image");
			return;
		}

		m_usesBootRom = !_config.bootRom.empty();

		m_uc.reset(new Microcontroller(*this, _config.bootRom));

		// second flash: persisted image if present, otherwise synthesized from the OS
		auto& flash = m_uc->getFlash();
		if(_config.flashFile.empty() || !flash.load(_config.flashFile))
			flash.createFromOs(_config.os.data, _config.os.versionMajor, _config.os.versionMinor);

		for (const auto slot : _config.dspSlots)
		{
			m_dsps.emplace_back(new DSP(*this, m_uc->getDspSlots().slot(slot), slot));
			auto* dsp = m_dsps.back().get();
			m_uc->getDspSlots().setIcrCallback(slot, [dsp](const uint8_t _icr) { dsp->onHostIcrWrite(_icr); });
		}

		if(!m_usesBootRom)
		{
			m_uc->writeRam(g_ramAddress, _config.os.data);
			hleBoot();
		}

		m_valid = true;

		m_ucThread.reset(new std::thread([this]
		{
			ucThreadFunc();
		}));

		// run until the DSP produces audio frames, i.e. boot ROM and OS init are through
		if(!m_dsps.empty())
		{
			synthLib::TAudioInputs ins{};
			synthLib::TAudioOutputs outs{};
			const auto t0 = std::chrono::steady_clock::now();
			auto tReport = t0;
			while(!m_bootFinished)
			{
				processAudio(ins, outs, 8, 8);

				const auto now = std::chrono::steady_clock::now();
				if(now - tReport > std::chrono::seconds(3))
				{
					tReport = now;
					auto* dsp = getMasterDSP();
					NMMLOG("boot: still waiting for the first DSP audio frame after %lld s, 68k pc=$%06x instr=%llu, DSP booted=%d pc=$%06x instr=%llu",
						static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now - t0).count()), m_uc->getPC(), static_cast<unsigned long long>(m_uc->getInstructionCount()),
						dsp && dsp->isBooted() ? 1 : 0, dsp && dsp->isBooted() ? dsp->dsp().getPC().var : 0, dsp && dsp->isBooted() ? static_cast<unsigned long long>(dsp->dsp().getInstructionCounter()) : 0ull);
				}
			}
		}
		// Fast-forward through the OS boot: the OS needs a couple of seconds of emulated time
		// before it accepts editor messages. Run that unpaced (a fraction of a second of wall
		// time) so a freshly loaded plugin is ready at once.
		// The OS is ready once it answers an editor's "I am" on the PC port.
		if(!m_dsps.empty() && m_bootFinished)
		{
			synthLib::TAudioInputs ins{};
			synthLib::TAudioOutputs outs{};
			const auto maxFrames = static_cast<uint64_t>(g_samplerate) * 8;
			std::vector<uint8_t> reply, out;
			bool ready = false;
			uint64_t nextIAm = 0;
			while(!ready && m_essiFrameIndex < maxFrames)
			{
				// the OS resets its UART during init and drops what was queued, so keep asking
				if(m_essiFrameIndex >= nextIAm)
				{
					m_uc->getPcPort().write(PatchSysex::iAm());
					nextIAm = m_essiFrameIndex + g_samplerate / 4;
				}
				processAudio(ins, outs, 256, 0);
				out.clear();
				m_uc->getPcPort().read(out);
				reply.insert(reply.end(), out.begin(), out.end());
				for(size_t i=0; i+4<reply.size(); ++i)
					if(reply[i] == 0xf0 && reply[i+1] == 0x33 && reply[i+2] == 0x00 && reply[i+3] == 0x06 && reply[i+4] == 0x01) { ready = true; break; }
			}
			// whatever else the OS sent meanwhile (voice count, lights) is dropped
			m_uc->getMidi().read(out);
			NMMLOG("OS %s after %.2f s of emulated time, %zu reply bytes, pc port rx=%llu tx=%llu", ready ? "ready" : "NOT ready", static_cast<double>(m_essiFrameIndex) / g_samplerate, reply.size(),
				static_cast<unsigned long long>(m_uc->getPcPort().getRxCount()), static_cast<unsigned long long>(m_uc->getPcPort().getTxCount()));
			for(size_t i=0; i<reply.size() && i<24; ++i) std::printf(" %02x", reply[i]);
			if(!reply.empty()) std::printf("\n");
		}
		m_midiOffsetCounter = 0;
	}

	Hardware::~Hardware()
	{
		m_destroy = true;

		if(!m_dsps.empty() && m_bootFinished)
		{
			synthLib::TAudioInputs ins{};
			synthLib::TAudioOutputs outs{};
			while(m_destroy)
				processAudio(ins, outs, 8, 64);
		}
		else
		{
			while(m_destroy)
				std::this_thread::yield();
		}

		for (auto& d : m_dsps)
			d->terminate();

		m_essiFrameIndex = 0;
		m_essiLatency = 0;

		// let the DSP threads run out: unblock anything they might be waiting on
		for (auto& d : m_dsps)
		{
			while(d->getDSPThread() && !d->getDSPThread()->runThread())	// the run flag flips back to true once the thread function has exited
			{
				m_haltDSPSem.notify(999999);
				auto& p = d->getPeriph();
				if(p.getEssi0().getAudioOutputs().full()) p.getEssi0().getAudioOutputs().pop_front();
				if(p.getEssi1().getAudioOutputs().full()) p.getEssi1().getAudioOutputs().pop_front();
				if(p.getEssi0().getAudioInputs().empty()) p.getEssi0().getAudioInputs().push_back({});
				if(p.getEssi1().getAudioInputs().empty()) p.getEssi1().getAudioInputs().push_back({});
				std::this_thread::yield();
			}
		}

		if(m_ucThread)
			m_ucThread->join();
	}

	void Hardware::hleBoot()
	{
		// what the boot rom does after copying: jmp $100000. The OS sets SR/SP itself.
		m_uc->setPC(g_osEntryCold);
		m_osStarted = true;
	}

	void Hardware::ucThreadFunc()
	{
		dsp56k::ThreadTools::setCurrentThreadName("MC68331");
		dsp56k::ThreadTools::setCurrentThreadPriority(dsp56k::ThreadPriority::Highest);

		while(!m_destroy)
		{
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
		}
		resumeDSPs();
		m_destroy = false;
	}

	void Hardware::processUC()
	{
		if(m_remainingUcCycles <= 0)
			syncUCtoDSP();

		const auto deltaCycles = m_uc->exec();

		if(!m_osStarted)
		{
			const auto pc = m_uc->getPC();
			if(pc >= g_ramAddress && pc < g_ramAddress + g_ramSize)
			{
				m_osStarted = true;
				NMMLOG("68k: entered OS in RAM at pc=$%06x after %llu instructions", pc, static_cast<unsigned long long>(m_uc->getInstructionCount()));
			}
		}

		if(m_essiFrameIndex > 0)
			m_remainingUcCycles -= static_cast<int64_t>(deltaCycles);
	}

	void Hardware::syncUCtoDSP()
	{
		// the UC can only be clocked by the DSP once the DSP produces frames
		if(m_essiFrameIndex <= 0)
			return;

		if(m_essiFrameIndex == m_lastEssiFrameIndex)
		{
			resumeDSPs();
			std::unique_lock uLock(m_essiFrameAddedMutex);
			m_essiFrameAddedCv.wait(uLock, [this]{ return m_essiFrameIndex > m_lastEssiFrameIndex || m_destroy; });
		}

		const auto essiFrameIndex = m_essiFrameIndex.load();
		const auto essiDelta = essiFrameIndex - m_lastEssiFrameIndex;

		const auto ucClock = m_uc->getSim().getSystemClockHz();
		const double ucCyclesPerFrame = static_cast<double>(ucClock) * m_samplerateInv;

		// if the UC consumed more cycles than it was allowed to, remove them from remaining cycles
		m_remainingUcCyclesD += static_cast<double>(m_remainingUcCycles);
		// add cycles for the ESSI time that has passed
		m_remainingUcCyclesD += ucCyclesPerFrame * static_cast<double>(essiDelta);
		m_remainingUcCycles = static_cast<int64_t>(m_remainingUcCyclesD);
		m_remainingUcCyclesD -= static_cast<double>(m_remainingUcCycles);

		if(essiDelta > g_syncHaltDspEssiThreshold)
			haltDSPs();

		m_lastEssiFrameIndex = essiFrameIndex;
	}

	void Hardware::onDspBooted(DSP& _dsp)
	{
		auto& periph = _dsp.getPeriph();

		for (auto* essi : {&periph.getEssi0(), &periph.getEssi1()})
		{
			essi->processAudioInput<float>(g_essiInputPrefill, 0, [](size_t, dsp56k::Audio::RxFrame& _f)
			{
				_f.resize(2);
				_f[0] = dsp56k::Audio::RxSlot{0,0,0,0};
				_f[1] = dsp56k::Audio::RxSlot{0,0,0,0};
			});
		}

		if(&_dsp != getMasterDSP())
			return;

		// The hardware feeds a sample clock into the DSP's IRQD pin; the kernel's IRQD handler is
		// the patch program. One IRQD per ESSI0 transmit frame reproduces that. The emulator core
		// does not gate external interrupts on IPRC, so honour the IRQD enable bits here: the
		// kernel disables IRQD while the host uploads a patch.
		auto* dsp = &_dsp;
		_dsp.getAudioOut().setCallback([this, dsp](dsp56k::Audio*)
		{
			const auto iprc = dsp->getPeriph().read(dsp56k::XIO_IPRC, static_cast<dsp56k::Instruction>(0));
			if((iprc >> 9) & 3)
			{
				++m_irqdInjected;
				dsp->dsp().injectInterrupt(dsp56k::Vba_IRQD);
			}
			else
				++m_irqdMasked;

			if(m_dspProfile)
				++m_dspPcHist[dsp->dsp().getPC().var];

			// NMM_ISRDUMP=<frames>: after the second host flag clear (the first patch load), print
			// module memory and registers once per frame, for comparing the JIT against the interpreter
			static const int isrDumpFrames = std::getenv("NMM_ISRDUMP") ? std::atoi(std::getenv("NMM_ISRDUMP")) : 0;
			if(isrDumpFrames && dsp->getHf0ClearCount() == 2)
			{
				static int n = 0;
				// NMM_DSPTRACE_ISR=<ordinal>:<frames> traces instructions from that frame ordinal on
				static const char* traceIsr = std::getenv("NMM_DSPTRACE_ISR");
				if(traceIsr && n == std::atoi(traceIsr))
				{
					const auto* colon = std::strchr(traceIsr, ':');
					m_dspTraceFrames = colon ? static_cast<uint32_t>(std::atoi(colon + 1)) : 2;
					dsp->dsp().enableTrace(static_cast<dsp56k::DSP::TraceMode>(dsp56k::DSP::Ops | dsp56k::DSP::StackIndent | (std::getenv("NMM_DSPTRACE_REGS") ? dsp56k::DSP::Regs : 0)));
					std::fprintf(stderr, "ISRTRACE enabled at ordinal %d for %u frames\n", n, m_dspTraceFrames.load());
				}
				if(n < isrDumpFrames)
				{
					auto& mem = dsp->dsp().memory();
					const auto& r = dsp->dsp().regs();
					std::string line = "ISRDUMP " + std::to_string(n) + " pc=" + std::to_string(r.pc.var) + " x1=" + std::to_string(mem.get(dsp56k::MemArea_X, 1)) + " X:";
					for(uint32_t a=0x5f; a<0x68; ++a) { char b[16]; std::snprintf(b, sizeof(b), " %06x", mem.get(dsp56k::MemArea_X, a)); line += b; }
					line += " Y:";
					for(uint32_t a=0x5f; a<0x68; ++a) { char b[16]; std::snprintf(b, sizeof(b), " %06x", mem.get(dsp56k::MemArea_Y, a)); line += b; }
					char b[160]; std::snprintf(b, sizeof(b), " x0=%06x x1=%06x y0=%06x y1=%06x r3=%06x r4=%06x r6=%06x sp=%u sr=%06x la=%06x lc=%06x", static_cast<uint32_t>(r.x.var & 0xffffff), static_cast<uint32_t>((r.x.var >> 24) & 0xffffff), static_cast<uint32_t>(r.y.var & 0xffffff), static_cast<uint32_t>((r.y.var >> 24) & 0xffffff), r.r[3].var, r.r[4].var, r.r[6].var, r.sp.var, r.sr.var, r.la.var, r.lc.var);
					line += b;
					std::fprintf(stderr, "%s\n", line.c_str());
					++n;
				}
			}

			if(m_dspTraceFrames && --m_dspTraceFrames == 0)
			{
				NMMLOG("[%s] DSP instruction trace disabled", dsp->getName().c_str());
				dsp->dsp().enableTrace(dsp56k::DSP::Disabled);
			}
			// NMM_DSPTRACE_AT=<frame>:<count> starts a trace at an absolute frame index
			if(m_dspTraceAtFrame && m_essiFrameIndex == m_dspTraceAtFrame)
			{
				NMMLOG("[%s] DSP instruction trace enabled at frame %llu", dsp->getName().c_str(), static_cast<unsigned long long>(m_dspTraceAtFrame));
				dsp->dsp().enableTrace(static_cast<dsp56k::DSP::TraceMode>(dsp56k::DSP::Ops | dsp56k::DSP::StackIndent));
				m_dspTraceFrames = m_dspTraceAtCount;
			}

			onEssiFrame();
		});
	}

	void Hardware::onEssiFrame()
	{
		// runs on the DSP thread once per audio frame
		++m_essiFrameIndex;

		if(!m_bootFinished)
			m_bootFinished = true;

		processMidiInput();

		if((m_essiFrameIndex & (g_syncEssiFrameRate-1)) == 0)
			m_essiFrameAddedCv.notify_one();

		auto& outs = getMasterDSP()->getAudioOut().getAudioOutputs();

		m_requestedFramesAvailableMutex.lock();

		if(m_requestedFrames && outs.size() >= m_requestedFrames)
		{
			m_requestedFramesAvailableMutex.unlock();
			m_requestedFramesAvailableCv.notify_one();
		}
		else
		{
			m_requestedFramesAvailableMutex.unlock();
		}

		// wait for the host to ask for more
		m_haltDSPSem.wait(1);
	}

	void Hardware::processMidiInput()
	{
		++m_midiOffsetCounter;

		while(!m_midiIn.empty())
		{
			const auto& e = m_midiIn.front();

			if(e.offset > m_midiOffsetCounter)
				break;

			if(!e.sysex.empty())
				m_uc->getPcPort().write(std::vector<uint8_t>(e.sysex.begin(), e.sysex.end()));
			else
				m_uc->getMidi().write(e);

			m_midiIn.pop_front();
		}
	}

	bool Hardware::sendMidi(const synthLib::SMidiEvent& _ev)
	{
		m_midiIn.push_back(_ev);
		return true;
	}

	void Hardware::readMidiOut(std::vector<uint8_t>& _midiOut, std::vector<uint8_t>& _pcOut)
	{
		m_uc->getMidi().read(_midiOut);
		m_uc->getPcPort().read(_pcOut);
	}

	void Hardware::ensureBufferSize(const uint32_t _frames)
	{
		if(m_dummyInput.size() >= _frames)
			return;
		m_dummyInput.resize(_frames, 0.0f);
		for (auto& o : m_audioOutputs)
			o.resize(_frames, 0);
	}

	void Hardware::processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, uint32_t _frames, const uint32_t _latency)
	{
		m_uc->getMidi().process(_frames);

		ensureBufferSize(_frames);

		float* outL = _outputs[0];
		float* outR = _outputs[1];

		auto* dsp = getMasterDSP();

		if(!dsp || !m_bootFinished)
		{
			// nothing produces audio yet, the 68k thread runs freely and the DSP is not paced
			if(outL) std::fill(outL, outL + _frames, 0.0f);
			if(outR) std::fill(outR, outR + _frames, 0.0f);
			for (auto& o : m_audioOutputs)
				std::fill(o.begin(), o.begin() + _frames, 0);
			std::this_thread::sleep_for(std::chrono::microseconds(_frames * 1000000ull / g_samplerate));
			return;
		}

		auto& essi0 = dsp->getPeriph().getEssi0();
		auto& essi1 = dsp->getPeriph().getEssi1();

		// audio inputs, same port assignment as the outputs: ESSI1 = left, ESSI0 = right. The ADC
		// words are assumed to sit in the low bits like the DAC side does (unverified)
		const float* inL = _inputs[0] ? _inputs[0] : m_dummyInput.data();
		const float* inR = _inputs[1] ? _inputs[1] : m_dummyInput.data();

		// The DSP consumes one input frame per output frame. Inputs are pushed per 64 frame chunk
		// right before the DSP is allowed to run those frames: pushing a whole large host block up
		// front would fill the input ring (32K frames) and block this thread while the DSP waits
		// for its semaphore, a deadlock.
		auto pushInputs = [&](dsp56k::Essi& _essi, const float* _in, const uint32_t _count)
		{
			_essi.processAudioInput<float>(_count, _latency, [&](const size_t _s, dsp56k::Audio::RxFrame& _f)
			{
				const auto clamped = std::max(-32768.0f, std::min(32767.0f, _in[_s] * 32768.0f));
				const auto v = static_cast<dsp56k::TWord>(static_cast<int32_t>(clamped)) & 0xffff;
				_f.resize(2);
				_f[0] = dsp56k::Audio::RxSlot{v, 0, 0, 0};
				_f[1] = dsp56k::Audio::RxSlot{v, 0, 0, 0};
			});
		};

		size_t writePos = 0;

		while (_frames)
		{
			const auto processCount = std::min(_frames, static_cast<uint32_t>(64));
			_frames -= processCount;

			pushInputs(essi0, inR + writePos, processCount);
			pushInputs(essi1, inL + writePos, processCount);

			advanceSamples(processCount, _latency);

			const auto requiredSize = processCount > 8 ? processCount - 8 : 0;

			if(essi0.getAudioOutputs().size() < requiredSize)
			{
				// wait until enough output is there to avoid entering the ring mutex per frame
				std::unique_lock uLock(m_requestedFramesAvailableMutex);
				m_requestedFrames = requiredSize;
				auto tWait = std::chrono::steady_clock::now();
				while(!m_requestedFramesAvailableCv.wait_for(uLock, std::chrono::seconds(3), [&]()
				{
					if(essi0.getAudioOutputs().size() < requiredSize)
						return false;
					m_requestedFrames = 0;
					return true;
				}))
				{
					// the DSP stopped producing frames, report where it is
					const auto& r = dsp->dsp().regs();
					NMMLOG("audio: DSP produced no frames for %lld s, dsp pc=$%06x sr=$%06x la=$%06x lc=$%06x sp=$%02x pending irq=%d iprc=$%06x hcr=$%02x hsr=$%02x crb0=$%06x instr=%llu, 68k pc=$%06x, outputs=%zu",
						static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - tWait).count()),
						r.pc.var, r.sr.var, r.la.var, r.lc.var, r.sp.var, dsp->dsp().hasPendingInterrupts() ? 1 : 0,
						dsp->getPeriph().read(dsp56k::XIO_IPRC, static_cast<dsp56k::Instruction>(0)), dsp->hdi08().readControlRegister(), dsp->hdi08().readStatusRegister(),
						dsp->getPeriph().getEssi0().readCRB(),
						static_cast<unsigned long long>(dsp->dsp().getInstructionCounter()), m_uc->getPC(), essi0.getAudioOutputs().size());
					for(uint32_t i=1; i<=r.sp.var && i<16; ++i)
						NMMLOG("  ss[%u] = $%06x / $%06x", i, static_cast<uint32_t>(r.ss[i].var >> 24) & 0xffffff, static_cast<uint32_t>(r.ss[i].var) & 0xffffff);
				}
			}

			for(uint32_t i=0; i<processCount; ++i)
			{
				// ESSI1 carries the left channel, ESSI0 the right one: a patch that cables only the
				// output module's "out left" input shows up on ESSI1
				dsp56k::TWord l = 0, r = 0, l2 = 0, r2 = 0;
				essi0.getAudioOutputs().waitNotEmpty();
				essi0.getAudioOutputs().pop_front([&](dsp56k::Audio::TxFrame& _tx) { if(!_tx.empty()) { r = _tx[0][0]; if(_tx.size() > 1) r2 = _tx[1][0]; } });
				essi1.getAudioOutputs().waitNotEmpty();
				essi1.getAudioOutputs().pop_front([&](dsp56k::Audio::TxFrame& _tx) { if(!_tx.empty()) { l = _tx[0][0]; if(_tx.size() > 1) l2 = _tx[1][0]; } });

				m_audioOutputs[0][writePos] = l;
				m_audioOutputs[1][writePos] = r;
				m_audioOutputs[2][writePos] = l2;
				m_audioOutputs[3][writePos] = r2;

				if(outL) outL[writePos] = dacToFloat(l);
				if(outR) outR[writePos] = dacToFloat(r);
				if(_outputs[2]) _outputs[2][writePos] = dacToFloat(l2);
				if(_outputs[3]) _outputs[3][writePos] = dacToFloat(r2);
				++writePos;
			}
		}
	}

	void Hardware::advanceSamples(const uint32_t _samples, const uint32_t _latency)
	{
		// if the latency was higher first but now is lower, we might report < 0 samples. In this case we
		// cannot notify but have to wait for another sample block until we can notify again
		const auto latencyDiff = static_cast<int>(_latency) - static_cast<int>(m_essiLatency);
		m_essiLatency = _latency;

		const auto notifyCount = static_cast<int>(_samples) + latencyDiff + m_dspNotifyCorrection;

		if (notifyCount > 0)
		{
			m_haltDSPSem.notify(notifyCount);
			m_dspNotifyCorrection = 0;
		}
		else
		{
			m_dspNotifyCorrection = notifyCount;
		}
	}

	void Hardware::haltDSPs()
	{
		if(m_dspHalted)
			return;
		m_dspHalted = true;
		for (auto& d : m_dsps)
			if(d->isBooted()) d->getHaltDSP().haltDSP();
	}

	void Hardware::resumeDSPs()
	{
		if(!m_dspHalted)
			return;
		m_dspHalted = false;
		for (auto& d : m_dsps)
			if(d->isBooted()) d->getHaltDSP().resumeDSP();
	}
}
