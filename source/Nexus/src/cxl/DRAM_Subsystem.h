#ifndef DRAM_SUBSYSTEM_H
#define DRAM_SUBSYSTEM_H

// Isolated ablation: 1 = conservative refill age; 2 = plus bounded tail pruning.
#ifndef MYWORK_REPLACEMENT_VARIANT
#define MYWORK_REPLACEMENT_VARIANT 2
#endif

#include <iostream>
#include <cstdint>

#include <set>
#include <map>
#include <vector>
#include <list>
#include "CXL_Config.h"
#include "OutputLog.h"
#include "lrfu_heap.h"
#include "CFLRU.h"

#include <cstdint>
#include <list>
#include <unordered_map>
#include <optional>

#include <cstdint>
#include <list>
#include <unordered_map>
#include <optional>
#include <vector>
#include <array>
#include <functional>
#include <limits>
#include <algorithm>
#include <utility>
#include <tuple>
#ifdef T0
#pragma push_macro("T0")
#undef T0
#define MQSIM_RESTORE_T0_AFTER_PBDS
#endif
#include <ext/pb_ds/assoc_container.hpp>
#include <ext/pb_ds/tree_policy.hpp>
#ifdef MQSIM_RESTORE_T0_AFTER_PBDS
#pragma pop_macro("T0")
#undef MQSIM_RESTORE_T0_AFTER_PBDS
#endif

#include <iostream>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <optional>

class LRUSet {
public:
	static constexpr uint64_t lines_per_page{64};
	static constexpr uint32_t invalid_entry_id{std::numeric_limits<uint32_t>::max()};

	struct Entry {
		uint64_t key;
		bool dirty;
	};

	struct EvictedEntry {
		uint64_t key;
		bool dirty;
	};

	struct EvictedSegment {
		uint64_t lba;
		uint64_t mask;
		uint64_t used_mask;
		bool dirty;
		int64_t metadata_bytes_released{0};
	};

	explicit LRUSet(std::size_t capacity,
		uint64_t metadata_entry_bytes = cxl_config::default_cache_metadata_entry_bytes)
		: capacity_(capacity), admission_metadata_entry_bytes_(metadata_entry_bytes) {}

	bool setDirty(uint64_t key, bool dirty)
	{
		uint32_t id = find_segment_id(key);
		if (id == invalid_entry_id) {
			return false;
		}
		setDirtyMaskBit(key, dirty);
		bool was_dirty = entries_[id].dirty;
		if (dirty) {
			entries_[id].dirty = true;
		}
		else {
			entries_[id].dirty = segment_has_dirty_lines(entries_[id]);
		}
		if (was_dirty != entries_[id].dirty) refresh_policy_candidates(id);
		return true;
	}

	void remove(uint64_t key)
	{
		uint32_t id = find_segment_id(key);
		if (id == invalid_entry_id) {
			return;
		}
		uint64_t lba = key / lines_per_page;
		uint64_t bit = 1ULL << (key % lines_per_page);
		uint64_t retained = segment_page_mask(entries_[id]) & ~bit;
		bool inherit_dirty = entries_[id].dirty;
		replace_entry_at_same_lru_position(id, retained, inherit_dirty);
		remove_page_bits(lba, bit);
	}

	uint64_t index_bytes() const { return index_bytes_; }
	uint64_t modeled_segment_entry_count() const { return active_entry_count_; }
	uint64_t resident_mask_for_page(uint64_t lba) const
	{
		auto it = page_line_masks_.find(lba);
		return it == page_line_masks_.end() ? 0 : it->second;
	}
	uint64_t modeled_segment_entry_count_for_mask(uint64_t mask) const
	{
		return managed_entry_count_for_mask(mask);
	}
	uint64_t modeled_metadata_bytes_for_mask(uint64_t mask) const
	{
		return metadata_bytes_for_mask(mask);
	}
	// Admission used this fixed estimate before the index redesign.  Keep it
	// unchanged so the admission policy itself is not altered by this patch.
	uint64_t modeled_entry_metadata_bytes() const
	{
		return admission_metadata_entry_bytes_;
	}

	int lookup_segment_order_for_key(uint64_t key) const
	{
		uint32_t id = find_segment_id(key);
		return id == invalid_entry_id ? -1 : static_cast<int>(entries_[id].order);
	}

	uint64_t managed_entry_count(uint64_t length) const
	{
		if (length == 0 || (length & (length - 1)) != 0) {
			return 0;
		}
		uint8_t order = order_for_length(length);
		return order <= 6 ? segment_tables_[order].size() : 0;
	}

	double clean_first_eviction_scan_fraction() const
	{
		return static_cast<double>(tail_unused_scan_percent_) / 100.0;
	}
	uint64_t clean_first_scan_count() const { return clean_first_scan_count_; }
	double average_clean_first_scan_entries() const
	{
		return clean_first_scan_count_ == 0 ? 0.0
			: static_cast<double>(clean_first_scanned_entry_total_)
				/ static_cast<double>(clean_first_scan_count_);
	}
	uint64_t clean_page_eviction_selection_count() const
	{
		return clean_page_eviction_selection_count_;
	}
	uint64_t clean_segment_eviction_selection_count() const
	{
		return clean_segment_eviction_selection_count_;
	}
	uint64_t lru_fallback_eviction_selection_count() const
	{
		return lru_fallback_eviction_selection_count_;
	}
	uint64_t unused_reclamation_count() const { return unused_reclamation_count_; }
	uint64_t clean_whole_segment_selection_count() const { return clean_whole_segment_selection_count_; }

	uint64_t dirty_mask_for_page(uint64_t lba) const
	{
		auto it = page_dirty_masks_.find(lba);
		return it == page_dirty_masks_.end() ? 0 : it->second;
	}

	void mark_used(uint64_t key)
	{
		uint32_t id = find_segment_id(key);
		if (id == invalid_entry_id) {
			return;
		}
		mark_entry_used(id, 1ULL << (key % lines_per_page));
	}

