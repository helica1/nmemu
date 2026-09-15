#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace mc68k
{
	class Mc68k;
	class Port;
}

namespace nmm
{
	/*
	The "PC" MIDI port: channel A of a MC68681-style DUART hung off the 68331's Port GP data
	lines, addressed and strobed through Port E:

	  Port E bit 1 = RD strobe (active low)
	  Port E bit 2 = WR strobe (active low)
	  Port E bit 3 = A0, bit 6 = A1, bit 7 = A2

	  A=0 MR1A/MR2A   A=1 SRA/CSRA   A=2 CRA   A=3 RBA/TBA   A=4 ACR   A=5 ISR/IMR   A=6,7 CTUR/CTLR

	The chip's receiver-ready output clocks the GPT pulse accumulator, which the OS keeps
	preloaded at $FF so every received byte overflows it and raises the PAOVF interrupt; the
	handler then reads RBA. The OS polls SRA.TXRDY before writing TBA.
	*/
	class PcPort
	{
	public:
		explicit PcPort(mc68k::Mc68k& _uc);

		// host -> synth
		void write(uint8_t _byte);
		void write(const std::vector<uint8_t>& _bytes);

		// synth -> host
		void read(std::vector<uint8_t>& _out);

		void exec(uint32_t _deltaCycles);

		// the OS enables the receiver once it is ready to talk to an editor
		bool isReceiverEnabled() const { return m_rxEnabled; }
		uint64_t getRxCount() const { return m_rxCount; }
		uint64_t getTxCount() const { return m_txCount; }

	private:
		enum SraBits : uint8_t
		{
			SraRxRdy = 1 << 0,
			SraFFull = 1 << 1,
			SraTxRdy = 1 << 2,
			SraTxEmt = 1 << 3,
		};

		void onPortEWrite(const mc68k::Port& _port);
		uint8_t onPortGPRead(uint8_t _current);
		uint8_t regRead(uint8_t _addr);
		void regWrite(uint8_t _addr, uint8_t _val);
		void loadReceiver();

		static uint8_t addrFromPortE(const uint8_t _e)
		{
			return static_cast<uint8_t>(((_e >> 3) & 1) | (((_e >> 6) & 1) << 1) | (((_e >> 7) & 1) << 2));
		}

		mc68k::Mc68k& m_uc;

		std::mutex m_mutex;
		std::deque<uint8_t> m_toSynth;
		std::deque<uint8_t> m_fromSynth;

		bool m_rxFull = false;			// RBA holds a byte
		uint8_t m_rxData = 0;
		bool m_rxEnabled = false;
		bool m_txEnabled = false;
		uint8_t m_mrPointer = 0;
		uint8_t m_mr[2] = {0, 0};
		uint8_t m_csr = 0;
		uint8_t m_acr = 0;
		uint8_t m_imr = 0;

		uint32_t m_cyclesUntilNext = 0;
		uint8_t m_lastPortE = 0xff;
		uint64_t m_rxCount = 0;
		uint64_t m_txCount = 0;
	};
}
