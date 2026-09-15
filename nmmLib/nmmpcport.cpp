#include "nmmpcport.h"

#include "nmmlog.h"

#include "mc68k/mc68k.h"
#include "mc68k/port.h"

namespace nmm
{
	namespace
	{
		// MIDI: 10 bits at 31250 baud = 320 us per byte, at ~21 MHz system clock
		constexpr uint32_t g_cyclesPerByte = 6720;

		constexpr uint8_t g_portERd = 1 << 1;
		constexpr uint8_t g_portEWr = 1 << 2;

		constexpr uint32_t g_tflg2Addr = 0xff923;
		constexpr uint8_t g_tflg2Paovf = 1 << 5;
		constexpr uint8_t g_gptVbaPaovf = 0xa;

		enum Reg : uint8_t
		{
			RegMR = 0,
			RegSR_CSR = 1,
			RegCR = 2,
			RegRB_TB = 3,
			RegIPCR_ACR = 4,
			RegISR_IMR = 5,
			RegCTU = 6,
			RegCTL = 7,
		};
	}

	PcPort::PcPort(mc68k::Mc68k& _uc) : m_uc(_uc)
	{
		m_uc.getPortE().setWriteTXCallback([this](const mc68k::Port& _port)
		{
			onPortEWrite(_port);
		});

		m_uc.getPortGP().setReadRXCallback([this](const mc68k::Port&, const uint8_t _current)
		{
			return onPortGPRead(_current);
		});
	}

	void PcPort::write(const uint8_t _byte)
	{
		std::lock_guard lock(m_mutex);
		m_toSynth.push_back(_byte);
	}

	void PcPort::write(const std::vector<uint8_t>& _bytes)
	{
		std::lock_guard lock(m_mutex);
		for (const auto b : _bytes)
			m_toSynth.push_back(b);
	}

	void PcPort::read(std::vector<uint8_t>& _out)
	{
		std::lock_guard lock(m_mutex);
		_out.assign(m_fromSynth.begin(), m_fromSynth.end());
		m_fromSynth.clear();
	}

	void PcPort::exec(const uint32_t _deltaCycles)
	{
		if(m_cyclesUntilNext > _deltaCycles)
		{
			m_cyclesUntilNext -= _deltaCycles;
			return;
		}
		m_cyclesUntilNext = 0;

		// A byte loaded while the receiver is disabled would raise no interrupt and sit in RBA
		// forever; keep the data queued until the OS has enabled the receiver instead.
		if(m_rxFull || !m_rxEnabled)
			return;

		std::lock_guard lock(m_mutex);
		if(m_toSynth.empty())
			return;

		loadReceiver();
		m_cyclesUntilNext = g_cyclesPerByte;
	}

	void PcPort::loadReceiver()
	{
		m_rxData = m_toSynth.front();
		m_toSynth.pop_front();
		m_rxFull = true;
		++m_rxCount;

		NMMTRACE(io, "PCPORT rx $%02x", m_rxData);

		if(!m_rxEnabled)
			return;

		// receiver-ready pulse overflows the pulse accumulator
		auto& gpt = m_uc.getGPT();
		const auto tflg2 = static_cast<mc68k::PeriphAddress>(g_tflg2Addr);
		gpt.write8(tflg2, static_cast<uint8_t>(gpt.read8(tflg2) | g_tflg2Paovf));
		gpt.injectInterrupt(g_gptVbaPaovf);
	}

	uint8_t PcPort::onPortGPRead(const uint8_t _current)
	{
		// only a read strobe on Port E puts the chip on the bus, otherwise the pins float
		if(m_lastPortE & g_portERd)
			return _current;
		if(m_uc.getPortGP().getDirection() == 0xff)
			return _current;

		return regRead(addrFromPortE(m_lastPortE));
	}

	uint8_t PcPort::regRead(const uint8_t _addr)
	{
		switch (_addr)
		{
		case RegMR:
			return m_mr[m_mrPointer];
		case RegSR_CSR:
			{
				uint8_t sr = SraTxRdy | SraTxEmt;
				if(m_rxFull)
					sr |= SraRxRdy;
				return sr;
			}
		case RegRB_TB:
			{
				const auto b = m_rxData;
				if(m_rxFull)
				{
					m_rxFull = false;
					NMMTRACE(io, "PCPORT read RBA $%02x", b);
				}
				return b;
			}
		case RegISR_IMR:
			return static_cast<uint8_t>((m_rxFull ? 0x02 : 0) | 0x01);	// RXRDYA, TXRDYA
		case RegIPCR_ACR:
			return 0;
		default:
			return 0;
		}
	}

	void PcPort::regWrite(const uint8_t _addr, const uint8_t _val)
	{
		switch (_addr)
		{
		case RegMR:
			m_mr[m_mrPointer] = _val;
			m_mrPointer = 1;
			NMMTRACE(io, "PCPORT MR%u <- $%02x", m_mrPointer, _val);
			break;
		case RegSR_CSR:
			m_csr = _val;
			NMMTRACE(io, "PCPORT CSRA <- $%02x", _val);
			break;
		case RegCR:
			NMMTRACE(io, "PCPORT CRA <- $%02x", _val);
			switch (_val & 0xf0)
			{
			case 0x10: m_mrPointer = 0; break;			// reset MR pointer
			case 0x20: m_rxFull = false; break;			// reset receiver
			case 0x30: break;							// reset transmitter
			case 0x40: break;							// reset error status
			default: break;
			}
			if(_val & 0x01) m_rxEnabled = true;
			if(_val & 0x02) m_rxEnabled = false;
			if(_val & 0x04) m_txEnabled = true;
			if(_val & 0x08) m_txEnabled = false;
			break;
		case RegRB_TB:
			{
				NMMTRACE(io, "PCPORT tx $%02x", _val);
				std::lock_guard lock(m_mutex);
				m_fromSynth.push_back(_val);
				++m_txCount;
			}
			break;
		case RegIPCR_ACR:
			m_acr = _val;
			break;
		case RegISR_IMR:
			m_imr = _val;
			break;
		default:
			break;
		}
	}

	void PcPort::onPortEWrite(const mc68k::Port& _port)
	{
		const auto v = _port.read();
		const auto prev = m_lastPortE;
		m_lastPortE = v;

		// falling edge of the write strobe latches Port GP into the selected register
		if((prev & g_portEWr) && !(v & g_portEWr))
		{
			const auto data = m_uc.getPortGP().read();
			regWrite(addrFromPortE(v), data);
		}
	}
}
