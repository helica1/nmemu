#include "nmmdsp.h"

#include <thread>
#include <chrono>
#include <cstdlib>
#include <string>

#include "nmmhardware.h"
#include "nmmlog.h"

#include "mc68k/hdi08.h"

namespace nmm
{
	// DSP56303: 4K P, 2K X, 2K Y internal. Everything above is external SRAM shared between the
	// address spaces. Sizes are generous, the module code library and delay lines live out there.
	static constexpr dsp56k::TWord g_pMemSize			= 0x040000;
	static constexpr dsp56k::TWord g_xyMemSize			= 0x040000;
	static constexpr dsp56k::TWord g_externalMemAddr	= 0x000800;

	namespace
	{
		dsp56k::DefaultMemoryValidator g_memValidator;
	}

	DSP::DSP(Hardware& _hw, mc68k::Hdi08& _hdiUc, const uint32_t _slot)
		: m_hardware(_hw)
		, m_hdiUC(_hdiUc)
		, m_slot(_slot)
		, m_name("DSP" + std::to_string(_slot))
		, m_memory(g_memValidator, g_pMemSize, g_xyMemSize, g_externalMemAddr)
		, m_dsp(m_memory, &m_periphX, &m_periphNop)
		, m_haltDSP(m_dsp)
		, m_boot(m_dsp)
	{
		auto& clock = m_periphX.getEssiClock();
		// NMM_DSP_CLOCK_SCALE=1.5 gives the emulated DSP 50% more cycles per sample (experiments)
		const auto clockScale = std::getenv("NMM_DSP_CLOCK_SCALE") ? std::atof(std::getenv("NMM_DSP_CLOCK_SCALE")) : 1.0;
		clock.setExternalClockFrequency(static_cast<uint32_t>(g_dspExtalHz * (clockScale > 0.1 ? clockScale : 1.0)));
		clock.setSamplerate(g_samplerate);
		clock.setClockSource(dsp56k::EsxiClock::ClockSource::Cycles);

		auto config = m_dsp.getJit().getConfig();
		config.aguSupportBitreverse = true;
		config.linkJitBlocks = true;
		config.dynamicPeripheralAddressing = true;	// the kernel loads DMA offset registers through host commands that write x:(r0)
		config.maxInstructionsPerBlock = 0;
		config.support16BitSCMode = true;
		config.dynamicFastInterrupts = true;
		m_dsp.getJit().setConfig(config);

		// fill P memory with a debug instruction so a jump into garbage is visible
		for(dsp56k::TWord i=0; i<m_memory.sizeP(); ++i)
		{
			m_memory.set(dsp56k::MemArea_P, i, 0x000200);
			m_dsp.getJit().notifyProgramMemWrite(i);
		}

		hdi08().setRXRateLimit(0);

		m_periphX.getEssi0().writeEmptyAudioIn(2);
		m_periphX.getEssi1().writeEmptyAudioIn(2);

		m_hdiUC.setRxEmptyCallback([this](const bool _needMoreData)
		{
			onUCRxEmpty(_needMoreData);
		});

		m_hdiUC.setWriteTxCallback([this](const uint32_t _word)
		{
			++m_wordsToDsp;
			NMMTRACE(hdi, "[%s] boot word $%06x", m_name.c_str(), _word);
			if(m_boot.hdiWriteTX(_word))
				onDspBootFinished();
		});

		m_hdiUC.setWriteIrqCallback([this](const uint8_t _irq)
		{
			hdiSendIrqToDSP(_irq);
		});

		m_hdiUC.setReadIsrCallback([this](const uint8_t _isr)
		{
			return hdiUcReadIsr(_isr);
		});

		m_hdiUC.setInitHdi08Callback([this]
		{
			// clear init flag again immediately, code is waiting for it to happen
			m_hdiUC.icr(m_hdiUC.icr() & 0x7f);
			m_hdiUC.isr(m_hdiUC.isr() | mc68k::Hdi08::IsrBits::Txde | mc68k::Hdi08::IsrBits::Trdy);
		});
	}

	DSP::~DSP()
	{
		terminate();
		join();
	}

	void DSP::terminate()
	{
		if(m_thread)
			m_thread->terminate();
	}

	void DSP::join() const
	{
		if(m_thread)
			m_thread->join();
	}

	void DSP::onDspBootFinished()
	{
		NMMLOG("[%s] boot finished: %u words, initial PC $%06x", m_name.c_str(), m_boot.getLength(), m_boot.getInitialPC());

		m_hdiUC.setWriteTxCallback([this](const uint32_t _word)
		{
			hdiTransferUCtoDSP(_word);
		});

		m_thread.reset(new dsp56k::DSPThread(m_dsp, m_name.c_str()));
		m_thread->setLogToStdout(false);

		m_booted = true;
		m_hardware.onDspBooted(*this);
	}