	void mark_used_mask(uint64_t lba, uint64_t mask)
	{
		uint64_t used = mask & resident_mask_for_page(lba);
		if (used == 0) {
			return;
		}
		uint64_t remaining = used;
		while (remaining != 0) {
			uint64_t idx = static_cast<uint64_t>(__builtin_ctzll(remaining));
			uint32_t id = find_segment_id(lba * lines_per_page + idx);
			if (id == invalid_entry_id) {
				remaining &= ~(1ULL << idx);
				continue;
			}
			uint64_t entry_mask = segment_page_mask(entries_[id]);
			mark_entry_used(id, used & entry_mask);
			remaining &= ~entry_mask;
		}
	}

	void mark_page_clean(uint64_t lba)
	{
		uint64_t remaining = resident_mask_for_page(lba);
		while (remaining != 0) {
			uint64_t idx = static_cast<uint64_t>(__builtin_ctzll(remaining));
			uint32_t id = find_segment_id(lba * lines_per_page + idx);
			if (id == invalid_entry_id) {
				remaining &= ~(1ULL << idx);
				continue;
			}
			if (entries_[id].dirty) {
				entries_[id].dirty = false;
				refresh_policy_candidates(id);
			}
			remaining &= ~segment_page_mask(entries_[id]);
		}
		page_dirty_masks_.erase(lba);
	}

	bool Is_Dirty(uint64_t key)
	{
		uint32_t id = find_segment_id(key);
		return id != invalid_entry_id && entries_[id].dirty;
	}

	std::optional<EvictedEntry> put(uint64_t key, bool dirty = false)
	{
		auto evicted = put_segment(key / lines_per_page, key % lines_per_page, 1, dirty);
		if (!evicted) {
			return std::nullopt;
		}
		uint64_t first_idx = static_cast<uint64_t>(__builtin_ctzll(evicted->mask));
		return EvictedEntry{evicted->lba * lines_per_page + first_idx, evicted->dirty};
	}

	std::optional<EvictedSegment> put_segment(
		uint64_t lba, uint64_t start, uint64_t length, bool dirty = false)
	{
		if (length == 0 || start >= lines_per_page) {
			return std::nullopt;
		}
		if (start + length > lines_per_page) {
			length = lines_per_page - start;
		}
		uint64_t range_mask = mask_for_range(start, length);
		uint64_t old_mask = resident_mask_for_page(lba);
		uint64_t missing_mask = range_mask & ~old_mask;
		uint64_t base = lba * lines_per_page;

		for (const auto& chunk : decompose_page_mask(lba, missing_mask)) {
			uint32_t id = create_entry_front(chunk.first, chunk.second, 0, dirty);
			id = merge_buddies_to_largest(id);
			(void)id;
		}
		if (missing_mask != 0) {
			page_line_masks_[lba] = old_mask | missing_mask;
			resident_cachelines_ += static_cast<uint64_t>(__builtin_popcountll(missing_mask));
		}

		if (dirty) {
			page_dirty_masks_[lba] |= range_mask;
		}

		// The requested range is the most recently used/admitted range.  Each
		// resident Segment is represented by one LRU node, so touch it once.
		for (uint64_t i = start; i < start + length;) {
			uint32_t id = find_segment_id(base + i);
			if (id != invalid_entry_id) {
				if (dirty && !entries_[id].dirty) {
					entries_[id].dirty = true;
					refresh_policy_candidates(id);
				}
				// A refill touching only part of an existing/merged Segment must
				// not rejuvenate resident CLs outside the supplied range. Real
				// demand hits still use contains() and refresh the whole Segment.
				if ((segment_page_mask(entries_[id]) & ~range_mask) == 0) {
					move_to_front(id);
				}
				i = entries_[id].base_cl_id - base + segment_length(entries_[id]);
			}
			else ++i;
		}

		// Capacity including variable metadata is enforced by dram_subsystem,
		// after the entire admission has been installed.
		return std::nullopt;
	}

	bool contains(uint64_t key, bool touch = true)
	{
		uint32_t id = find_segment_id(key);
		if (id == invalid_entry_id) {
			return false;
		}
		if (touch) {
			move_to_front(id);
			mark_entry_used(id, 1ULL << (key % lines_per_page));
		}
		return true;
	}

	bool markDirty(uint64_t key) { return setDirty(key, true); }
	bool markClean(uint64_t key) { return setDirty(key, false); }
	std::optional<bool> isDirty(uint64_t key) const
	{
		uint32_t id = find_segment_id(key);
		return id == invalid_entry_id ? std::optional<bool>{} : std::optional<bool>{entries_[id].dirty};
	}

	std::optional<EvictedEntry> popLast()
	{
		auto evicted = pop_tail_segment();
		if (!evicted) {
			return std::nullopt;
		}
		uint64_t first_idx = static_cast<uint64_t>(__builtin_ctzll(evicted->mask));
		return EvictedEntry{evicted->lba * lines_per_page + first_idx, evicted->dirty};
	}

	std::optional<EvictedSegment> pop_tail_segment()
	{
		if (tail_id_ == invalid_entry_id) {
			return std::nullopt;
		}
		lru_fallback_eviction_selection_count_++;
		return pop_entry(tail_id_);
	}

