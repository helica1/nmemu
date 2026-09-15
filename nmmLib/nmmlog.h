#pragma once

#include <cstdio>
#include <cstdint>

namespace nmm
{
	// Runtime-switchable tracing for hardware bring-up. Cheap when disabled.
	struct Trace
	{
		static inline bool io = false;			// every peripheral access
		static inline bool hdi = false;			// host port traffic
		static inline bool panel = false;		// panel latches
		static inline bool flash = false;		// flash command sequences
		static inline bool irq = false;			// 68k interrupt acknowledges
		static inline uint32_t watchAddr = 0;	// log RAM reads/writes touching [watchAddr, watchAddr+watchSize)
		static inline uint32_t watchSize = 0;
	};
}

#define NMMLOG(...) do { std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while(0)
#define NMMTRACE(flag, ...) do { if(::nmm::Trace::flag) { std::printf(__VA_ARGS__); std::printf("\n"); } } while(0)
