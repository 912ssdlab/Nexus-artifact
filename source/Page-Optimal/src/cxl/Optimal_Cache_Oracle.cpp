#include "Optimal_Cache_Oracle.h"

#include "../host/ASCII_Trace_Definition.h"
#include "../utils/StringTools.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

namespace SSD_Components
{
	namespace
	{
		struct FutureAccessList {
			std::vector<uint64_t> positions;
			uint64_t cursor{0};
		};

		std::unordered_map<uint64_t, FutureAccessList> future_accesses;
		uint64_t loaded_access_count{0};
		uint64_t consumed_access_count{0};
			bool oracle_ready{false};

			uint64_t configured_lookahead_window()
			{
				// Artifact policy: fixed for every workload and cache size.
				return Optimal_Cache_Oracle::DEFAULT_LOOKAHEAD_WINDOW;
			}

		LHA_type cxl_lba_from_memory_address(uint64_t memory_addr,
			uint64_t num_sec,
			LHA_type start_lsa_on_device,
			LHA_type end_lsa_on_device)
		{
			if (num_sec == 0) {
				num_sec = 8;
			}

			const uint64_t page_bytes = num_sec * 512;
			LHA_type lba = memory_addr / page_bytes;
			LHA_type page_base_addr = lba * page_bytes;
			LHA_type sector_offset = (memory_addr - page_base_addr) / 512;
			LHA_type lsa = lba * num_sec + sector_offset;

			if (lsa < start_lsa_on_device || lsa > end_lsa_on_device) {
				const LHA_type span = end_lsa_on_device - start_lsa_on_device;
				if (span > 0) {
					lsa = start_lsa_on_device + (lsa % span);
					lba = lsa / num_sec;
				}
			}

			return lba;
		}

		bool parse_trace_lba(const std::string& trace_line,
			uint64_t num_sec,
			LHA_type start_lsa_on_device,
			LHA_type end_lsa_on_device,
			uint64_t& lba)
		{
			if (trace_line.empty()) {
				return false;
			}

			std::string line = trace_line;
			Utils::Helper_Functions::Remove_cr(line);
			if (line.empty()) {
				return false;
			}

			std::vector<std::string> tokens;
			Utils::Helper_Functions::Tokenize(line, ASCIILineDelimiter, tokens);
			if (tokens.size() != ASCIIItemsPerLine) {
				return false;
			}
			if (tokens[ASCIITraceTypeColumn].compare(ASCIITraceWriteCode) != 0
				&& tokens[ASCIITraceTypeColumn].compare(ASCIITraceReadCode) != 0) {
				return false;
			}

			char* end_ptr = nullptr;
			uint64_t memory_addr = std::strtoull(tokens[ASCIITraceAddressColumn].c_str(), &end_ptr, 0);
			lba = cxl_lba_from_memory_address(memory_addr, num_sec, start_lsa_on_device, end_lsa_on_device);
			return true;
		}
	}

	void Optimal_Cache_Oracle::Reset()
	{
		future_accesses.clear();
		loaded_access_count = 0;
		consumed_access_count = 0;
		oracle_ready = false;
	}

	bool Optimal_Cache_Oracle::LoadTrace(const std::string& trace_file_path,
		uint64_t request_count,
		uint64_t num_sec,
		LHA_type start_lsa_on_device,
		LHA_type end_lsa_on_device)
	{
		Reset();
		if (request_count == 0) {
			return false;
		}

		std::string trace_line;
		uint64_t replay_rounds = 0;
		while (loaded_access_count < request_count) {
			std::ifstream trace_file(trace_file_path, std::ios::in);
			if (!trace_file.is_open()) {
				std::cerr << "Optimal cache oracle failed to open trace: " << trace_file_path << std::endl;
				return false;
			}

#if defined(OLD_TRACE)
			std::string ignored_line;
			std::getline(trace_file, ignored_line);
			std::getline(trace_file, ignored_line);
			std::getline(trace_file, ignored_line);
#endif

			uint64_t loaded_before_round = loaded_access_count;
			while (loaded_access_count < request_count && std::getline(trace_file, trace_line)) {
				uint64_t lba = 0;
				if (!parse_trace_lba(trace_line, num_sec, start_lsa_on_device, end_lsa_on_device, lba)) {
					continue;
				}
				future_accesses[lba].positions.push_back(loaded_access_count);
				loaded_access_count++;
			}

			replay_rounds++;
			if (loaded_access_count == loaded_before_round) {
				break;
			}
		}

		oracle_ready = loaded_access_count > 0;
		std::cout << "Optimal cache oracle loaded " << loaded_access_count
			<< " future accesses in " << replay_rounds
			<< " pass(es) from " << trace_file_path
				<< "; lookahead window ";
			if (LookaheadWindow() == 0) {
				std::cout << "full";
			}
			else {
				std::cout << LookaheadWindow() << " requests";
			}
			std::cout << std::endl;
		return oracle_ready;
	}

	bool Optimal_Cache_Oracle::IsReady()
	{
		return oracle_ready;
	}

	void Optimal_Cache_Oracle::ConsumeAccess(uint64_t lba)
	{
		if (!oracle_ready) {
			return;
		}

		auto it = future_accesses.find(lba);
		if (it != future_accesses.end()) {
			auto& access_list = it->second;
			while (access_list.cursor < access_list.positions.size()
				&& access_list.positions[access_list.cursor] < consumed_access_count) {
				access_list.cursor++;
			}
			if (access_list.cursor < access_list.positions.size()
				&& access_list.positions[access_list.cursor] == consumed_access_count) {
				access_list.cursor++;
			}
		}
		consumed_access_count++;
	}

	uint64_t Optimal_Cache_Oracle::NextUse(uint64_t lba)
	{
		if (!oracle_ready) {
			return NO_FUTURE_USE;
		}

		auto it = future_accesses.find(lba);
		if (it == future_accesses.end()) {
			return NO_FUTURE_USE;
		}

		auto& access_list = it->second;
		while (access_list.cursor < access_list.positions.size()
			&& access_list.positions[access_list.cursor] < consumed_access_count) {
			access_list.cursor++;
		}
		if (access_list.cursor >= access_list.positions.size()) {
			return NO_FUTURE_USE;
		}
		return access_list.positions[access_list.cursor];
	}

	uint64_t Optimal_Cache_Oracle::NextUseWithinWindow(uint64_t lba, uint64_t lookahead_window)
	{
		uint64_t next_use = NextUse(lba);
		if (next_use == NO_FUTURE_USE) {
			return NO_FUTURE_USE;
		}
		if (lookahead_window == 0) {
			return next_use;
		}
		if (next_use - consumed_access_count >= lookahead_window) {
			return NO_FUTURE_USE;
		}
		return next_use;
	}

	uint64_t Optimal_Cache_Oracle::LookaheadWindow()
	{
		static const uint64_t lookahead_window = configured_lookahead_window();
		return lookahead_window;
	}

	uint64_t Optimal_Cache_Oracle::LoadedAccessCount()
	{
		return loaded_access_count;
	}

	uint64_t Optimal_Cache_Oracle::ConsumedAccessCount()
	{
		return consumed_access_count;
	}
}