	// No scan, clean-first search, or protection for the retained children.
	std::optional<EvictedSegment> prune_clean_unused_tail()
	{
		if (tail_id_ == invalid_entry_id) return std::nullopt;
		tail_prune_attempts_++;
		const auto& old = entries_[tail_id_];
		if (old.dirty || segment_has_dirty_lines(old)) return std::nullopt;
		uint64_t mask = segment_page_mask(old);
		uint64_t used = old.used_mask & mask;
		uint64_t unused = mask & ~used;
		if (unused == 0) return std::nullopt;
		if (managed_entry_count_for_mask(used) > 2) {
			tail_prune_fragmentation_rejects_++;
			return std::nullopt;
		}
		int64_t net = 64 * static_cast<int64_t>(__builtin_popcountll(unused))
			+ static_cast<int64_t>(entry_logical_bytes(old.order))
			- static_cast<int64_t>(metadata_bytes_for_mask(used));
		if (net <= 0) {
			tail_prune_nonpositive_rejects_++;
			return std::nullopt;
		}
		auto result = prune_unused_from_entry(tail_id_);
		if (result) {
			unused_reclamation_count_++;
			tail_prune_reclaimed_cl_ += __builtin_popcountll(result->mask);
			tail_prune_net_bytes_ += net;
		}
		return result;
	}

	uint64_t tail_prune_attempts() const { return tail_prune_attempts_; }
	uint64_t tail_prune_fragmentation_rejects() const { return tail_prune_fragmentation_rejects_; }
	uint64_t tail_prune_nonpositive_rejects() const { return tail_prune_nonpositive_rejects_; }
	uint64_t tail_prune_reclaimed_cl() const { return tail_prune_reclaimed_cl_; }
	uint64_t tail_prune_net_bytes() const { return tail_prune_net_bytes_; }

	std::optional<EvictedSegment> pop_segment_containing_key(uint64_t key)
	{
		uint32_t id = find_segment_id(key);
		if (id == invalid_entry_id) {
			id = tail_id_;
		}
		return id == invalid_entry_id ? std::optional<EvictedSegment>{} : pop_entry(id);
	}

	// Fixed oldest-5% window: reclaim UNUSED CLs only, including those in
	// conservatively dirty Segments. Never evict used CLs just because a
	// non-tail Segment is clean. Partial victims write back only their removed
	// mask; only the caller's whole-tail fallback may clean dirty page siblings.
	// Retained children keep their position/use/dirty state, without protection.
	std::vector<EvictedSegment> prune_unused_tail_fraction(
		const std::function<bool()>& capacity_satisfied)
	{
		std::vector<EvictedSegment> reclaimed;
		// Replacement ablation: zero disables scanning/splitting and counters.
		// Keep the optimized indexes and metadata model otherwise unchanged.
		if (tail_unused_scan_percent_ == 0) return reclaimed;
		if (tail_id_ == invalid_entry_id || active_entry_count_ == 0
			|| (capacity_satisfied && capacity_satisfied())) {
			return reclaimed;
		}
		clean_first_scan_count_++;
		uint64_t scan_limit = (active_entry_count_ * tail_unused_scan_percent_ + 99) / 100;
		scan_limit = std::max<uint64_t>(1, scan_limit);
		auto boundary_it = lru_order_.find_by_order(scan_limit - 1);
		if (boundary_it == lru_order_.end()) {
			clean_first_scanned_entry_total_ += scan_limit;
			return reclaimed;
		}
		const auto& last = entries_[std::get<2>(*boundary_it)];
		// Include all children if the last original entry is split in this pass.
		const OrderKey boundary{last.lru_epoch,
			last.lru_subrank + static_cast<uint32_t>(segment_length(last)) - 1,
			invalid_entry_id};
		uint64_t logically_scanned = 0;
		bool capacity_met = false;
		while (!unused_candidate_order_.empty()) {
			auto candidate_it = unused_candidate_order_.begin();
			const OrderKey candidate = *candidate_it;
			if (candidate > boundary) {
				break;
			}
			// order_of_key gives the logical number of older entries skipped.
			// The simulator charges this scan distance, while the implementation
			// jumps directly to the indexed unused candidate.
			logically_scanned = std::max<uint64_t>(logically_scanned,
				std::min<uint64_t>(scan_limit, lru_order_.order_of_key(candidate) + 1));
			uint32_t id = std::get<2>(candidate);
			if (id >= entries_.size() || !entries_[id].valid) {
				unused_candidate_order_.erase(candidate_it);
				continue;
			}
			auto evicted = prune_unused_from_entry(id);
			if (!evicted || evicted->mask == 0) {
				refresh_policy_candidates(id);
				continue;
			}
			if (!evicted->dirty) clean_segment_eviction_selection_count_++;
			unused_reclamation_count_++;
			reclaimed.push_back(*evicted);
			if (capacity_satisfied && capacity_satisfied()) {
				capacity_met = true;
				break;
			}
		}
		clean_first_scanned_entry_total_ += capacity_met ? logically_scanned : scan_limit;
		return reclaimed;
	}

	std::size_t size() const { return resident_cachelines_; }
	bool empty() const { return active_entry_count_ == 0; }

