#include "nmmhardware.h"

#include <cassert>
#include <chrono>

#include "nmmlog.h"

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
			while(!m_bootFinished)
				processAudio(ins, outs, 8, 8);
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
			std::fill(m_audioOutputs[0].begin(), m_audioOutputs[0].begin() + _frames, 0);
			std::fill(m_audioOutputs[1].begin(), m_audioOutputs[1].begin() + _frames, 0);
			std::this_thread::sleep_for(std::chrono::microseconds(_frames * 1000000ull / g_samplerate));
			return;
		}

		auto& essi0 = dsp->getPeriph().getEssi0();
		auto& essi1 = dsp->getPeriph().getEssi1();

		// audio inputs: ESSI0 slot 0 = left, ESSI1 slot 0 = right. The ADC words sit in the low 16
		// bits like the DAC side does
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

			pushInputs(essi0, inL + writePos, processCount);
			pushInputs(essi1, inR + writePos, processCount);

			advanceSamples(processCount, _latency);

			const auto requiredSize = processCount > 8 ? processCount - 8 : 0;

			if(essi0.getAudioOutputs().size() < requiredSize)
			{
				// wait until enough output is there to avoid entering the ring mutex per frame
				std::unique_lock uLock(m_requestedFramesAvailableMutex);
				m_requestedFrames = requiredSize;
				m_requestedFramesAvailableCv.wait(uLock, [&]()
				{
					if(essi0.getAudioOutputs().size() < requiredSize)
						return false;
					m_requestedFrames = 0;
					return true;
				});
			}

			for(uint32_t i=0; i<processCount; ++i)
			{
				dsp56k::TWord l = 0, r = 0;
				essi0.getAudioOutputs().waitNotEmpty();
				essi0.getAudioOutputs().pop_front([&](dsp56k::Audio::TxFrame& _tx) { if(!_tx.empty()) l = _tx[0][0]; });
				essi1.getAudioOutputs().waitNotEmpty();
				essi1.getAudioOutputs().pop_front([&](dsp56k::Audio::TxFrame& _tx) { if(!_tx.empty()) r = _tx[0][0]; });

				m_audioOutputs[0][writePos] = l;
				m_audioOutputs[1][writePos] = r;

				// 16 bit DAC words in the low bits, with the kernel's offset trim removed
				const auto fl = static_cast<float>(static_cast<int16_t>(l & 0xffff) - 341) / 32768.0f;
				const auto fr = static_cast<float>(static_cast<int16_t>(r & 0xffff) - 341) / 32768.0f;
				if(outL) outL[writePos] = fl;
				if(outR) outR[writePos] = fr;
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
