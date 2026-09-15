#include "nmmhardware.h"

#include "nmmlog.h"

#include "dsp56kEmu/interrupts.h"

namespace nmm
{
	Hardware::Hardware(const HardwareConfig& _config)
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
			// no boot rom: place the OS in RAM ourselves and start it
			m_uc->writeRam(g_ramAddress, _config.os.data);
			hleBoot();
		}

		m_valid = true;
	}

	Hardware::~Hardware()
	{
		for (auto& d : m_dsps)
			d->terminate();
		for (auto& d : m_dsps)
			d->join();
	}

	void Hardware::hleBoot()
	{
		// what the boot rom does after copying: jmp $100000. The OS sets SR/SP itself.
		m_uc->setPC(g_osEntryCold);
		m_osStarted = true;
	}

	void Hardware::stepUC()
	{
		m_uc->exec();

		if(!m_osStarted && m_uc->getPC() >= g_ramAddress && m_uc->getPC() < g_ramAddress + g_ramSize)
		{
			m_osStarted = true;
			NMMLOG("68k: entered OS in RAM at pc=$%06x after %llu instructions", m_uc->getPC(), static_cast<unsigned long long>(m_uc->getInstructionCount()));
		}
	}

	void Hardware::runUC(const uint64_t _instructions)
	{
		for(uint64_t i=0; i<_instructions; ++i)
			stepUC();
	}

	void Hardware::onDspBooted(DSP& _dsp)
	{
		// The hardware feeds a sample clock into the DSP's IRQD pin; the kernel's IRQD handler counts
		// frames and the main loop processes the patch once enough have arrived. One IRQD per ESSI
		// transmit frame reproduces that.
		// The emulator core does not gate external interrupts on IPRC, so honour the IRQD enable
		// bits here: the kernel disables IRQD (IDL = 00) while the host uploads a patch.
		auto* dsp = &_dsp;
		_dsp.getAudioOut().setCallback([this, dsp](dsp56k::Audio*)
		{
			onEssiFrame();
			const auto iprc = dsp->getPeriph().read(dsp56k::XIO_IPRC, static_cast<dsp56k::Instruction>(0));
			if((iprc >> 9) & 3)
			{
				++m_irqdInjected;
				dsp->dsp().injectInterrupt(dsp56k::Vba_IRQD);
			}
			else
				++m_irqdMasked;
		});

		// the DSP kernel enables its receivers during init and blocks on an empty input ring,
		// so give it something to chew on right away
		topUpInput(_dsp.getPeriph().getEssi0(), 8192);
		topUpInput(_dsp.getPeriph().getEssi1(), 8192);
	}

	void Hardware::onEssiFrame()
	{
		++m_essiFrames;
	}

	void Hardware::topUpInput(dsp56k::Essi& _essi, const size_t _targetFrames)
	{
		const auto have = _essi.getAudioInputs().size();
		if(have >= _targetFrames)
			return;

		const auto missing = static_cast<uint32_t>(_targetFrames - have);

		_essi.processAudioInput<float>(missing, 0, [](size_t, dsp56k::Audio::RxFrame& _f)
		{
			_f.resize(2);
			_f[0] = dsp56k::Audio::RxSlot{0,0,0,0};
			_f[1] = dsp56k::Audio::RxSlot{0,0,0,0};
		});
	}

	void Hardware::drainOutput(dsp56k::Essi& _essi, const uint32_t _channelBase)
	{
		auto& outs = _essi.getAudioOutputs();

		while(!outs.empty())
		{
			outs.pop_front([&](dsp56k::Audio::TxFrame& _tx)
			{
				if(_channelBase >= g_captureChannels)
					return;
				if(m_capture[_channelBase].size() >= m_captureLimit)
					return;

				const auto s0 = _tx.size() > 0 ? _tx[0][0] : 0;
				const auto s1 = _tx.size() > 1 ? _tx[1][0] : 0;

				m_capture[_channelBase    ].push_back(dsp56k::signextend<int32_t,24>(static_cast<int32_t>(s0)));
				m_capture[_channelBase + 1].push_back(dsp56k::signextend<int32_t,24>(static_cast<int32_t>(s1)));
			});
		}
	}

	void Hardware::serviceAudio()
	{
		for (auto& d : m_dsps)
		{
			if(!d->isBooted())
				continue;

			if(m_dspProfile)
			{
				++m_dspPcHist[d->dsp().getPC().var];
				// watch the IRQD vector target word
				const auto hcr = d->hdi08().readControlRegister();
				if((hcr & 0x08) != (m_lastHcr & 0x08))
				{
					NMMLOG("[%s] HF2 %s, dsp pc=$%06x, DOR0=$%06x P:$17=$%06x", d->getName().c_str(), (hcr & 8) ? "set" : "cleared", d->dsp().getPC().var,
						d->getPeriph().getDMA().getDOR(0), d->dsp().memory().get(dsp56k::MemArea_P, 0x17));
					m_lastHcr = hcr;
				}
				const auto v17 = d->dsp().memory().get(dsp56k::MemArea_P, 0x17);
				if(v17 != m_lastP17)
				{
					NMMLOG("[%s] P:$17 (IRQD vector target) changed $%06x -> $%06x, dsp pc=$%06x", d->getName().c_str(), m_lastP17, v17, d->dsp().getPC().var);
					m_lastP17 = v17;
				}
			}

			auto& periph = d->getPeriph();

			topUpInput(periph.getEssi0(), 4096);
			topUpInput(periph.getEssi1(), 4096);

			// only the first DSP's outputs are captured
			const auto capture = d.get() == m_dsps.front().get();
			drainOutput(periph.getEssi0(), capture ? 0 : g_captureChannels);
			drainOutput(periph.getEssi1(), capture ? 2 : g_captureChannels);
		}
	}
}