	uint64_t invariant_error_count() const
	{
		uint64_t errors = 0;
		uint64_t valid_count = 0;
		uint64_t expected_index_bytes = 0;
		uint64_t expected_cachelines = 0;
		std::unordered_map<uint64_t, uint64_t> rebuilt_page_masks;
		for (uint32_t id = 0; id < entries_.size(); id++) {
			const SegmentEntry& entry = entries_[id];
			if (!entry.valid) continue;
			valid_count++;
			expected_index_bytes += entry_logical_bytes(entry.order);
			expected_cachelines += segment_length(entry);
			auto table_it = segment_tables_[entry.order].find(entry.base_cl_id);
			if (table_it == segment_tables_[entry.order].end() || table_it->second != id) errors++;
			uint64_t lba = entry.base_cl_id / lines_per_page;
			uint64_t mask = segment_page_mask(entry);
			if ((rebuilt_page_masks[lba] & mask) != 0) errors++;
			rebuilt_page_masks[lba] |= mask;
			if ((entry.used_mask & ~mask) != 0) errors++;
		}
		uint64_t table_entries = 0;
		for (const auto& table : segment_tables_) table_entries += table.size();
		if (valid_count != active_entry_count_ || table_entries != active_entry_count_) errors++;
		if (lru_order_.size() != active_entry_count_) errors++;
		for (const auto& candidate : unused_candidate_order_) {
			uint32_t id = std::get<2>(candidate);
			if (id >= entries_.size() || !entries_[id].valid
				|| candidate != order_key(id) || !has_unused_cachelines(entries_[id])) errors++;
		}
		for (const auto& candidate : clean_candidate_order_) {
			uint32_t id = std::get<2>(candidate);
			if (id >= entries_.size() || !entries_[id].valid
				|| candidate != order_key(id) || entries_[id].dirty) errors++;
		}
		for (uint32_t id = 0; id < entries_.size(); id++) {
			if (!entries_[id].valid) continue;
			bool indexed_unused = unused_candidate_order_.count(order_key(id)) != 0;
			if (indexed_unused != has_unused_cachelines(entries_[id])) errors++;
			if ((clean_candidate_order_.count(order_key(id)) != 0) != !entries_[id].dirty) errors++;
			if (!entries_[id].dirty && segment_has_dirty_lines(entries_[id])) errors++;
		}
		if (expected_index_bytes != index_bytes_ || expected_cachelines != resident_cachelines_) errors++;
		if (rebuilt_page_masks.size() != page_line_masks_.size()) errors++;
		for (const auto& page : rebuilt_page_masks) {
			auto it = page_line_masks_.find(page.first);
			if (it == page_line_masks_.end() || it->second != page.second) errors++;
		}

		std::vector<bool> seen(entries_.size(), false);
		uint64_t lru_count = 0;
		uint32_t previous = invalid_entry_id;
		uint32_t cursor = head_id_;
		while (cursor != invalid_entry_id) {
			if (cursor >= entries_.size() || !entries_[cursor].valid || seen[cursor]) {
				errors++;
				break;
			}
			seen[cursor] = true;
			if (entries_[cursor].prev_id != previous) errors++;
			if (previous != invalid_entry_id && !(order_key(cursor) < order_key(previous))) errors++;
			previous = cursor;
			cursor = entries_[cursor].next_id;
			lru_count++;
			if (lru_count > active_entry_count_) {
				errors++;
				break;
			}
		}
		if (previous != tail_id_ || lru_count != active_entry_count_) errors++;
		return errors;
	}

private:
	using OrderKey = std::tuple<uint64_t, uint32_t, uint32_t>;
	using LRUOrderTree = __gnu_pbds::tree<
		OrderKey,
		__gnu_pbds::null_type,
		std::less<OrderKey>,
		__gnu_pbds::rb_tree_tag,
		__gnu_pbds::tree_order_statistics_node_update>;

	struct SegmentEntry {
		uint64_t base_cl_id{0};
		uint64_t used_mask{0};       // page-relative bitmap, logical storage is compact
		uint64_t lru_epoch{0};       // simulator-only acceleration, not charged metadata
		uint32_t lru_subrank{0};     // exact sibling order after splitting, simulator-only
		uint32_t prev_id{invalid_entry_id};
		uint32_t next_id{invalid_entry_id};
		uint8_t order{0};
		bool dirty{false};
		bool valid{false};
	};

	static uint64_t mask_for_range(uint64_t start, uint64_t length)
	{
		if (length == 0 || start >= lines_per_page) {
			return 0;
		}
		length = std::min<uint64_t>(length, lines_per_page - start);
		return length == 64 ? ~0ULL : ((1ULL << length) - 1) << start;
	}

	static uint8_t order_for_length(uint64_t length)
	{
		uint8_t order = 0;
		while ((1ULL << order) < length && order < 7) {
			order++;
		}
		return order;
	}

	static uint64_t entry_logical_bytes(uint8_t order)
	{
		// 12B hash slot + 17B base SegmentEntry + compact extension bitmap.
		uint64_t extension = order < 3 ? 0 : (1ULL << (order - 3));
		return 12 + 17 + extension;
	}

	static uint64_t segment_length(const SegmentEntry& entry)
	{
		return 1ULL << entry.order;
	}

	static uint64_t segment_page_mask(const SegmentEntry& entry)
	{
		return mask_for_range(entry.base_cl_id % lines_per_page, segment_length(entry));
	}

	template <typename Visitor>
	static void for_each_page_chunk(uint64_t lba, uint64_t mask, Visitor visit)
	{
		if (mask == 0) return;
		static constexpr uint64_t alignment[] = {
			~0ULL, 0x5555555555555555ULL, 0x1111111111111111ULL,
			0x0101010101010101ULL, 0x0001000100010001ULL, 0x0000000100000001ULL, 1ULL};
		uint64_t full[7];
		full[0] = mask;
		for (unsigned order = 1; order <= 6; ++order)
			full[order] = full[order - 1] & (full[order - 1] >> (1U << (order - 1)));
		uint64_t remaining = mask;
		for (int order = 6; order >= 0 && remaining != 0; order--) {
			uint64_t length = 1ULL << order;
			uint64_t starts = full[order] & alignment[order] & remaining;
			while (starts != 0) {
				uint64_t start = static_cast<uint64_t>(__builtin_ctzll(starts));
				visit(lba * lines_per_page + start, static_cast<uint8_t>(order));
				remaining &= ~mask_for_range(start, length);
				starts &= starts - 1;
			}
		}
	}

	static std::vector<std::pair<uint64_t, uint8_t>> decompose_page_mask(uint64_t lba, uint64_t mask)
	{
		std::vector<std::pair<uint64_t, uint8_t>> chunks;
		for_each_page_chunk(lba, mask, [&](uint64_t base, uint8_t order) {
			chunks.emplace_back(base, order);
		});
		return chunks;
	}

