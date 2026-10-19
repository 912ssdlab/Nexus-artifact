#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Belady / MIN optimal (offline) page-cache replacement oracle.
//
// Before the simulation runs, the whole trace is scanned once to record, for
// every page (page_id = byte_address / 4096), the list of request positions at
// which the page is accessed in the future.
//
// During the simulation the oracle answers, for each page access, the position
// of that page's NEXT future access (or INF if it will never be accessed again).
// This next-use value is the only input needed for the optimal victim selection
// (evict the cached page whose next use is furthest in the future; pages that
// are never used again are evicted first).
class OptimalOracle {
public:
	// Sentinel meaning "this page will never be accessed again in the future".
	static constexpr uint64_t INF = UINT64_MAX;

	static OptimalOracle& instance();

	// Scan the trace and build the future-access lists.
	// trace_path: the same .trace file consumed by IO_Flow_Trace_Based
	// (default ASCII format: "time device address size rw", address at column 2).
	void load_trace(const std::string& trace_path);

	bool is_loaded() const { return loaded_; }

	// Call exactly once per page access (page hit or page miss-insertion).
	// Advances the page's future-access cursor and returns the next-use position
	// (INF if the page will never be accessed again).
	uint64_t access(uint64_t page);

	// Return the page's current next-use position WITHOUT advancing the cursor.
	// Returns INF if the page has never been registered.
	uint64_t peek(uint64_t page) const;

private:
	OptimalOracle() = default;

	bool loaded_{ false };

	// page -> trace positions (request indices) where the page is accessed, in
	// increasing order.
	std::unordered_map<uint64_t, std::vector<uint64_t>> future_;

	// page -> number of accesses already consumed == index of the next future
	// occurrence inside future_[page].
	std::unordered_map<uint64_t, uint64_t> cursor_;

	// page -> current next-use position (or INF). Kept up to date by access().
	std::unordered_map<uint64_t, uint64_t> next_use_;
};
