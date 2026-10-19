#include "OptimalOracle.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

OptimalOracle& OptimalOracle::instance() {
	static OptimalOracle inst;
	return inst;
}

void OptimalOracle::load_trace(const std::string& trace_path) {
	future_.clear();
	cursor_.clear();
	next_use_.clear();
	loaded_ = false;

	std::ifstream fin(trace_path);
	if (!fin.is_open()) {
		std::cerr << "[OptimalOracle] cannot open trace file: " << trace_path << std::endl;
		return;
	}

	std::string line;
	uint64_t index = 0;
	// Default ASCII trace definition (see src/host/ASCII_Trace_Definition.h):
	//   time(0) device(1) address(2) size(3) rw(4)   -- space-delimited.
	// The address column is a byte address, so page_id = address / 4096.
	while (std::getline(fin, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		// Manually locate the 3rd whitespace-separated token (the address),
		// avoiding per-line std::vector<std::string> allocations for speed.
		size_t pos = 0;
		int token = 0;
		size_t addr_begin = std::string::npos;
		size_t addr_end = std::string::npos;
		while (pos < line.size()) {
			while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
				pos++;
			}
			if (pos >= line.size()) {
				break;
			}
			size_t start = pos;
			while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') {
				pos++;
			}
			if (token == 2) {
				addr_begin = start;
				addr_end = pos;
				break;
			}
			token++;
		}

		if (addr_begin == std::string::npos) {
			continue; // not enough columns (e.g. header / malformed line)
		}

		char* endptr = nullptr;
		uint64_t address = std::strtoull(line.c_str() + addr_begin, &endptr, 10);
		if (endptr == line.c_str() + addr_begin) {
			continue; // non-numeric address
		}

		uint64_t page = address / 4096;
		future_[page].push_back(index);
		index++;
	}

	fin.close();
	loaded_ = true;
	std::cout << "[OptimalOracle] loaded " << index << " requests, "
	          << future_.size() << " unique pages" << std::endl;
}

uint64_t OptimalOracle::access(uint64_t page) {
	uint64_t& cursor = cursor_[page];
	cursor++;

	auto it = future_.find(page);
	uint64_t nu = INF;
	if (it != future_.end() && cursor < it->second.size()) {
		nu = it->second[cursor];
	}
	next_use_[page] = nu;
	return nu;
}

uint64_t OptimalOracle::peek(uint64_t page) const {
	auto it = next_use_.find(page);
	if (it == next_use_.end()) {
		return INF;
	}
	return it->second;
}