	static uint64_t managed_entry_count_for_mask(uint64_t mask)
	{
		// Count each aligned buddy merge as one fewer entry.  The four 16CL
		// lookups cover all within-quarter merges; only 32/64CL merges remain.
		struct Counts16 {
			std::array<uint8_t, 65536> counts{};
			Counts16() {
				for (uint32_t m = 1; m < 65536; ++m) {
					unsigned count = __builtin_popcount(m);
					for (unsigned length = 2; length <= 16; length *= 2) {
						for (unsigned start = 0; start < 16; start += length) {
							uint32_t chunk = ((1U << length) - 1) << start;
							if ((m & chunk) == chunk) --count;
						}
					}
					counts[m] = static_cast<uint8_t>(count);
				}
			}
		};
		static const Counts16 table;
		return static_cast<uint64_t>(table.counts[mask & 0xffff])
			+ table.counts[(mask >> 16) & 0xffff] + table.counts[(mask >> 32) & 0xffff]
			+ table.counts[mask >> 48] - ((mask & 0xffffffffULL) == 0xffffffffULL)
			- ((mask >> 32) == 0xffffffffULL) - (mask == ~0ULL);
	}

	uint64_t metadata_bytes_for_mask(uint64_t mask) const
	{
		uint64_t bytes = 0;
		for_each_page_chunk(0, mask, [&](uint64_t, uint8_t order) { bytes += entry_logical_bytes(order); });
		return bytes;
	}

	uint32_t find_segment_id(uint64_t key) const
	{
		for (uint8_t order = 0; order <= 6; order++) {
			uint64_t length = 1ULL << order;
			uint64_t base = key & ~(length - 1);
			auto it = segment_tables_[order].find(base);
			if (it != segment_tables_[order].end()) {
				return it->second;
			}
		}
		return invalid_entry_id;
	}

	uint32_t allocate_entry()
	{
		if (!free_entry_ids_.empty()) {
			uint32_t id = free_entry_ids_.back();
			free_entry_ids_.pop_back();
			entries_[id] = SegmentEntry{};
			return id;
		}
		entries_.push_back(SegmentEntry{});
		return static_cast<uint32_t>(entries_.size() - 1);
	}

	bool has_unused_cachelines(const SegmentEntry& entry) const
	{
		return (segment_page_mask(entry) & ~entry.used_mask) != 0;
	}

	OrderKey order_key(uint32_t id) const
	{
		return {entries_[id].lru_epoch, entries_[id].lru_subrank, id};
	}

	void register_order_indexes(uint32_t id)
	{
		lru_order_.insert(order_key(id));
		if (!entries_[id].dirty) clean_candidate_order_.insert(order_key(id));
		if (has_unused_cachelines(entries_[id])) {
			unused_candidate_order_.insert(order_key(id));
		}
	}

	void unregister_order_indexes(uint32_t id)
	{
		lru_order_.erase(order_key(id));
		unused_candidate_order_.erase(order_key(id));
		clean_candidate_order_.erase(order_key(id));
	}

	void refresh_policy_candidates(uint32_t id)
	{
		if (id == invalid_entry_id || id >= entries_.size() || !entries_[id].valid) return;
		unused_candidate_order_.erase(order_key(id));
		clean_candidate_order_.erase(order_key(id));
		if (!entries_[id].dirty) clean_candidate_order_.insert(order_key(id));
		if (has_unused_cachelines(entries_[id])) {
			unused_candidate_order_.insert(order_key(id));
		}
	}

	void mark_entry_used(uint32_t id, uint64_t bits)
	{
		auto& entry = entries_[id];
		if ((bits & ~entry.used_mask) == 0) return;
		entry.used_mask |= bits;
		page_used_masks_[entry.base_cl_id / lines_per_page] |= bits;
		// Setting use bits can only remove a candidate, never add one.
		if (!has_unused_cachelines(entry)) unused_candidate_order_.erase(order_key(id));
	}

	void link_front(uint32_t id)
	{
		entries_[id].prev_id = invalid_entry_id;
		entries_[id].next_id = head_id_;
		if (head_id_ != invalid_entry_id) {
			entries_[head_id_].prev_id = id;
		}
		else {
			tail_id_ = id;
		}
		head_id_ = id;
	}

	void unlink(uint32_t id)
	{
		uint32_t prev = entries_[id].prev_id;
		uint32_t next = entries_[id].next_id;
		if (prev == invalid_entry_id) head_id_ = next;
		else entries_[prev].next_id = next;
		if (next == invalid_entry_id) tail_id_ = prev;
		else entries_[next].prev_id = prev;
		entries_[id].prev_id = entries_[id].next_id = invalid_entry_id;
	}

	void move_to_front(uint32_t id)
	{
		if (id == invalid_entry_id || id == head_id_) return;
		unregister_order_indexes(id);
		unlink(id);
		entries_[id].lru_epoch = ++lru_epoch_counter_;
		entries_[id].lru_subrank = 0;
		link_front(id);
		register_order_indexes(id);
	}

	uint32_t create_entry_front(uint64_t base, uint8_t order, uint64_t used_mask, bool dirty)
	{
		uint32_t id = allocate_entry();
		entries_[id].base_cl_id = base;
		entries_[id].used_mask = used_mask & mask_for_range(base % lines_per_page, 1ULL << order);
		entries_[id].order = order;
		entries_[id].dirty = dirty;
		entries_[id].valid = true;
		entries_[id].lru_epoch = ++lru_epoch_counter_;
		segment_tables_[order][base] = id;
		index_bytes_ += entry_logical_bytes(order);
		active_entry_count_++;
		link_front(id);
		register_order_indexes(id);
		return id;
	}

	uint32_t create_entry_unlinked(
		uint64_t base, uint8_t order, uint64_t used_mask, bool dirty, uint64_t lru_epoch,
		uint32_t lru_subrank)
	{
		uint32_t id = allocate_entry();
		entries_[id].base_cl_id = base;
		entries_[id].used_mask = used_mask & mask_for_range(base % lines_per_page, 1ULL << order);
		entries_[id].order = order;
		entries_[id].dirty = dirty;
		entries_[id].valid = true;
		entries_[id].lru_epoch = lru_epoch;
		entries_[id].lru_subrank = lru_subrank;
		segment_tables_[order][base] = id;
		index_bytes_ += entry_logical_bytes(order);
		active_entry_count_++;
		return id;
	}