	void DSP::onHostIcrWrite(const uint8_t _icr)
	{
		const uint8_t hf0 = (_icr & mc68k::Hdi08::IcrBits::Hf0) ? 1 : 0;
		const uint8_t hf1 = (_icr & mc68k::Hdi08::IcrBits::Hf1) ? 1 : 0;
		NMMTRACE(hdi, "[%s] host flags HF0=%u HF1=%u", m_name.c_str(), hf0, hf1);
		const bool hf0Cleared = !hf0 && m_lastHf0;
		m_lastHf0 = hf0 != 0;
		const bool hf0Set = hf0 && !hf0Cleared && m_lastHf0 && false;	// placeholder, see below
		(void)hf0Set;
		const bool onSet = std::getenv("NMM_DSPTRACE_ON") && std::string(std::getenv("NMM_DSPTRACE_ON")) == "set";
		const bool trigger = onSet ? (hf0 != 0 && !m_prevHf0Level) : hf0Cleared;
		m_prevHf0Level = hf0 != 0;
		if(trigger) ++m_hf0ClearCount;
		const uint32_t traceNth = std::getenv("NMM_DSPTRACE_NTH") ? static_cast<uint32_t>(std::atoi(std::getenv("NMM_DSPTRACE_NTH"))) : 1;
		if(trigger && std::getenv("NMM_DSPTRACE") && m_booted && m_hf0ClearCount == traceNth)
		{
			// debugging aid: trace DSP instructions from the patch handshake on, the hardware stops it after some frames
			NMMLOG("[%s] DSP instruction trace enabled", m_name.c_str());
			m_dsp.enableTrace(static_cast<dsp56k::DSP::TraceMode>(dsp56k::DSP::Ops | dsp56k::DSP::StackIndent | (std::getenv("NMM_DSPTRACE_REGS") ? dsp56k::DSP::Regs : 0)));
			m_hardware.setDspTraceFrames(static_cast<uint32_t>(std::atoi(std::getenv("NMM_DSPTRACE"))));
		}
		// handed over as pending flags: the DSP thread applies them on its next HSR read, which avoids
		// racing its own read-modify-writes of the status word
		hdi08().setPendingHostFlags01((static_cast<uint32_t>(hf0) << dsp56k::HDI08::HSR_HF0) | (static_cast<uint32_t>(hf1) << dsp56k::HDI08::HSR_HF1));
	}

	void DSP::onUCRxEmpty(const bool _needMoreData)
	{
		if(_needMoreData && m_booted)
		{
			dsp56k::ScopedResumeDSP r(getHaltDSP());
			const auto t0 = std::chrono::steady_clock::now();
			while(dsp().hasPendingInterrupts())
			{
				std::this_thread::yield();
				if(std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(500))
					break;
			}
		}
		hdiTransferDSPtoUC();
	}

	void DSP::hdiTransferUCtoDSP(const uint32_t _word)
	{
		++m_wordsToDsp;
		NMMTRACE(hdi, "[%s] toDSP $%06x", m_name.c_str(), _word);
		hdi08().writeRX(&_word, 1);
	}

	void DSP::hdiSendIrqToDSP(const uint8_t _irq)
	{
		NMMTRACE(hdi, "[%s] host command irq $%02x", m_name.c_str(), _irq);

		if(!m_booted)
			return;

		dsp().injectExternalInterrupt(_irq);

		dsp56k::ScopedResumeDSP r(getHaltDSP());

		// wait for the DSP to pick the interrupt up, but never forever: a DSP that is blocked on
		// audio I/O would otherwise wedge the UC thread too
		const auto t0 = std::chrono::steady_clock::now();
		while(dsp().hasPendingExternalInterrupts())
		{
			std::this_thread::yield();
			if(std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(500))
			{
				NMMLOG("[%s] warning: DSP did not accept host command irq $%02x within 500 ms, dsp pc=$%06x", m_name.c_str(), _irq, dsp().getPC().var);
				break;
			}
		}

		hdiTransferDSPtoUC();
	}

	uint8_t DSP::hdiUcReadIsr(uint8_t _isr)
	{
		hdiTransferDSPtoUC();

		// transfer DSP host flags HF2&3 to uc
		const auto hf23 = hdi08().readControlRegister() & 0x18;

		if(Trace::hdi && ((++m_isrPolls & 8191) == 1 || hf23 != m_lastHf23))
		{
			NMMLOG("[%s] ISR poll #%llu: hcr=$%06x hsr=$%06x dsp pc=$%06x", m_name.c_str(), static_cast<unsigned long long>(m_isrPolls), hdi08().readControlRegister(), hdi08().readStatusRegister(), dsp().getPC().var);
			m_lastHf23 = static_cast<uint8_t>(hf23);
		}
		_isr &= ~0x18;
		_isr |= static_cast<uint8_t>(hf23);
		// always ready to receive more data
		_isr |= mc68k::Hdi08::IsrBits::Trdy;
		return _isr;
	}

	bool DSP::hdiTransferDSPtoUC()
	{
		if (m_hdiUC.canReceiveData() && hdi08().hasTX())
		{
			const auto v = hdi08().readTX();
			++m_wordsToHost;
			NMMTRACE(hdi, "[%s] toUC $%06x", m_name.c_str(), v);
			m_hdiUC.writeRx(v);
			return true;
		}
		return false;
	}
}
