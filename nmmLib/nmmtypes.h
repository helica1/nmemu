#pragma once

#include <cstdint>

namespace nmm
{
	/*
	Clavia Nord Modular / Micro Modular host memory map, recovered from the rack boot ROM's
	MC68331 chip-select programming (SIM registers at $FFFA00) and from the OS images.

	CHIP SELECT   START      SIZE      DEVICE
	CSBOOT        $000000    512K      boot flash (boot code, update utility, factory OS copy)
	CS8/9/10      $100000    1M        RAM, the OS is copied here by the boot ROM and runs from RAM
	CS0           $200000    2K        DSP56303 host ports (HDI08), one 8 byte register block per DSP slot
	CS4           $201800    2K        front panel input (buttons / dial)
	CS2           $202000    2K        front panel output latches (LEDs, display)
	CS7           $300000    1M        second flash: field-updated OS + patch storage
	*/

	static constexpr uint32_t g_bootRomAddress		= 0x000000;
	static constexpr uint32_t g_bootRomSize			= 0x080000;

	static constexpr uint32_t g_ramAddress			= 0x100000;
	static constexpr uint32_t g_ramSize				= 0x100000;

	// boot rom + ram live in one power-of-two sized buffer for fast access, like n2x does
	static constexpr uint32_t g_romRamSize			= g_ramAddress + g_ramSize;	// $200000

	static constexpr uint32_t g_dspWindowAddress	= 0x200000;
	static constexpr uint32_t g_dspWindowSize		= 0x000800;
	static constexpr uint32_t g_dspSlotSize			= 8;
	static constexpr uint32_t g_dspSlotCount		= 32;			// 32 * 8 = $100, the window repeats above that

	static constexpr uint32_t g_panelInAddress		= 0x201800;
	static constexpr uint32_t g_panelInSize			= 0x000800;

	static constexpr uint32_t g_panelOutAddress		= 0x202000;
	static constexpr uint32_t g_panelOutSize		= 0x000800;

	static constexpr uint32_t g_panelAuxAddress		= 0x202800;
	static constexpr uint32_t g_panelAuxSize		= 0x000800;

	static constexpr uint32_t g_flashAddress		= 0x300000;
	static constexpr uint32_t g_flashWindowSize		= 0x100000;
	static constexpr uint32_t g_flashChipSize		= 0x100000;	// Am29F080, 1M, fills the window

	// layout of the OS flash as read by the boot ROM
	static constexpr uint32_t g_flashOsVersionOffset	= 0x04;	// word, major.minor as two bytes
	static constexpr uint32_t g_flashOsLengthOffset		= 0x08;	// long, OS length in bytes
	static constexpr uint32_t g_flashOsDataOffset		= 0x20;	// OS image, copied to $100000

	// OS entry points once copied to RAM
	static constexpr uint32_t g_osEntryCold			= 0x100000;
	static constexpr uint32_t g_osEntryWarm			= 0x10001e;

	// audio
	static constexpr uint32_t g_samplerate			= 96000;		// Nord Modular runs its DSPs at 96 kHz
	static constexpr uint32_t g_samplerateHost		= g_samplerate;
	// DSP56303 crystal. The boot stub programs PCTL = $3C001A (MF=27, PD=4), so a 14.7456 MHz
	// crystal yields the 99.5 MHz core clock of a 100 MHz part. Unverified, tune when audio is up.
	static constexpr uint32_t g_dspExtalHz			= 14'745'600;

	enum class Model
	{
		MicroModular,	// 1 DSP
		Modular,		// 4 DSPs (+4 with expansion)
	};
}