	void erase_entry(uint32_t id)
	{
		SegmentEntry& entry = entries_[id];
		unregister_order_indexes(id);
		segment_tables_[entry.order].erase(entry.base_cl_id);
		index_bytes_ -= entry_logical_bytes(entry.order);
		active_entry_count_--;
		unlink(id);
		entry.valid = false;
		free_entry_ids_.push_back(id);
	}

	uint32_t merge_buddies_to_largest(uint32_t id)
	{
		while (id != invalid_entry_id && entries_[id].valid && entries_[id].order < 6) {
			SegmentEntry current = entries_[id];
			uint64_t length = 1ULL << current.order;
			uint64_t buddy_base = current.base_cl_id ^ length;
			if (buddy_base / lines_per_page != current.base_cl_id / lines_per_page) break;
			auto it = segment_tables_[current.order].find(buddy_base);
			if (it == segment_tables_[current.order].end()) break;
			uint32_t buddy_id = it->second;
			uint64_t used = current.used_mask | entries_[buddy_id].used_mask;
			bool dirty = current.dirty || entries_[buddy_id].dirty;
			uint64_t parent_base = std::min(current.base_cl_id, buddy_base);
			// Keep the older operand's list position. Delete the younger first
			// so the saved neighbors remain valid even for adjacent buddies.
			uint32_t older = order_key(id) < order_key(buddy_id) ? id : buddy_id;
			uint32_t younger = older == id ? buddy_id : id;
			erase_entry(younger);
			SegmentEntry age = entries_[older];
			erase_entry(older);
			id = create_entry_unlinked(parent_base, current.order + 1, used, dirty,
				age.lru_epoch, age.lru_subrank);
			entries_[id].prev_id = age.prev_id;
			entries_[id].next_id = age.next_id;
			if (age.prev_id == invalid_entry_id) head_id_ = id;
			else entries_[age.prev_id].next_id = id;
			if (age.next_id == invalid_entry_id) tail_id_ = id;
			else entries_[age.next_id].prev_id = id;
			register_order_indexes(id);
			normalize_equal_epoch_ranks(id);
		}
		return id;
	}

	// Simulator-only ordering labels: after conservative merges an old epoch
	// may contain several siblings. Keep room for later splits, without changing
	// any linked-list age or charging extra hardware metadata.
	void normalize_equal_epoch_ranks(uint32_t id)
	{
		uint64_t epoch = entries_[id].lru_epoch;
		uint32_t first = id;
		while (entries_[first].prev_id != invalid_entry_id &&
			entries_[entries_[first].prev_id].lru_epoch == epoch)
			first = entries_[first].prev_id;
		std::vector<uint32_t> group;
		for (uint32_t p = first; p != invalid_entry_id && entries_[p].lru_epoch == epoch;
			p = entries_[p].next_id) group.push_back(p);
		if (group.size() == 1) return;
		for (uint32_t p : group) unregister_order_indexes(p);
		uint32_t rank = static_cast<uint32_t>(group.size()) * 64;
		for (uint32_t p : group) {
			entries_[p].lru_subrank = rank;
			rank -= 64;
			register_order_indexes(p);
		}
	}

	void replace_entry_at_same_lru_position(uint32_t id, uint64_t retained_mask, bool inherit_dirty)
	{
		SegmentEntry old = entries_[id];
		uint64_t lba = old.base_cl_id / lines_per_page;
		uint32_t prev = old.prev_id;
		uint32_t next = old.next_id;
		erase_entry(id);

		auto chunks = decompose_page_mask(lba, retained_mask);
		std::vector<uint32_t> children;
		children.reserve(chunks.size());
		uint32_t rank = old.lru_subrank + static_cast<uint32_t>(__builtin_popcountll(retained_mask));
		for (const auto& chunk : chunks) {
			rank -= 1U << chunk.second;
			children.push_back(create_entry_unlinked(
				chunk.first, chunk.second, old.used_mask, inherit_dirty, old.lru_epoch, rank));
		}

		if (children.empty()) {
			if (prev == invalid_entry_id) head_id_ = next;
			else entries_[prev].next_id = next;
			if (next == invalid_entry_id) tail_id_ = prev;
			else entries_[next].prev_id = prev;
			return;
		}

		uint32_t first = children.front();
		uint32_t last = children.back();
		if (prev == invalid_entry_id) head_id_ = first;
		else entries_[prev].next_id = first;
		entries_[first].prev_id = prev;
		for (std::size_t i = 1; i < children.size(); i++) {
			entries_[children[i - 1]].next_id = children[i];
			entries_[children[i]].prev_id = children[i - 1];
		}
		entries_[last].next_id = next;
		if (next == invalid_entry_id) tail_id_ = last;
		else entries_[next].prev_id = last;
		for (uint32_t child : children) {
			register_order_indexes(child);
		}
		normalize_equal_epoch_ranks(first);
	}

	void remove_page_bits(uint64_t lba, uint64_t removed)
	{
		uint64_t old = resident_mask_for_page(lba);
		uint64_t now = old & ~removed;
		resident_cachelines_ -= static_cast<uint64_t>(__builtin_popcountll(old & removed));
		if (now == 0) page_line_masks_.erase(lba);
		else page_line_masks_[lba] = now;

		auto used_it = page_used_masks_.find(lba);
		if (used_it != page_used_masks_.end()) {
			used_it->second &= now;
			if (used_it->second == 0) page_used_masks_.erase(used_it);
		}
		auto dirty_it = page_dirty_masks_.find(lba);
		if (dirty_it != page_dirty_masks_.end()) {
			dirty_it->second &= now;
			if (dirty_it->second == 0) page_dirty_masks_.erase(dirty_it);
		}
	}

