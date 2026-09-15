#pragma once

#include <memory>
#include <string>
#include <atomic>

#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/dspthread.h"
#include "dsp56kEmu/dspBootCode.h"
#include "dsp56kEmu/haltDSP.h"
#include "dsp56kEmu/peripherals.h"

namespace mc68k
{
	class Hdi08;
}

namespace nmm
{
	class Hardware;

	// One DSP56303 attached to a host port slot
	class DSP
	{
	public:
		DSP(Hardware& _hw, mc68k::Hdi08& _hdiUc, uint32_t _slot);

		// host wrote its ICR: forward HF0/HF1 into the DSP's HSR
		void onHostIcrWrite(uint8_t _icr);
		~DSP();

		dsp56k::HDI08& hdi08() { return m_periphX.getHI08(); }
		dsp56k::DSP& dsp() { return m_dsp; }
		dsp56k::Peripherals56303& getPeriph() { return m_periphX; }
		dsp56k::Essi& getAudioOut() { return m_periphX.getEssi0(); }

		dsp56k::DSPThread* getDSPThread() const { return m_thread.get(); }
		auto& getHaltDSP() { return m_haltDSP; }

		bool isBooted() const { return m_booted; }
		uint32_t getSlot() const { return m_slot; }
		const std::string& getName() const { return m_name; }

		void terminate();
		void join() const;

		uint64_t getHostWordsToDsp() const { return m_wordsToDsp; }
		uint64_t getDspWordsToHost() const { return m_wordsToHost; }

	private:
		void onDspBootFinished();
		void onUCRxEmpty(bool _needMoreData);
		void hdiTransferUCtoDSP(uint32_t _word);
		void hdiSendIrqToDSP(uint8_t _irq);
		uint8_t hdiUcReadIsr(uint8_t _isr);
		bool hdiTransferDSPtoUC();

		Hardware& m_hardware;
		mc68k::Hdi08& m_hdiUC;

		const uint32_t m_slot;
		const std::string m_name;

		dsp56k::PeripheralsNop m_periphNop;
		dsp56k::Peripherals56303 m_periphX;
		dsp56k::Memory m_memory;
		dsp56k::DSP m_dsp;

		std::unique_ptr<dsp56k::DSPThread> m_thread;

		dsp56k::HaltDSP m_haltDSP;
		dsp56k::DspBoot m_boot;

		std::atomic<bool> m_booted{false};
		uint64_t m_wordsToDsp = 0;
		uint64_t m_wordsToHost = 0;
		uint64_t m_isrPolls = 0;
		uint8_t m_lastHf23 = 0;
		bool m_lastHf0 = false;
		uint32_t m_hf0ClearCount = 0;
		bool m_prevHf0Level = false;
	};
}
