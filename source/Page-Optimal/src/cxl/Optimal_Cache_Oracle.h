#ifndef OPTIMAL_CACHE_ORACLE_H
#define OPTIMAL_CACHE_ORACLE_H

#include "../ssd/SSD_Defs.h"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace SSD_Components
{
	class Optimal_Cache_Oracle
	{
	public:
		static constexpr uint64_t NO_FUTURE_USE = std::numeric_limits<uint64_t>::max();
		static constexpr uint64_t DEFAULT_LOOKAHEAD_WINDOW = 256ULL * 1024;

		static void Reset();
		static bool LoadTrace(const std::string& trace_file_path,
			uint64_t request_count,
			uint64_t num_sec,
			LHA_type start_lsa_on_device,
			LHA_type end_lsa_on_device);
		static bool IsReady();
		static void ConsumeAccess(uint64_t lba);
		static uint64_t NextUse(uint64_t lba);
		static uint64_t NextUseWithinWindow(uint64_t lba, uint64_t lookahead_window);
		static uint64_t LookaheadWindow();
		static uint64_t LoadedAccessCount();
		static uint64_t ConsumedAccessCount();
	};
}

#endif