	std::optional<EvictedSegment> prune_unused_from_entry(uint32_t id)
	{
		SegmentEntry old = entries_[id];
		uint64_t lba = old.base_cl_id / lines_per_page;
		uint64_t entry_mask = segment_page_mask(old);
		uint64_t used = old.used_mask & entry_mask;
		uint64_t unused = entry_mask & ~used;
		if (unused == 0) return std::nullopt;

		uint64_t removed_dirty = dirty_mask_for_page(lba) & unused;
		uint64_t old_meta = entry_logical_bytes(old.order);
		replace_entry_at_same_lru_position(id, used, old.dirty);
		uint64_t new_meta = metadata_bytes_for_mask(used);
		remove_page_bits(lba, unused);
		// Preserve conservative dirty state on children and write back only
		// the removed mask if it might contain dirty data.
		return EvictedSegment{lba, unused, 0, old.dirty || removed_dirty != 0,
			static_cast<int64_t>(old_meta) - static_cast<int64_t>(new_meta)};
	}

	std::optional<EvictedSegment> pop_entry(uint32_t id)
	{
		if (id == invalid_entry_id || id >= entries_.size() || !entries_[id].valid) {
			return std::nullopt;
		}
		SegmentEntry old = entries_[id];
		uint64_t lba = old.base_cl_id / lines_per_page;
		uint64_t mask = segment_page_mask(old);
		uint64_t used = old.used_mask & mask;
		bool dirty = old.dirty || ((dirty_mask_for_page(lba) & mask) != 0);
		uint64_t meta = entry_logical_bytes(old.order);
		erase_entry(id);
		remove_page_bits(lba, mask);
		return EvictedSegment{lba, mask, used, dirty, static_cast<int64_t>(meta)};
	}

	void setDirtyMaskBit(uint64_t key, bool dirty)
	{
		uint64_t lba = key / lines_per_page;
		uint64_t bit = 1ULL << (key % lines_per_page);
		if (dirty) {
			page_dirty_masks_[lba] |= bit;
			return;
		}
		auto it = page_dirty_masks_.find(lba);
		if (it == page_dirty_masks_.end()) return;
		it->second &= ~bit;
		if (it->second == 0) page_dirty_masks_.erase(it);
	}

	bool segment_has_dirty_lines(const SegmentEntry& entry) const
	{
		uint64_t lba = entry.base_cl_id / lines_per_page;
		return (dirty_mask_for_page(lba) & segment_page_mask(entry)) != 0;
	}

	std::size_t capacity_;
	std::array<std::unordered_map<uint64_t, uint32_t>, 7> segment_tables_;
	std::vector<SegmentEntry> entries_;
	std::vector<uint32_t> free_entry_ids_;
	LRUOrderTree lru_order_;
	// Simulator acceleration only: model still scans the same bounded LRU window.
	std::set<OrderKey> unused_candidate_order_;
	std::set<OrderKey> clean_candidate_order_;
	uint64_t lru_epoch_counter_{0};
	uint32_t head_id_{invalid_entry_id};
	uint32_t tail_id_{invalid_entry_id};
	std::size_t resident_cachelines_{0};
	std::size_t active_entry_count_{0};
	std::unordered_map<uint64_t, uint64_t> page_line_masks_;
	std::unordered_map<uint64_t, uint64_t> page_used_masks_;
	std::unordered_map<uint64_t, uint64_t> page_dirty_masks_;
	uint64_t admission_metadata_entry_bytes_{cxl_config::default_cache_metadata_entry_bytes};
	uint64_t index_bytes_{0};
	static constexpr uint64_t tail_unused_scan_percent_{0};
	uint64_t clean_first_scan_count_{0};
	uint64_t clean_first_scanned_entry_total_{0};
	uint64_t tail_prune_attempts_{0};
	uint64_t tail_prune_fragmentation_rejects_{0};
	uint64_t tail_prune_nonpositive_rejects_{0};
	uint64_t tail_prune_reclaimed_cl_{0};
	uint64_t tail_prune_net_bytes_{0};
	uint64_t clean_page_eviction_selection_count_{0};
	uint64_t clean_segment_eviction_selection_count_{0};
	uint64_t unused_reclamation_count_{0};
	uint64_t clean_whole_segment_selection_count_{0};
	uint64_t lru_fallback_eviction_selection_count_{0};
};

class lruTwoListClass {
private:
	list<uint64_t> active;
	list<uint64_t> inactive;
	uint64_t activeBound;
public:
	lruTwoListClass() { activeBound = 0; }

	void init(uint64_t numCL);
	void add(uint64_t lba);
	uint64_t getCandidate();
	uint64_t evictLBA();
	void updateWhenHit(uint64_t lba, bool& falsehit);
	void reset();
};

class lfuNode {
public:
	uint64_t addr;
	uint64_t count;

	lfuNode(uint64_t a) { addr = a; count = 0; }
};

class lfuHeap {
public:

	void add(uint64_t addr);
	uint64_t top();
	void pop();
	void update(uint64_t addr);

private:
	vector<lfuNode> h;
	map<uint64_t, uint64_t> m;
};


using namespace std;
namespace SSD_Components {

	class dram_subsystem {
	public:
		dram_subsystem() {};
		dram_subsystem(cxl_config confival);
		~dram_subsystem();

			void initDRAM();
				bool isCacheHit(uint64_t lba);
				bool isCacheHit_sector(uint64_t lba, uint64_t mask);
				uint64_t page_free_slot_count(uint64_t lba);
				uint64_t total_page_slot_count();
				uint64_t total_page_free_slot_count();
				uint64_t current_page_count() const;
				uint64_t page_cache_metadata_bytes(uint64_t additional_pages = 0) const;
				uint64_t cacheline_cache_metadata_bytes() const;
				uint64_t ghost_metadata_bytes() const;
				uint64_t metadata_overhead_bytes(uint64_t additional_pages = 0) const;
				void record_metadata_overhead_sample();
				double average_metadata_overhead_bytes() const;
				uint64_t max_metadata_overhead_bytes() const;
				uint64_t metadata_overhead_sample_count_value() const;
				uint64_t total_data_cacheline_occupancy(uint64_t additional_pages = 0) const;
				uint64_t dynamic_data_cacheline_capacity(uint64_t additional_pages = 0) const;
				uint64_t dynamic_page_cache_capacity(uint64_t additional_pages = 0) const;
				uint64_t managed_cacheline_entry_count(uint64_t length) const;
				std::vector<LRUSet::EvictedSegment> drain_cacheline_evictions();
				bool enforce_dynamic_cache_capacity(list<uint64_t>* flush_lba);
				bool ensure_page_cache_capacity(list<uint64_t>* flush_lba, uint64_t simtime,
					set<uint64_t>* prefetched_lba, set<uint64_t>& taggedAddr,
					set<uint64_t>& prefetch_pollution_tracker, set<uint64_t> not_finished,
					uint64_t trigger_lba, uint64_t pages_to_insert);

	        void resize_cache(uint64_t new_cache_size);


		void process_cache_hit(bool rw, uint64_t lba, bool& falsehit, uint64_t mask);
		//void process_miss_data_ready(bool rw, uint64_t lba, list<uint64_t>* flush_lba, uint64_t simtime, set<uint64_t>* prefetched_lba);
		void process_miss_data_ready_new(bool rw, uint64_t lba, list<uint64_t>* flush_lba, uint64_t simtime, set<uint64_t>* prefetched_lba, set<uint64_t>& taggedAddr, set<uint64_t>& prefetch_pollution_tracker, set<uint64_t> not_finished, uint64_t bitmap);

		bool is_next_evict_candidate(uint64_t lba);

		uint64_t get_cache_index(uint64_t lba);
        void evict(bool rw, uint64_t lba, bool& falsehit,uint64_t mask) ;

		uint64_t eviction_count{ 0 }, flush_count{0};

		void set_valid_bits(uint64_t lba, uint64_t mask);
		void set_dirty_bits(uint64_t lba, uint64_t mask);
		uint64_t get_dirty_mask(uint64_t lba);
		void clear_dirty_bits(uint64_t lba, uint64_t mask);
		map<uint64_t, uint64_t>* record_access_dram_dirty_bitmap{ nullptr };
        map<uint64_t, uint64_t>* write_map{ nullptr };
        
		LRUSet* cacheline_dram;
        LRUSet* page_dram;
        cxl_config cpara;

	private:
		//cxl_config cpara;
			map<uint64_t, uint64_t>* dram_mapping{ NULL }; // LBA, cache line index
		map<uint64_t, uint64_t>* dram_valid_bitmap{ nullptr }; // lba -> valid CL mask
        map<uint64_t, uint64_t>* dram_dirty_bitmap{ nullptr }; // lba -> dirty CL mask
        uint64_t cacheline_mask_to_sector_mask(uint64_t cl_mask) {
		uint64_t sec_mask = 0;
		for (int s = 0; s < 8; s++) {
			uint64_t group = (cl_mask >> (s * 8)) & 0xFFULL;
			if (group != 0) {
				sec_mask |= (1ULL << s);
			}
		}
		return sec_mask;
	}
		
		list<uint64_t>* freeCL{ NULL }; // aligned by ssd block size

		vector<list<uint64_t>*>* all_freeCL{ NULL };
		
		set<uint64_t>* cachedlba{ NULL };//for random 
		//vector<set<uint64_t>*>* all_cachedlba{ NULL };
		vector<vector<uint64_t>*>* all_cachedlba{ NULL };

		vector<list<uint64_t>*>* all_fifocachedlba{ NULL };
		vector<CFLRU*>* all_cflrucachedlba{ NULL };
		vector<lfuHeap*>* all_lfucachedlba{NULL};

		lrfuHeap* lrfucachedlba{NULL}; //for lrfu
		vector<lrfuHeap*>* all_lrfucachedlba{ NULL };

		lruTwoListClass* lru2cachedlba{NULL};
		vector<lruTwoListClass*>* all_lru2cachedlba{ NULL };

		map < uint64_t, uint64_t>* dirtyCL{ NULL }; //cache line index, write count
		vector<map < uint64_t, uint64_t>*>* all_dirtyCL{ NULL };

		//For mix seoeration mode

		map<uint64_t, uint64_t>* pref_dram_mapping{ NULL };
		list<uint64_t>* pref_freeCL{ NULL };

		set<uint64_t>* pref_cachedlba{ NULL };//for random 
		lrfuHeap* pref_lrfucachedlba{ NULL }; //for lrfu
		lruTwoListClass* pref_lru2cachedlba{ NULL };


		map<uint64_t, uint64_t>* pref_dirtyCL{ NULL };
		
			uint64_t* next_eviction_candidate{ NULL };
			uint64_t* pref_next_eviction_candidate{ NULL };
			long double metadata_overhead_sample_sum_bytes{ 0 };
			uint64_t metadata_overhead_sample_count{ 0 };
			uint64_t metadata_overhead_max_bytes{ 0 };
			std::vector<LRUSet::EvictedSegment> pending_cacheline_evictions;
			void account_cacheline_eviction(const LRUSet::EvictedSegment& evicted,
				list<uint64_t>* flush_lba, bool coalesce_tail_page = false);
			bool evict_cacheline_tail_for_capacity(list<uint64_t>* flush_lba);
			bool evict_one_page_for_capacity(list<uint64_t>* flush_lba, uint64_t simtime,
				set<uint64_t>* prefetched_lba, set<uint64_t>& taggedAddr,
				set<uint64_t>& prefetch_pollution_tracker, set<uint64_t> not_finished,
				uint64_t trigger_lba);

		};


}


#endif
