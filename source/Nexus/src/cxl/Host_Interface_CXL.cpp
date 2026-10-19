
#include <stdexcept>
#include "../sim/Engine.h"
#include "Host_Interface_CXL.h"
#include "Community_Score_Table.h"

// Segment-cache operations use the indexed LRU implementation in
// DRAM_Subsystem.h; policy decisions in this file remain unchanged.
#include "../ssd/NVM_Transaction_Flash_RD.h"
#include "../ssd/NVM_Transaction_Flash_WR.h"
#include <fstream>
#include "../ssd/Stats.h"
#include <bitset>
#include <FTL.h>
#include <Address_Mapping_Unit_Page_Level.h>
#include <algorithm> 
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <string>

// ofstream ofFlush{ "Flush_initiation_time.txt" };
// ofstream ofrequest{ "Request_recieved.txt" };

ofstream oflatep{};
ofstream oflat_no_cache{};
// ofstream ofprefetch_chance{ "./Results/prefetch_potential.txt" };
ofstream ofrepeated_access;
ofstream of_delta{"Prediction_delta.txt"};

class prefetch_info_node
{
public:
	uint64_t hit_count{0};
	bool late{0};
};

map<uint64_t, prefetch_info_node> PREFETCH_INFO_MAP;
map<uint64_t, prefetch_info_node> PREFETCH_INFO_MAP_SAMPLE;
uint64_t LAST_CACHE_MISS{0}, LAST_POLLUTION{0}, LAST_PREFETCH{0};

set<uint64_t> PREFETCH_ACCURACY_INFO;
set<uint64_t> PREFETCH_LATE_INFO;

namespace
{
	struct AdmissionDecision {
		uint64_t mask;
		uint64_t start;
		uint64_t length;
		int score;
		uint64_t resident_mask{0};
		uint64_t baseline_mask{0};
		uint64_t community_mask{0};
		uint64_t required_mask{0};
		uint64_t required_new_mask{0};
		uint64_t resident_cl_count{0};
		uint64_t resident_run_count{0};
		uint64_t candidate_count{0};
		uint64_t community_logical_size{0};
		bool community_accepted{false};
		double q_best{0};
		uint64_t demand_read_bytes{64};
		uint64_t community_admission_bytes{0};
		uint64_t hole_fill_bytes{0};
		uint64_t merge_attempt_count{0};
		uint64_t merge_success_count{0};
		uint64_t merge_reject_count{0};
		uint64_t merge_total_absorbed_segment_entries{0};
		uint64_t merge_max_absorbed_segment_entries{0};
		uint64_t merge_total_metadata_bytes_saved{0};
		uint64_t merge_total_hole_cls{0};
		uint64_t merge_total_hole_bytes{0};
		int64_t merge_total_gain_bytes{0};
		int64_t merge_max_gain_bytes{0};
		uint64_t merge_to_granularity_count[7]{0};
	};

	uint64_t clamp_granularity_to_candidates(uint64_t granularity, uint64_t lines_per_page)
	{
		const uint64_t candidates[] = {1, 2, 4, 8, 16, 32, 64};
		for (uint64_t candidate : candidates) {
			if (candidate >= granularity && candidate <= lines_per_page) {
				return candidate;
			}
		}
		return lines_per_page;
	}

	uint64_t make_aligned_mask(uint64_t miss_idx, uint64_t length)
	{
		const uint64_t lines_per_page = 64;
		if (length == 0 || miss_idx >= lines_per_page) {
			return 0;
		}
		uint64_t start = (miss_idx / length) * length;
		if (start >= lines_per_page) {
			return 0;
		}
		if (start + length > lines_per_page) {
			length = lines_per_page - start;
		}
		if (length >= lines_per_page && start == 0) {
			return ~0ULL;
		}
		return ((1ULL << length) - 1) << start;
	}

	uint64_t make_range_mask(uint64_t start, uint64_t length, uint64_t lines_per_page)
	{
		if (length == 0 || start >= lines_per_page) {
			return 0;
		}
		if (start + length > lines_per_page) {
			length = lines_per_page - start;
		}
		if (length >= 64 && start == 0) {
			return ~0ULL;
		}
		return ((1ULL << length) - 1) << start;
	}

	uint64_t make_min_aligned_cover_mask(
		uint64_t seed_idx,
		uint64_t required_mask,
		uint64_t min_granularity,
		uint64_t lines_per_page)
	{
		if (seed_idx >= lines_per_page || required_mask == 0) {
			return 0;
		}

		min_granularity = clamp_granularity_to_candidates(min_granularity, lines_per_page);
		const uint64_t candidates[] = {1, 2, 4, 8, 16, 32, 64};
		for (uint64_t length : candidates) {
			if (length < min_granularity || length > lines_per_page) {
				continue;
			}

			uint64_t mask = make_aligned_mask(seed_idx, length);
			if ((required_mask & ~mask) == 0) {
				return mask;
			}
		}

		return make_range_mask(0, lines_per_page, lines_per_page);
	}

	uint64_t lowest_set_bit_index(uint64_t mask)
	{
		return mask == 0 ? 0 : static_cast<uint64_t>(__builtin_ctzll(mask));
	}

	uint64_t highest_set_bit_index(uint64_t mask)
	{
		return mask == 0 ? 0 : 63ULL - static_cast<uint64_t>(__builtin_clzll(mask));
	}

	uint64_t cacheline_count_from_mask(uint64_t mask)
	{
		return static_cast<uint64_t>(__builtin_popcountll(mask));
	}

	int order_from_length(uint64_t length)
	{
		switch (length) {
		case 1: return 0;
		case 2: return 1;
		case 4: return 2;
		case 8: return 3;
		case 16: return 4;
		case 32: return 5;
		case 64: return 6;
		default: return -1;
		}
	}

	double safe_xlogy(uint64_t count, double probability, bool& valid)
	{
		if (count == 0) {
			return 0.0;
		}
		if (probability <= 0.0 || probability > 1.0) {
			valid = false;
			return 0.0;
		}
		return static_cast<double>(count) * std::log(probability);
	}

	uint64_t count_resident_runs(uint64_t mask, uint64_t lines_per_page)
	{
		if (lines_per_page < 64) mask &= (1ULL << lines_per_page) - 1;
		return cacheline_count_from_mask(mask & ~(mask << 1));
	}

	uint64_t query_resident_cacheline_mask(
		SSD_Components::dram_subsystem* dram,
		uint64_t lba,
		uint64_t lines_per_page)
	{
		if (dram->cacheline_dram == nullptr) {
			return 0;
		}
		return dram->cacheline_dram->resident_mask_for_page(lba);
	}

	uint64_t choose_resident_community_mask(
		uint64_t resident_mask,
		uint64_t miss_idx,
		uint64_t lines_per_page,
		double& q_best,
		uint64_t& candidate_count,
		bool& accepted)
	{
		accepted = false;
		q_best = 0.0;
		candidate_count = 0;

		uint64_t miss_mask = 1ULL << miss_idx;
		uint64_t sampled_resident_mask = resident_mask & ~miss_mask;
		uint64_t K = cacheline_count_from_mask(sampled_resident_mask);
		const uint64_t N = lines_per_page - 1;
		if (K == 0 || N == 0) {
			return 0;
		}

		double p = static_cast<double>(K) / static_cast<double>(N);
		bool valid_l0 = true;
		double l0 = safe_xlogy(K, p, valid_l0)
			+ safe_xlogy(N - K, 1.0 - p, valid_l0);
		if (!valid_l0) {
			return 0;
		}

		uint64_t best_mask = 0;
		double best_q = -std::numeric_limits<double>::infinity();
		const double* scores = lines_per_page == 64 ? CommunityScoreTable64::for_total(K) : nullptr;
		std::array<uint8_t, 65> prefix{};
		for (uint64_t i = 0; i < lines_per_page; ++i) {
			prefix[i + 1] = prefix[i] + ((sampled_resident_mask >> i) & 1ULL);
		}

		for (uint64_t l = 0; l <= miss_idx; l++) {
			for (uint64_t r = miss_idx; r < lines_per_page; r++) {
				uint64_t g = r - l + 1;
				if (g <= 1) {
					continue;
				}
				uint64_t n = g - 1;
				if (n == 0 || n >= N) {
					continue;
				}
				uint64_t candidate_mask = make_range_mask(l, g, lines_per_page);
				uint64_t k = prefix[r + 1] - prefix[l];
				if (k == 0) {
					continue;
				}
				if (scores != nullptr) {
					candidate_count++;
					double q = scores[n * 64 + k];
					if (q > best_q) {
						best_q = q;
						best_mask = candidate_mask;
					}
					continue;
				}
				uint64_t outside_n = N - n;
				uint64_t outside_k = K - k;
				double p_in = static_cast<double>(k) / static_cast<double>(n);
				double p_out = static_cast<double>(outside_k) / static_cast<double>(outside_n);
				candidate_count++;
				if (!(p_in > p_out)) {
					continue;
				}

				bool valid_l1 = true;
				double l1 = safe_xlogy(k, p_in, valid_l1)
					+ safe_xlogy(n - k, 1.0 - p_in, valid_l1)
					+ safe_xlogy(outside_k, p_out, valid_l1)
					+ safe_xlogy(outside_n - outside_k, 1.0 - p_out, valid_l1);
				if (!valid_l1) {
					continue;
				}

				double q = 2.0 * (l1 - l0) - std::log(static_cast<double>(N));
				if (q > best_q) {
					best_q = q;
					best_mask = candidate_mask;
				}
			}
		}

		if (best_mask != 0 && best_q > 0.0) {
			accepted = true;
			q_best = best_q;
			return best_mask;
		}

		q_best = best_q > 0.0 ? best_q : 0.0;
		return 0;
	}

	void apply_metadata_aware_segment_merge(
		SSD_Components::dram_subsystem* dram,
		uint64_t& post_resident_mask,
		uint64_t focus_mask,
		uint64_t lines_per_page,
		AdmissionDecision& decision)
	{
		if (dram->cacheline_dram == nullptr || post_resident_mask == 0 || focus_mask == 0) {
			return;
		}

		const uint64_t meta_bytes = dram->cacheline_dram->modeled_entry_metadata_bytes();
		const uint64_t parent_lengths[] = {2, 4, 8, 16, 32, 64};

		while (true) {
			uint64_t best_parent_mask = 0;
			uint64_t best_length = 0;
			uint64_t best_absorbed = 0;
			uint64_t best_hole_cls = 0;
			uint64_t best_meta_save = 0;
			int64_t best_gain = 0;

			for (uint64_t length : parent_lengths) {
				if (length > lines_per_page) {
					continue;
				}
				for (uint64_t start = 0; start < lines_per_page; start += length) {
					uint64_t parent_mask = make_range_mask(start, length, lines_per_page);
					if ((parent_mask & focus_mask) == 0) {
						continue;
					}
					uint64_t resident_inside = post_resident_mask & parent_mask;
					uint64_t absorbed_entries = dram->cacheline_dram->modeled_segment_entry_count_for_mask(resident_inside);
					if (absorbed_entries < 2) {
						continue;
					}

					uint64_t hole_mask = parent_mask & ~post_resident_mask;
					uint64_t hole_cls = cacheline_count_from_mask(hole_mask);
					uint64_t hole_bytes = hole_cls * 64;
					uint64_t meta_save = (absorbed_entries - 1) * meta_bytes;
					int64_t gain = static_cast<int64_t>(meta_save) - static_cast<int64_t>(hole_bytes);
					decision.merge_attempt_count++;
					if (gain <= 0) {
						decision.merge_reject_count++;
						continue;
					}
					if (gain > best_gain) {
						best_gain = gain;
						best_parent_mask = parent_mask;
						best_length = length;
						best_absorbed = absorbed_entries;
						best_hole_cls = hole_cls;
						best_meta_save = meta_save;
					}
				}
			}

			if (best_gain <= 0 || best_parent_mask == 0) {
				break;
			}

			uint64_t before_entries = dram->cacheline_dram->modeled_segment_entry_count_for_mask(post_resident_mask);
			uint64_t before_used = cacheline_count_from_mask(post_resident_mask) * 64
				+ before_entries * meta_bytes;
			uint64_t after_mask = post_resident_mask | best_parent_mask;
			uint64_t after_entries = dram->cacheline_dram->modeled_segment_entry_count_for_mask(after_mask);
			uint64_t after_used = cacheline_count_from_mask(after_mask) * 64
				+ after_entries * meta_bytes;
			if (after_used >= before_used) {
				break;
			}

			post_resident_mask = after_mask;
			focus_mask |= best_parent_mask;
			decision.merge_success_count++;
			decision.merge_total_absorbed_segment_entries += best_absorbed;
			if (best_absorbed > decision.merge_max_absorbed_segment_entries) {
				decision.merge_max_absorbed_segment_entries = best_absorbed;
			}
			decision.merge_total_metadata_bytes_saved += best_meta_save;
			decision.merge_total_hole_cls += best_hole_cls;
			decision.merge_total_hole_bytes += best_hole_cls * 64;
			decision.merge_total_gain_bytes += best_gain;
			if (best_gain > decision.merge_max_gain_bytes) {
				decision.merge_max_gain_bytes = best_gain;
			}
			int order = order_from_length(best_length);
			if (order >= 0) {
				decision.merge_to_granularity_count[order]++;
			}
		}
	}

	AdmissionDecision choose_admission_segment(
		SSD_Components::dram_subsystem* dram,
		uint64_t lba,
		uint64_t miss_idx,
		uint64_t lines_per_page,
		uint64_t history_mask,
		bool has_history,
		uint64_t min_load_granularity)
	{
		min_load_granularity = clamp_granularity_to_candidates(min_load_granularity, lines_per_page);
		uint64_t resident_mask = query_resident_cacheline_mask(dram, lba, lines_per_page);
		uint64_t evidence_mask = resident_mask | (has_history ? history_mask : 0);
		uint64_t miss_mask = 1ULL << miss_idx;
		uint64_t baseline_mask = make_aligned_mask(miss_idx, min_load_granularity);

		AdmissionDecision decision{baseline_mask,
			lowest_set_bit_index(baseline_mask),
			highest_set_bit_index(baseline_mask) - lowest_set_bit_index(baseline_mask) + 1,
			0};
		decision.resident_mask = resident_mask;
		decision.baseline_mask = baseline_mask;
		decision.resident_cl_count = cacheline_count_from_mask(resident_mask & ~miss_mask);
		decision.resident_run_count = count_resident_runs(resident_mask & ~miss_mask, lines_per_page);

		uint64_t community_mask = 0;
		if ((evidence_mask & ~miss_mask) == 0) {
			decision.community_accepted = false;
		}
		else {
			community_mask = choose_resident_community_mask(
				evidence_mask,
				miss_idx,
				lines_per_page,
				decision.q_best,
				decision.candidate_count,
				decision.community_accepted);
		}

		if (!decision.community_accepted || community_mask == 0) {
			community_mask = 0;
		}
		decision.community_mask = community_mask;
		// Community evidence is the primary admission signal.  When a reliable
		// resident/history community exists, select the smallest aligned segment
		// that covers that community and the miss CL; the coverage-derived
		// dynamic minimum must not inflate it.  With no reliable community,
		// retain the dynamic minimum as the conservative fallback.
		uint64_t selection_floor = min_load_granularity;
		uint64_t raw_required_mask = baseline_mask | miss_mask;
		if (decision.community_accepted && community_mask != 0) {
			selection_floor = 1;
			raw_required_mask = community_mask | miss_mask;
		}
		decision.required_mask = make_min_aligned_cover_mask(
			miss_idx,
			raw_required_mask,
			selection_floor,
			lines_per_page);
		if (decision.required_mask == 0) {
			decision.required_mask = baseline_mask | miss_mask;
		}
		decision.required_new_mask = decision.required_mask & ~resident_mask;
		decision.community_logical_size = decision.community_accepted && community_mask != 0
			? highest_set_bit_index(community_mask) - lowest_set_bit_index(community_mask) + 1
			: 0;
		decision.community_admission_bytes = cacheline_count_from_mask(decision.required_new_mask) * 64;

		uint64_t post_resident_mask = resident_mask | decision.required_new_mask;
		apply_metadata_aware_segment_merge(
			dram,
			post_resident_mask,
			decision.required_new_mask == 0 ? miss_mask : decision.required_new_mask,
			lines_per_page,
			decision);

		decision.mask = post_resident_mask & ~resident_mask;
		if (decision.mask == 0) {
			decision.mask = miss_mask;
		}
		decision.hole_fill_bytes = decision.merge_total_hole_bytes;
		decision.start = lowest_set_bit_index(decision.mask);
		decision.length = highest_set_bit_index(decision.mask) - decision.start + 1;
		decision.score = static_cast<int>(decision.resident_cl_count);
		return decision;
	}

	uint64_t count_pending_page_fills_for_set(
		SSD_Components::dram_subsystem* dram,
		const std::set<uint64_t>& pending_page_fill,
		uint64_t lba)
	{
		uint64_t cache_index = dram->get_cache_index(lba);
		uint64_t count = 0;
		for (uint64_t pending_lba : pending_page_fill) {
			if (dram->get_cache_index(pending_lba) == cache_index) {
				count++;
			}
		}
		return count;
	}

	uint64_t resident_cachelines_absorbed_by_page_fills(
		SSD_Components::dram_subsystem* dram,
		const std::set<uint64_t>& pending_page_fill,
		uint64_t lba,
		uint64_t lines_per_page)
	{
		uint64_t absorbed = 0;
		for (uint64_t pending_lba : pending_page_fill) {
			absorbed += static_cast<uint64_t>(
				__builtin_popcountll(query_resident_cacheline_mask(dram, pending_lba, lines_per_page)));
		}
		if (pending_page_fill.count(lba) == 0) {
			absorbed += static_cast<uint64_t>(
				__builtin_popcountll(query_resident_cacheline_mask(dram, lba, lines_per_page)));
		}
		return absorbed;
	}

		bool cache_initialization_has_global_room_for_page(
			SSD_Components::dram_subsystem* dram,
			const std::set<uint64_t>& pending_page_fill,
			uint64_t lba,
			uint64_t lines_per_page)
		{
			uint64_t used_page_slots = dram->current_page_count();
			uint64_t pending_page_slots = pending_page_fill.size() + (pending_page_fill.count(lba) == 0 ? 1 : 0);
			uint64_t absorbed_cachelines = resident_cachelines_absorbed_by_page_fills(dram, pending_page_fill, lba, lines_per_page);
			uint64_t cacheline_occupancy_after_fills = dram->cacheline_dram->size() > absorbed_cachelines
				? dram->cacheline_dram->size() - absorbed_cachelines
				: 0;
			uint64_t projected_occupancy = (used_page_slots + pending_page_slots) * lines_per_page
				+ cacheline_occupancy_after_fills;

			return projected_occupancy <= dram->dynamic_data_cacheline_capacity(pending_page_slots);
		}

	bool cache_initialization_has_page_slot_for_request(
		SSD_Components::dram_subsystem* dram,
		const std::set<uint64_t>& pending_page_fill,
		uint64_t lba)
	{
		uint64_t pending_in_set = count_pending_page_fills_for_set(dram, pending_page_fill, lba);
		return pending_in_set < dram->page_free_slot_count(lba);
	}

	uint64_t count_cacheline_fill_misses(
		SSD_Components::dram_subsystem* dram,
		uint64_t lba,
		uint64_t mask,
		uint64_t lines_per_page)
	{
		if (lines_per_page < 64) mask &= (1ULL << lines_per_page) - 1;
		return cacheline_count_from_mask(mask & ~dram->cacheline_dram->resident_mask_for_page(lba));
	}

	bool cacheline_initialization_has_room_for_full_page(
		SSD_Components::dram_subsystem* dram,
		const std::map<uint64_t, uint64_t>& pending_fill_bitmap,
		uint64_t lba,
		uint64_t lines_per_page)
	{
		uint64_t full_page_mask = make_aligned_mask(0, lines_per_page);
		uint64_t pending_new_lines = 0;
		bool included_current = false;

		for (const auto& pending : pending_fill_bitmap) {
			uint64_t mask = pending.second;
			if (pending.first == lba) {
				mask |= full_page_mask;
				included_current = true;
			}
			pending_new_lines += count_cacheline_fill_misses(dram, pending.first, mask, lines_per_page);
		}

		if (!included_current) {
			pending_new_lines += count_cacheline_fill_misses(dram, lba, full_page_mask, lines_per_page);
		}

		return dram->total_data_cacheline_occupancy() + pending_new_lines <=
			dram->dynamic_data_cacheline_capacity();
	}

	int admission_granularity_bucket(uint64_t length)
	{
		switch (length) {
		case 1: return 0;
		case 2: return 1;
		case 4: return 2;
		case 8: return 3;
		case 16: return 4;
		case 32: return 5;
		case 64: return 6;
		default: return -1;
		}
	}
}
namespace
{
	constexpr uint16_t INTERNAL_FLUSH_CMDID = 0xFFFF;
}
namespace SSD_Components
{

	CXL_Manager::CXL_Manager(Host_Interface_Base *hosti)
	{
		cxl_config_para.readConfigFile();
		for (int i = 0; i < 7; i++) {
			admission_granularity_count[i] = 0;
			merge_to_granularity_count[i] = 0;
			hash_hit_count[i] = 0;
			victim_granularity_count[i] = 0;
			dynamic_min_granularity_count[i] = 0;
		}
		community_logical_size_count.fill(0);
		runtime_metadata_bytes_min = UINT64_MAX;
		runtime_segment_entry_count_min = UINT64_MAX;

		dram = new dram_subsystem{cxl_config_para};
		dram->initDRAM();
		hi = hosti;
		mshr = new cxl_mshr;
		prefetched_lba = new set<uint64_t>;
		in_progress_prefetch_lba = new map<uint64_t, uint64_t>;
		admission_history_map.reserve(cxl_config_para.admission_history_entry_count);

		const char* timeline_enable = std::getenv("CXL_ENABLE_GRANULARITY_TIMELINE");
		granularity_timeline_enabled = timeline_enable != nullptr &&
			(std::string(timeline_enable) == "1" || std::string(timeline_enable) == "true" ||
			 std::string(timeline_enable) == "TRUE");
		if (granularity_timeline_enabled) {
			const char* window_value = std::getenv("CXL_GRANULARITY_TIMELINE_WINDOW");
			if (window_value != nullptr) {
				try {
					uint64_t configured_window = std::stoull(window_value);
					if (configured_window > 0) {
						granularity_timeline_window_requests = configured_window;
					}
				}
				catch (const std::exception&) {
					std::cerr << "Invalid CXL_GRANULARITY_TIMELINE_WINDOW='"
						<< window_value << "'; using "
						<< granularity_timeline_window_requests << endl;
				}
			}
			const char* timeline_path = std::getenv("CXL_GRANULARITY_TIMELINE_FILE");
			std::string output_path = timeline_path != nullptr && timeline_path[0] != '\0'
				? timeline_path : "granularity_timeline.csv";
			const char* future_alignment_path =
				std::getenv("CXL_GRANULARITY_FUTURE_ALIGNMENT_FILE");
			granularity_future_alignment_path =
				future_alignment_path != nullptr && future_alignment_path[0] != '\0'
				? future_alignment_path : output_path + ".future.csv";
			granularity_timeline_file.open(output_path, std::ios::out | std::ios::trunc);
			if (!granularity_timeline_file.is_open()) {
				std::cerr << "Unable to open granularity timeline file: " << output_path << endl;
				granularity_timeline_enabled = false;
			}
			else {
				granularity_timeline_file
					<< "window,request_begin,request_end,demand_requests,admission_count,"
					<< "mean_access_window_coverage,mean_admission_window_coverage,"
					<< "mean_dynamic_min_cl,mean_logical_selected_cl,mean_selected_cl,mean_requested_new_cl,"
					<< "mean_backend_sectors,logical_below_dynamic_min_count,"
					<< "count_1cl,count_2cl,count_4cl,count_8cl,"
					<< "count_16cl,count_32cl,count_64cl\n";
				granularity_timeline_file << std::setprecision(12);
			}
		}

		((Host_Interface_CXL *)hi)->cxl_dram->total_number_of_requests = cxl_config_para.total_number_of_requets;
	}
	CXL_Manager::~CXL_Manager()
	{
		finalize_granularity_timeline();
		if (dram)
		{
			delete dram;
		}
		if (mshr)
		{
			delete mshr;
		}
		if (prefetched_lba)
		{
			prefetched_lba->clear();
			delete prefetched_lba;
		}
	}

	void CXL_Manager::record_admission_eviction(uint64_t lba, uint64_t used_bitmap)
	{
		if (cxl_config_para.admission_history_entry_count == 0 || used_bitmap == 0) {
			return;
		}

		auto it = admission_history_map.find(lba);
		if (it != admission_history_map.end()) {
			it->second->bitmap |= used_bitmap;
			admission_history_lru.splice(admission_history_lru.begin(), admission_history_lru, it->second);
			it->second = admission_history_lru.begin();
			return;
		}

		if (admission_history_lru.size() >= cxl_config_para.admission_history_entry_count) {
			auto& victim = admission_history_lru.back();
			admission_history_map.erase(victim.tag);
			admission_history_lru.pop_back();
			admission_history_replace_count++;
		}

		admission_history_lru.push_front(AdmissionHistoryEntry{lba, used_bitmap});
		admission_history_map[lba] = admission_history_lru.begin();
	}

	void CXL_Manager::record_cacheline_eviction_history(const LRUSet::EvictedSegment& evicted)
	{
		record_admission_eviction(evicted.lba, evicted.used_mask);
		uint64_t evicted_cls = cacheline_count_from_mask(evicted.mask);
		total_segment_evictions++;
		total_evicted_cls += evicted_cls;
		released_data_bytes += evicted_cls * 64;
		if (evicted.metadata_bytes_released > 0) {
			released_metadata_bytes += static_cast<uint64_t>(evicted.metadata_bytes_released);
		}
		int bucket = order_from_length(evicted_cls);
		if (bucket >= 0) {
			victim_granularity_count[bucket]++;
		}
		if (evicted.dirty) {
			dirty_segment_eviction_count++;
			dirty_writeback_bytes += evicted_cls * 64;
		}
		else {
			clean_segment_eviction_count++;
		}
	}

	void CXL_Manager::drain_cacheline_eviction_history()
	{
		if (dram == nullptr) {
			return;
		}
		auto evictions = dram->drain_cacheline_evictions();
		for (const auto& evicted : evictions) {
			record_cacheline_eviction_history(evicted);
		}
	}

	uint64_t CXL_Manager::get_admission_history_mask(uint64_t lba)
	{
		if (cxl_config_para.admission_history_entry_count == 0) {
			return 0;
		}
		admission_history_lookup_count++;

		auto it = admission_history_map.find(lba);
		if (it == admission_history_map.end()) {
			return 0;
		}

		admission_history_hit_count++;
		admission_history_lru.splice(admission_history_lru.begin(), admission_history_lru, it->second);
		it->second = admission_history_lru.begin();
		return it->second->bitmap;
	}

	void CXL_Manager::record_recent_access_coverage(uint64_t lba, uint64_t cacheline_idx)
	{
		if (cacheline_idx >= 64) {
			return;
		}

		static constexpr uint64_t recent_window_size = 4096;
		recent_access_window.emplace_back(lba, cacheline_idx);
		auto& page = recent_window_page_coverage[lba];
		if (page.cacheline_count[cacheline_idx] == 0) {
			page.unique_cachelines++;
			recent_window_unique_cacheline_total++;
		}
		page.cacheline_count[cacheline_idx]++;

		while (recent_access_window.size() > recent_window_size) {
			auto old = recent_access_window.front();
			recent_access_window.pop_front();
			auto page_it = recent_window_page_coverage.find(old.first);
			if (page_it == recent_window_page_coverage.end()) {
				continue;
			}

			auto& old_page = page_it->second;
			if (old_page.cacheline_count[old.second] > 0) {
				old_page.cacheline_count[old.second]--;
				if (old_page.cacheline_count[old.second] == 0) {
					old_page.unique_cachelines--;
					recent_window_unique_cacheline_total--;
				}
			}
			if (old_page.unique_cachelines == 0) {
				recent_window_page_coverage.erase(page_it);
			}
		}

	}

	double CXL_Manager::recent_window_average_page_coverage() const
	{
		if (recent_window_page_coverage.empty()) {
			return 0.0;
		}
		return static_cast<double>(recent_window_unique_cacheline_total)
			/ static_cast<double>(recent_window_page_coverage.size() * 64);
	}

	uint64_t CXL_Manager::dynamic_min_load_granularity() const
	{
		double coverage = recent_window_average_page_coverage();
		// This is only the no-community fallback.  Keep the mapping independent
		// of configured cache capacity so a larger cache does not by itself turn
		// a medium-scope phase into full-page admission.
		if (coverage >= 0.625) {
			return 64;
		}
		if (coverage >= 0.375) {
			return 32;
		}
		if (coverage >= 0.1875) {
			return 16;
		}
		if (coverage >= 0.09375) {
			return 8;
		}
		if (coverage >= 0.046875) {
			return 4;
		}
		if (coverage >= 0.0234375) {
			return 2;
		}
		return 1;
	}

	void CXL_Manager::record_granularity_timeline_access(
		uint64_t trace_page,
		uint64_t trace_cacheline)
	{
		static constexpr uint64_t coverage_scale = 1000000000ULL;
		static constexpr uint64_t future_window_size = 4096;
		if (!granularity_timeline_enabled) {
			return;
		}
		if (trace_cacheline >= 64) {
			granularity_timeline_invalid_size_count++;
			return;
		}
		if (granularity_timeline_bin_requests >= granularity_timeline_window_requests) {
			flush_granularity_timeline_bin();
		}
		uint64_t ordinal = granularity_timeline_total_requests;
		granularity_timeline_total_requests++;
		if (granularity_timeline_bin_requests == 0) {
			granularity_timeline_bin_request_begin = granularity_timeline_total_requests;
		}
		granularity_timeline_bin_requests++;
		double coverage = recent_window_average_page_coverage();
		if (std::isfinite(coverage)) {
			coverage = std::max(0.0, std::min(1.0, coverage));
			granularity_timeline_access_coverage_scaled_sum +=
				static_cast<uint64_t>(std::llround(coverage * coverage_scale));
		}

		granularity_future_window.push_back(GranularityFutureEvent{
			trace_page, ordinal, static_cast<uint8_t>(trace_cacheline), false,
			0, 0, 0, 0});
		auto& page = granularity_future_pages[trace_page];
		if (page.cacheline_count[trace_cacheline]++ == 0) {
			page.active_mask |= (1ULL << trace_cacheline);
		}
		if (granularity_future_window.size() == future_window_size + 1) {
			evaluate_granularity_future_front();
		}
	}

	void CXL_Manager::record_granularity_timeline_admission(
		uint64_t dynamic_min,
		uint64_t logical_selected_cachelines,
		uint64_t selected_cachelines,
		uint64_t requested_new_cachelines,
		uint64_t backend_sectors)
	{
		static constexpr uint64_t coverage_scale = 1000000000ULL;
		if (!granularity_timeline_enabled) {
			return;
		}
		granularity_timeline_bin_admissions++;
		double coverage = recent_window_average_page_coverage();
		if (std::isfinite(coverage)) {
			coverage = std::max(0.0, std::min(1.0, coverage));
			granularity_timeline_admission_coverage_scaled_sum +=
				static_cast<uint64_t>(std::llround(coverage * coverage_scale));
		}
		int min_bucket = admission_granularity_bucket(dynamic_min);
		if (min_bucket >= 0 && dynamic_min <= 64) {
			granularity_timeline_dynamic_min_size_count[dynamic_min]++;
		}
		else {
			granularity_timeline_invalid_size_count++;
		}
		if (selected_cachelines <= 64) {
			granularity_timeline_selected_size_count[selected_cachelines]++;
		}
		else {
			granularity_timeline_invalid_size_count++;
		}
		if (logical_selected_cachelines <= 64) {
			granularity_timeline_logical_selected_size_count[logical_selected_cachelines]++;
			if (logical_selected_cachelines < dynamic_min) {
				granularity_timeline_logical_below_min_count++;
			}
		}
		else {
			granularity_timeline_invalid_size_count++;
		}
		if (requested_new_cachelines <= 64) {
			granularity_timeline_requested_new_size_count[requested_new_cachelines]++;
		}
		else {
			granularity_timeline_invalid_size_count++;
		}
		granularity_timeline_backend_sector_total += backend_sectors;
		if (granularity_future_window.empty() ||
			granularity_future_window.back().ordinal + 1 != granularity_timeline_total_requests) {
			granularity_timeline_invalid_size_count++;
			return;
		}
		auto& event = granularity_future_window.back();
		event.admission = true;
		event.dynamic_min = static_cast<uint8_t>(dynamic_min);
		event.logical_selected = static_cast<uint8_t>(logical_selected_cachelines);
		event.newly_admitted = static_cast<uint8_t>(selected_cachelines);
		event.backend_sectors = static_cast<uint8_t>(backend_sectors);
	}

	void CXL_Manager::evaluate_granularity_future_front()
	{
		if (granularity_future_window.empty()) {
			return;
		}
		const auto event = granularity_future_window.front();
		auto page_it = granularity_future_pages.find(event.page);
		if (page_it == granularity_future_pages.end() ||
			page_it->second.cacheline_count[event.cacheline] == 0) {
			granularity_timeline_invalid_size_count++;
			granularity_future_window.pop_front();
			return;
		}
		uint64_t active_cachelines =
			static_cast<uint64_t>(__builtin_popcountll(page_it->second.active_mask));
		uint64_t expected_requests = cxl_config_para.total_number_of_requets;
		uint64_t bin_index = expected_requests == 0 ? 0 :
			(event.ordinal * granularity_future_bins.size()) / expected_requests;
		if (bin_index >= granularity_future_bins.size()) {
			bin_index = granularity_future_bins.size() - 1;
		}
		auto& bin = granularity_future_bins[bin_index];
		bin.all_access_count++;
		bin.future_active_all_sum += active_cachelines;
		if (event.admission) {
			bin.admission_count++;
			bin.future_active_admission_sum += active_cachelines;
			bin.dynamic_min_sum += event.dynamic_min;
			bin.logical_selected_sum += event.logical_selected;
			bin.newly_admitted_sum += event.newly_admitted;
			bin.backend_sector_sum += event.backend_sectors;
		}

		auto& page = page_it->second;
		page.cacheline_count[event.cacheline]--;
		if (page.cacheline_count[event.cacheline] == 0) {
			page.active_mask &= ~(1ULL << event.cacheline);
		}
		if (page.active_mask == 0) {
			granularity_future_pages.erase(page_it);
		}
		granularity_future_window.pop_front();
	}

	void CXL_Manager::write_granularity_future_alignment()
	{
		if (granularity_future_alignment_path.empty()) {
			return;
		}
		std::ofstream output(granularity_future_alignment_path,
			std::ios::out | std::ios::trunc);
		if (!output.is_open()) {
			std::cerr << "Unable to open future-alignment file: "
				<< granularity_future_alignment_path << endl;
			return;
		}
		output << "bin,all_access_count,admission_count,"
			<< "mean_future_active_cl_all_accesses,"
			<< "mean_future_active_cl_admissions,mean_dynamic_min_cl,"
			<< "mean_logical_selected_cl,mean_newly_admitted_cl,"
			<< "mean_backend_sectors\n";
		output << std::setprecision(12);
		for (uint64_t index = 0; index < granularity_future_bins.size(); index++) {
			const auto& bin = granularity_future_bins[index];
			double mean_all = bin.all_access_count == 0 ? 0.0 :
				static_cast<double>(bin.future_active_all_sum) /
				static_cast<double>(bin.all_access_count);
			double mean_admission_active = bin.admission_count == 0 ? 0.0 :
				static_cast<double>(bin.future_active_admission_sum) /
				static_cast<double>(bin.admission_count);
			double mean_dynamic_min = bin.admission_count == 0 ? 0.0 :
				static_cast<double>(bin.dynamic_min_sum) /
				static_cast<double>(bin.admission_count);
			double mean_logical = bin.admission_count == 0 ? 0.0 :
				static_cast<double>(bin.logical_selected_sum) /
				static_cast<double>(bin.admission_count);
			double mean_new = bin.admission_count == 0 ? 0.0 :
				static_cast<double>(bin.newly_admitted_sum) /
				static_cast<double>(bin.admission_count);
			double mean_sectors = bin.admission_count == 0 ? 0.0 :
				static_cast<double>(bin.backend_sector_sum) /
				static_cast<double>(bin.admission_count);
			output << index << ',' << bin.all_access_count << ','
				<< bin.admission_count << ',' << mean_all << ','
				<< mean_admission_active << ',' << mean_dynamic_min << ','
				<< mean_logical << ',' << mean_new << ',' << mean_sectors << '\n';
		}
	}

	void CXL_Manager::flush_granularity_timeline_bin()
	{
		static constexpr double coverage_scale = 1000000000.0;
		if (!granularity_timeline_enabled || granularity_timeline_bin_requests == 0 ||
			!granularity_timeline_file.is_open()) {
			return;
		}
		double mean_access_coverage =
			static_cast<double>(granularity_timeline_access_coverage_scaled_sum) /
			coverage_scale / static_cast<double>(granularity_timeline_bin_requests);
		double mean_admission_coverage = granularity_timeline_bin_admissions == 0 ? 0.0 :
			static_cast<double>(granularity_timeline_admission_coverage_scaled_sum) /
			coverage_scale / static_cast<double>(granularity_timeline_bin_admissions);
		static constexpr uint64_t granularities[7] = {1, 2, 4, 8, 16, 32, 64};
		uint64_t dynamic_min_total = 0;
		uint64_t logical_selected_total = 0;
		uint64_t selected_total = 0;
		uint64_t requested_new_total = 0;
		for (uint64_t size = 0; size <= 64; size++) {
			dynamic_min_total += size * granularity_timeline_dynamic_min_size_count[size];
			logical_selected_total += size * granularity_timeline_logical_selected_size_count[size];
			selected_total += size * granularity_timeline_selected_size_count[size];
			requested_new_total += size * granularity_timeline_requested_new_size_count[size];
		}
		double mean_dynamic_min = granularity_timeline_bin_admissions == 0 ? 0.0 :
			static_cast<double>(dynamic_min_total) /
			static_cast<double>(granularity_timeline_bin_admissions);
		double mean_logical_selected = granularity_timeline_bin_admissions == 0 ? 0.0 :
			static_cast<double>(logical_selected_total) /
			static_cast<double>(granularity_timeline_bin_admissions);
		double mean_selected = granularity_timeline_bin_admissions == 0 ? 0.0 :
			static_cast<double>(selected_total) /
			static_cast<double>(granularity_timeline_bin_admissions);
		double mean_requested_new = granularity_timeline_bin_admissions == 0 ? 0.0 :
			static_cast<double>(requested_new_total) /
			static_cast<double>(granularity_timeline_bin_admissions);
		double mean_backend_sectors = granularity_timeline_bin_admissions == 0 ? 0.0 :
			static_cast<double>(granularity_timeline_backend_sector_total) /
			static_cast<double>(granularity_timeline_bin_admissions);

		granularity_timeline_file
			<< granularity_timeline_bin_index << ','
			<< granularity_timeline_bin_request_begin << ','
			<< granularity_timeline_total_requests << ','
			<< granularity_timeline_bin_requests << ','
			<< granularity_timeline_bin_admissions << ','
			<< mean_access_coverage << ','
			<< mean_admission_coverage << ','
			<< mean_dynamic_min << ','
			<< mean_logical_selected << ','
			<< mean_selected << ','
			<< mean_requested_new << ','
			<< mean_backend_sectors << ','
			<< granularity_timeline_logical_below_min_count;
		for (int i = 0; i < 7; i++) {
			granularity_timeline_file << ','
				<< granularity_timeline_selected_size_count[granularities[i]];
		}
		granularity_timeline_file << '\n';
		granularity_timeline_file.flush();
		if (granularity_timeline_invalid_size_count != 0) {
			std::cerr << "Granularity timeline rejected "
				<< granularity_timeline_invalid_size_count
				<< " invalid admission-size samples in window "
				<< granularity_timeline_bin_index << endl;
		}

		granularity_timeline_bin_index++;
		granularity_timeline_bin_request_begin = 0;
		granularity_timeline_bin_requests = 0;
		granularity_timeline_bin_admissions = 0;
		granularity_timeline_access_coverage_scaled_sum = 0;
		granularity_timeline_admission_coverage_scaled_sum = 0;
		granularity_timeline_selected_size_count.fill(0);
		granularity_timeline_logical_selected_size_count.fill(0);
		granularity_timeline_requested_new_size_count.fill(0);
		granularity_timeline_dynamic_min_size_count.fill(0);
		granularity_timeline_invalid_size_count = 0;
		granularity_timeline_logical_below_min_count = 0;
		granularity_timeline_backend_sector_total = 0;
	}

	void CXL_Manager::finalize_granularity_timeline()
	{
		if (granularity_timeline_finalized) {
			return;
		}
		granularity_timeline_finalized = true;
		while (!granularity_future_window.empty()) {
			evaluate_granularity_future_front();
		}
		write_granularity_future_alignment();
		flush_granularity_timeline_bin();
		if (granularity_timeline_file.is_open()) {
			granularity_timeline_file.close();
		}
	}

	void CXL_Manager::record_metadata_overhead_request()
	{
		if (metadata_overhead_request_samples_finalized) {
			return;
		}
		metadata_overhead_requests_since_sample++;
		if (metadata_overhead_requests_since_sample < metadata_overhead_request_sample_interval) {
			return;
		}

		uint64_t metadata_bytes = dram->metadata_overhead_bytes();
		metadata_overhead_request_sample_sum_bytes += static_cast<long double>(metadata_bytes);
		metadata_overhead_request_sample_count++;
		double metadata_ratio = cxl_config_para.cacheline_cache_size == 0 ? 0.0 :
			static_cast<double>(metadata_bytes) / static_cast<double>(cxl_config_para.cacheline_cache_size);
		uint64_t segment_entries = 0;
		if (dram->cacheline_dram != nullptr) {
			segment_entries = dram->cacheline_dram->modeled_segment_entry_count();
		}
		if (runtime_metadata_sample_count == 0) {
			runtime_metadata_bytes_min = metadata_bytes;
			runtime_metadata_bytes_max = metadata_bytes;
			runtime_metadata_ratio_min = metadata_ratio;
			runtime_metadata_ratio_max = metadata_ratio;
			runtime_segment_entry_count_min = segment_entries;
			runtime_segment_entry_count_max = segment_entries;
		}
		else {
			runtime_metadata_bytes_min = std::min(runtime_metadata_bytes_min, metadata_bytes);
			runtime_metadata_bytes_max = std::max(runtime_metadata_bytes_max, metadata_bytes);
			runtime_metadata_ratio_min = std::min(runtime_metadata_ratio_min, metadata_ratio);
			runtime_metadata_ratio_max = std::max(runtime_metadata_ratio_max, metadata_ratio);
			runtime_segment_entry_count_min = std::min(runtime_segment_entry_count_min, segment_entries);
			runtime_segment_entry_count_max = std::max(runtime_segment_entry_count_max, segment_entries);
		}
		runtime_metadata_bytes_sum += static_cast<long double>(metadata_bytes);
		runtime_metadata_ratio_sum += metadata_ratio;
		runtime_segment_entry_count_sum += static_cast<long double>(segment_entries);
		runtime_metadata_sample_count++;
		metadata_overhead_requests_since_sample = 0;
	}

	void CXL_Manager::finalize_metadata_overhead_request_samples()
	{
		if (metadata_overhead_request_samples_finalized) {
			return;
		}
		if (metadata_overhead_requests_since_sample > 0) {
			metadata_overhead_request_sample_sum_bytes += static_cast<long double>(dram->metadata_overhead_bytes());
			metadata_overhead_request_sample_count++;
			metadata_overhead_requests_since_sample = 0;
		}
		metadata_overhead_request_samples_finalized = true;
	}

	double CXL_Manager::average_metadata_overhead_request_sample_bytes() const
	{
		if (metadata_overhead_request_sample_count == 0) {
			return static_cast<double>(dram->metadata_overhead_bytes());
		}
		return static_cast<double>(metadata_overhead_request_sample_sum_bytes
			/ static_cast<long double>(metadata_overhead_request_sample_count));
	}

	int CXL_Manager::prefetch_feedback()
	{
		double paccuracy{static_cast<double>(PREFETCH_ACCURACY_INFO.size()) / static_cast<double>(PREFETCH_INFO_MAP.size())};
		double plateness{static_cast<double>(PREFETCH_LATE_INFO.size()) / static_cast<double>(PREFETCH_INFO_MAP.size())};
		double pollution{static_cast<double>(prefetch_pollution_count) / static_cast<double>(cache_miss_count)};

		if (paccuracy >= accuracy_high && plateness >= late_thresh && pollution < pollution_thresh)
		{
			return 1;
		}
		else if (paccuracy >= accuracy_high && plateness >= late_thresh && pollution >= pollution_thresh)
		{
			return 1;
		}
		else if (paccuracy >= accuracy_high && plateness < late_thresh && pollution < pollution_thresh)
		{
			return 0;
		}
		else if (paccuracy >= accuracy_high && plateness < late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_high && paccuracy >= accuracy_low && plateness >= late_thresh && pollution < pollution_thresh)
		{
			return 1;
		}
		else if (paccuracy < accuracy_high && paccuracy >= accuracy_low && plateness >= late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_high && paccuracy >= accuracy_low && plateness < late_thresh && pollution < pollution_thresh)
		{
			return 0;
		}
		else if (paccuracy < accuracy_high && paccuracy >= accuracy_low && plateness < late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_low && plateness >= late_thresh && pollution < pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_low && plateness >= late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_low && plateness < late_thresh && pollution < pollution_thresh)
		{
			return 0;
		}
		else if (paccuracy < accuracy_low && plateness < late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}

		return 0;
	}
	void CXL_Manager::prefetch_decision_maker(uint64_t lba, bool isMiss, uint64_t prefetch_hit_count)
	{
		list<uint64_t> prefetchlba;

		/*if (mshr->getSize() < 5 || flash_back_end_queue_size - flash_back_end_access_count - 1 < 9) {
			return;
		}*/

		if (in_progress_prefetch_lba->size() == prefetch_queue_size || flash_back_end_queue_size - flash_back_end_access_count < 1 + 1 + 0)
		{
			previous_unused_lba = lba;
			return;
		}

		if (cxl_config_para.prefetch_policy == prefetchertype::no)
		{
			return;
		}
		else if (cxl_config_para.prefetch_policy == prefetchertype::tagged || cxl_config_para.prefetch_policy == prefetchertype::feedback_direct)
		{
			if (cxl_config_para.prefetch_policy == prefetchertype::feedback_direct)
			{

				int action{prefetch_feedback()};

				prefetch_level += action;
				if (prefetch_level > 4)
					prefetch_level = 4;
				if (prefetch_level < 0)
					prefetch_level = 0;
				vector<uint64_t> temp{prefetch_aggre[prefetch_level]};
				prefetchK = temp[0];
				prefetch_timing_offset = temp[1];
			}

			if (!isMiss && tagAssertedLBA.count(lba) > 0)
			{
				return;
			}
			else if (!isMiss && tagAssertedLBA.count(lba) == 0)
			{
				tagAssertedLBA.insert(lba);
			}

			for (uint64_t i = 1; i <= prefetchK; i++)
			{
				uint64_t plba{lba + i + prefetch_timing_offset};
				if (!dram->isCacheHit(plba) && !mshr->isInProgress(plba) && (in_progress_prefetch_lba->count(plba) == 0) &&
					plba * 8 <= ((Input_Stream_CXL *)(((Host_Interface_CXL *)hi)->input_stream_manager->input_streams[0]))->End_logical_sector_address)
				{
					prefetchlba.push_back(plba);
					// prefetched_lba->insert(plba);
					// tagAssertedLBA.insert(plba);
				}
			}
			// if (tagAssertedLBA.count(lba)) {
			//	tagAssertedLBA.erase(tagAssertedLBA.find(lba));
			// }
		}
		else if (cxl_config_para.prefetch_policy == prefetchertype::bo)
		{
			if (lba >= boPrefetcher.offsetundertest && boPrefetcher.inhistory(lba - boPrefetcher.offsetundertest))
			{
				boPrefetcher.incrementscore(boPrefetcher.offsetundertest);
			}

			boPrefetcher.addhistory(lba);
			boPrefetcher.offsetundertest++;

			if (boPrefetcher.endround())
			{
				boPrefetcher.offsetundertest = 1;
				boPrefetcher.round++;
				if (boPrefetcher.endlphase())
				{
					boPrefetcher.round = 0;
					boPrefetcher.findoffsets();
					if (boPrefetcher.olist.size() > 0)
					{
						boPrefetcher.prefetch_on = 1;
					}
					else
					{
						boPrefetcher.prefetch_on = 0;
					}
					boPrefetcher.resetscore();
				}
			}

			if (boPrefetcher.prefetch_on)
			{
				for (auto i : boPrefetcher.olist)
				{
					uint64_t plba{lba + (i)};

					if (!dram->isCacheHit(plba) && !mshr->isInProgress(plba) && (in_progress_prefetch_lba->count(plba) == 0) &&
						plba * 8 <= ((Input_Stream_CXL *)(((Host_Interface_CXL *)hi)->input_stream_manager->input_streams[0]))->End_logical_sector_address)
					{
						prefetchlba.push_back(plba);
						// of_delta << i << endl;
						// prefetched_lba->insert(plba);
					}
				}
			}
		}
		else if (cxl_config_para.prefetch_policy == prefetchertype::leap && isMiss)
		{
			leapPrefetcher.historyinsert(lba);
			int64_t offset{leapPrefetcher.findoffset()};

			uint64_t leapprefetchK{leapPrefetcher.getk(prefetch_hit_count)};

			if (offset == 0)
			{
				offset = leapPrefetcher.last_offset;
				for (uint64_t i = 1; i <= leapprefetchK; i++)
				{

					uint64_t plba{lba + i};

					plba = (offset >= 0) ? plba + static_cast<uint64_t>(offset) : plba - static_cast<uint64_t>(-1 * offset);

					// check if the prefetch block is in the cache or miss map
					// prefetcher_ol.print_offset(plba - event->lba);
					if (!dram->isCacheHit(plba) && !mshr->isInProgress(plba) && (in_progress_prefetch_lba->count(plba) == 0) &&
						plba * 8 <= ((Input_Stream_CXL *)(((Host_Interface_CXL *)hi)->input_stream_manager->input_streams[0]))->End_logical_sector_address)
					{

						prefetchlba.push_back(plba);
						// prefetched_lba->insert(plba);
					}
				}
			}
			else
			{
				for (uint64_t i = 1; i <= leapprefetchK; i++)
				{
					uint64_t plba{lba};

					plba = (offset >= 0) ? plba + i * static_cast<uint64_t>(offset) : plba - i * static_cast<uint64_t>(-1 * offset);

					// check if the prefetch block is in the cache or miss map
					// prefetcher_ol.print_offset(plba - event->lba);
					if (!dram->isCacheHit(plba) && !mshr->isInProgress(plba) && (in_progress_prefetch_lba->count(plba) == 0) &&
						plba * 8 <= ((Input_Stream_CXL *)(((Host_Interface_CXL *)hi)->input_stream_manager->input_streams[0]))->End_logical_sector_address)
					{

						prefetchlba.push_back(plba);
						// prefetched_lba->insert(plba);
					}
				}
			}
			leapPrefetcher.last_offset = offset;
		}

		// if (mshr->getSize()-4 < flash_back_end_queue_size - flash_back_end_access_count - 8) {
		//	if (prefetchlba.size() > mshr->getSize()-4) {
		//		prefetchlba.resize(mshr->getSize()-4);
		//	}
		// }
		// else {
		//	if (prefetchlba.size() > flash_back_end_queue_size - flash_back_end_access_count - 1 - 8) {
		//		prefetchlba.resize(flash_back_end_queue_size - flash_back_end_access_count - 1 - 8);
		//	}
		// }

		if (prefetch_queue_size - in_progress_prefetch_lba->size() < flash_back_end_queue_size - flash_back_end_access_count - 1 - 0)
		{
			if (prefetchlba.size() > prefetch_queue_size - in_progress_prefetch_lba->size())
			{
				prefetchlba.resize(prefetch_queue_size - in_progress_prefetch_lba->size());
			}
		}
		else
		{
			if (prefetchlba.size() > flash_back_end_queue_size - flash_back_end_access_count - 1 - 0)
			{
				prefetchlba.resize(flash_back_end_queue_size - flash_back_end_access_count - 1 - 0);
			}
		}

		for (auto i : prefetchlba)
		{
			prefetched_lba->insert(i);
		}
		((Host_Interface_CXL *)hi)->process_CXL_prefetch_requests(prefetchlba);
	}

	bool CXL_Manager::process_requests(uint64_t address, void *payload, bool is_pref_req, uint64_t *evcit_lba)
	{
		bool cache_miss{1};

		Submission_Queue_Entry *sqe = (Submission_Queue_Entry *)payload;
		if (!is_pref_req)
		{
			record_metadata_overhead_request();
		}
		interval_count++;
		if (interval_count ==1)
		{
			now_arrival_time = Simulator->Time();
		}
		else if (interval_count > 1)
		{
			last_arrval_time = now_arrival_time;
			now_arrival_time = Simulator->Time();
			average_interval+=(now_arrival_time - last_arrval_time);
		}

		uint64_t internal_addr = sqe->Command_specific[0] % ((((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->End_logical_sector_address - (((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->Start_logical_sector_address)) * 512);

		//sqe->Reserved2 = sqe->Command_specific[0];

		uint64_t cacheline_idx = sqe->Command_specific[0] % 4096 / 64;

		sqe->Command_specific[2] = ((uint32_t)((uint16_t)1)) & (uint32_t)(0x0000ffff);

		LHA_type memory_addr{(((LHA_type)sqe->Command_specific[1]) << 32) | (LHA_type)sqe->Command_specific[0]};
		

		// No translate
		// LHA_type lba{ memory_addr };
		// translate
		//  4096 is the page size
		LHA_type lba{memory_addr / (cxl_config_para.num_sec * 512)}; // stream alignment will be done when dealing with transaction segmentation
		LHA_type page_base_addr = lba * cxl_config_para.num_sec * 512;
        LHA_type sector_offset = (memory_addr - page_base_addr) / 512;	
		//LHA_type lsa{lba * cxl_config_para.num_sec};				  // lsa to be used for request
		LHA_type lsa = lba * cxl_config_para.num_sec + sector_offset;


		if (lsa < ((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->Start_logical_sector_address || lsa > ((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->End_logical_sector_address)
		{
			lsa = ((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->Start_logical_sector_address + (lsa % (((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->End_logical_sector_address - (((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->Start_logical_sector_address)));
			lba = lsa / cxl_config_para.num_sec;
		}

		sqe->Command_specific[0] = (uint32_t)lsa;
		sqe->Command_specific[1] = (uint32_t)(lsa >> 32);

	

		sector_offset = lsa % cxl_config_para.num_sec;
		uint64_t byte_offset_in_sector = memory_addr % 512;
		cacheline_idx = (sector_offset * 512 + byte_offset_in_sector) / 64;


		uint64_t lines_per_page = cxl_config_para.ssd_page_size / 64; // 4096/64 = 64
		if (cacheline_idx >= lines_per_page) {
			cacheline_idx = lines_per_page - 1;
		}
		uint64_t global_cacheline_id = lba * lines_per_page + cacheline_idx;
		Stats::cacheline_visit[lba][static_cast<int>(cacheline_idx)]++;
		if (!is_pref_req) {
			record_recent_access_coverage(lba, cacheline_idx);
			record_granularity_timeline_access(
				memory_addr >> 12,
				(memory_addr >> 6) & 63ULL);
		}

		// 反推
		//  uint64_t lba = global_cacheline_id / 64;
		//  uint64_t cacheline_idx = global_cacheline_id % 64;

		if (!cxl_config_para.has_cache)
		{
			// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " cm" << endl;
			return 1;
		}
		bool cacheline_dram_enable = true;
		bool only_cacheline_enable= dram->cpara.only_cacheline_enable;
		if (cacheline_dram_enable)
		{
			if (!is_pref_req && dram->cacheline_dram != nullptr) {
				total_cache_lookups++;
				int lookup_order = dram->cacheline_dram->lookup_segment_order_for_key(global_cacheline_id);
				if (lookup_order >= 0 && lookup_order < 7) {
					hash_hit_count[lookup_order]++;
				}
			}
			if (dram->cacheline_dram->contains(global_cacheline_id)) // cacheline_cache hit
			{
				cache_miss = 0;
				bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_HIT, Simulator->Time()}};
				dram_request->is_cacheline = true;
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
				if (!rw)
				{
					dram->cacheline_dram->markDirty(global_cacheline_id);
				}
				return cache_miss;
			}
			else if (!only_cacheline_enable && sqe->Opcode == NVME_READ_OPCODE && (cxl_config_para.dram_mode || dram->isCacheHit(lba))) // page_cache hit for reads
			{
				auto it = dram->write_map->find(lba);
				bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
				if(rw && it != dram->write_map->end() && ((it->second & (1ULL << cacheline_idx)) == 0)) //read miss  lba is in write_map, but the cacheline is not written yet, so it's a miss
				{

					if (!cxl_config_para.has_mshr)
					{
						no_mshr_record_node n;
						n.time = Simulator->Time();
						n.rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
						if (no_mshr_requests_record.count(lba))
						{
							no_mshr_requests_record[lba].push_back(n);
							repeated_flash_access_count++;
							ofrepeated_access << lba << " " << 1 << endl;
						}
						else
						{
							list<no_mshr_record_node> l;
							l.push_back(n);
							no_mshr_requests_record.emplace(lba, l);
							ofrepeated_access << lba << " " << 0 << endl;
						}
						cache_miss = 1;
						return cache_miss;
					}
					if (mshr->isInProgress(lba))
					{
						cache_miss = 0; // 控制不去发送请求的
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
					}
					else
					{
						// if (sqe->Opcode == NVME_WRITE_OPCODE) {
						//
						//	cout << "Check" << endl;
						// }
						if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
						{
							if (prefetch_pollution_tracker.count(lba))
							{
								prefetch_pollution_count++;
								prefetch_pollution_tracker.erase(prefetch_pollution_tracker.find(lba));
							}
						}
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						// 下面是预取控制路径
						if (in_progress_prefetch_lba->count(lba))
						{
							cache_miss = 0; // 这里也是控制不去发送请求的
						}
						else
						{
							if (!is_pref_req)
							{
								cache_miss_count++;
								prefetch_decision_maker(lba, 1, prefetch_hit_count);
							}
						}
						//cache_miss = 1;

					}		
					//cache_miss = 1;
					return cache_miss;
				}
				cache_miss = 0;
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_HIT, Simulator->Time()}};
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
				bool falsehit{0};
				uint64_t mask{~0ULL};
				// std::cout<<"mask: "<<std::bitset<64>(mask)<<std::endl;
				// exit(0);
				auto temp = dram->record_access_dram_dirty_bitmap->find(lba);
				if (temp != dram->record_access_dram_dirty_bitmap->end())
				{
					(*(dram->record_access_dram_dirty_bitmap))[lba] |= (1ULL << cacheline_idx);
				}
				if (it != dram->write_map->end())
				{
					(*(dram->write_map))[lba] |= (1ULL << cacheline_idx);
				}

				if (!cxl_config_para.dram_mode)
					((Host_Interface_CXL *)hi)->Update_CXL_DRAM_state(rw, lba, falsehit, mask);
				if (falsehit)
					falsehitcount++;
				if (!is_pref_req)
					cache_hit_count++;
				if (!is_pref_req && prefetched_lba->count(lba))
				{
					prefetch_hit_count++;
					PREFETCH_INFO_MAP[lba].hit_count++;
					PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
					PREFETCH_ACCURACY_INFO.emplace(lba);
					prefetch_decision_maker(lba, 0, prefetch_hit_count);

					if (!is_pref_req)
					{
						// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " ph" << endl;
					}
				}
				else
				{
					// prefetch_decision_maker(previous_unused_lba, 0, prefetch_hit_count);
					if (!is_pref_req)
					{
						// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " ch" << endl;
					}
				}
				return cache_miss;
			}
			else // miss in both cacheline_cache and page_cache, need to check MSHR and decide whether to send request to dram or not
			{
					bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
					if (!rw)
					{
						list<uint64_t> flush_lba;
						auto static_evicted = dram->cacheline_dram->put_segment(lba, cacheline_idx, 1, true);
						dram->cacheline_dram->mark_used(global_cacheline_id);
						if (static_evicted) {
							record_cacheline_eviction_history(*static_evicted);
						}
						if (static_evicted && static_evicted->dirty) {
							uint64_t dirty_mask = static_evicted->mask | dram->cacheline_dram->dirty_mask_for_page(static_evicted->lba);
							dram->cacheline_dram->mark_page_clean(static_evicted->lba);
							(*dram->write_map)[static_evicted->lba] |= dirty_mask;
							flush_lba.push_back(static_evicted->lba);
						}
						dram->enforce_dynamic_cache_capacity(&flush_lba);
						drain_cacheline_eviction_history();
						if (!flush_lba.empty())
						{
							uint64_t evicted_lba = flush_lba.front();
							uint64_t evicted_cacheline_mask = ~0ULL;
							auto temp = dram->write_map->find(evicted_lba);
							if (temp != dram->write_map->end())
							{
								evicted_cacheline_mask = temp->second;
								dram->write_map->erase(temp);
							}
							uint64_t evicted_sector_mask = cacheline_mask_to_sector_mask(evicted_cacheline_mask);
							uint64_t lsa = evicted_lba  * cxl_config_para.num_sec;
							sqe->Command_specific[0] = (uint32_t)lsa;
							sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
							sqe->Command_specific[2] = ((uint32_t)((uint16_t)count_sector_no_from_status_bitmap(evicted_sector_mask))) & (uint32_t)(0x0000ffff);
							sqe->Command_specific[3] = 1;
							sqe->Command_specific[4] = (uint32_t)(evicted_sector_mask & 0xFFFFFFFFULL);
							sqe->Command_specific[5] = (uint32_t)((evicted_sector_mask >> 32) & 0xFFFFFFFFULL);
							sqe->Reserved = Simulator->Time();
							cache_miss = true;
							flush_count++;
							return cache_miss;
						}
						cache_miss = 0;
						CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_MISS, Simulator->Time()}};
						dram_request->is_cacheline = true;
						dram_request->is_writeback = true;
						((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
						return cache_miss;
					}
					else // It is a read request, and it misses in both cacheline_cache and page_cache, need to check MSHR and decide whether to send request to dram or not
					{
									bool use_page_level_admission = false;
									bool use_full_page_cacheline_admission = false;
									if (!only_cacheline_enable) {
										bool already_pending_page_fill = pending_page_fill.count(lba) > 0;
										bool initialization_has_global_room = !cache_initialization_done &&
											cache_initialization_has_global_room_for_page(dram, pending_page_fill, lba, lines_per_page);
										use_page_level_admission = already_pending_page_fill ||
											(initialization_has_global_room &&
											 cache_initialization_has_page_slot_for_request(dram, pending_page_fill, lba));
										if (!already_pending_page_fill && !initialization_has_global_room) {
											cache_initialization_done = true;
										}
									}
									else {
										bool initialization_has_global_room = !cache_initialization_done &&
											cacheline_initialization_has_room_for_full_page(dram, pending_fill_bitmap, lba, lines_per_page);
										use_full_page_cacheline_admission = initialization_has_global_room;
										if (!initialization_has_global_room) {
											cache_initialization_done = true;
										}
									}
									bool has_admission_history = admission_history_map.find(lba) != admission_history_map.end();
									uint64_t admission_history_mask = get_admission_history_mask(lba);
									uint64_t min_load_granularity = dynamic_min_load_granularity();
									if (use_full_page_cacheline_admission && min_load_granularity < lines_per_page) {
										use_full_page_cacheline_admission = false;
									}
										AdmissionDecision decision = use_page_level_admission
											? AdmissionDecision{make_aligned_mask(0, lines_per_page), 0, lines_per_page, 0}
											: choose_admission_segment(dram, lba, cacheline_idx, lines_per_page, admission_history_mask, has_admission_history, min_load_granularity);
								if (!use_page_level_admission) {
									community_read_miss_count++;
									if ((decision.resident_mask & ~(1ULL << cacheline_idx)) == 0) {
										community_r_empty_count++;
									}
									else {
										community_r_nonempty_count++;
									}
									community_resident_cl_sum += static_cast<long double>(decision.resident_cl_count);
									community_resident_run_sum += static_cast<long double>(decision.resident_run_count);
									community_candidate_count += decision.candidate_count;
									if (decision.community_accepted) {
										community_accept_count++;
									}
									else {
										community_fallback_count++;
									}
									community_q_best_sum += decision.q_best;
									if (decision.q_best > community_q_best_max) {
										community_q_best_max = decision.q_best;
									}
									if (decision.community_logical_size > 0 && decision.community_logical_size <= 64) {
										community_logical_size_count[decision.community_logical_size]++;
									}
									merge_attempt_count += decision.merge_attempt_count;
									merge_success_count += decision.merge_success_count;
									merge_reject_count += decision.merge_reject_count;
									merge_total_absorbed_segment_entries += decision.merge_total_absorbed_segment_entries;
									merge_max_absorbed_segment_entries = std::max(
										merge_max_absorbed_segment_entries,
										decision.merge_max_absorbed_segment_entries);
									merge_total_metadata_bytes_saved += decision.merge_total_metadata_bytes_saved;
									merge_total_hole_cls += decision.merge_total_hole_cls;
									merge_total_hole_bytes += decision.merge_total_hole_bytes;
									merge_total_gain_bytes += decision.merge_total_gain_bytes;
									merge_max_gain_bytes = std::max(merge_max_gain_bytes, decision.merge_max_gain_bytes);
									for (int i = 0; i < 7; i++) {
										merge_to_granularity_count[i] += decision.merge_to_granularity_count[i];
									}
									demand_read_bytes += decision.demand_read_bytes;
									community_admission_bytes += decision.community_admission_bytes;
									hole_fill_bytes += decision.hole_fill_bytes;
								}
								if (use_page_level_admission) {
									pending_page_fill.insert(lba);
									pending_fill_bitmap.erase(lba);
									pending_fill_used_bitmap.erase(lba);
								}
							else {
								pending_fill_bitmap[lba] |= decision.mask;
								pending_fill_used_bitmap[lba] |= (1ULL << cacheline_idx);
							}

						uint64_t admitted_cachelines = cacheline_count_from_mask(decision.mask);
						uint64_t logical_selected_cachelines = cacheline_count_from_mask(
							decision.required_mask == 0 ? decision.mask : decision.required_mask);
						uint64_t first_idx = lowest_set_bit_index(decision.mask);
						uint64_t last_idx = highest_set_bit_index(decision.mask);
						uint64_t first_sector = first_idx / 8;
						uint64_t last_sector = last_idx / 8;
						uint64_t sector_count = last_sector - first_sector + 1;
						int bucket = admission_granularity_bucket(admitted_cachelines);
						if (bucket >= 0) {
							admission_granularity_count[bucket]++;
						}
					int min_bucket = admission_granularity_bucket(min_load_granularity);
					if (min_bucket >= 0) {
						dynamic_min_granularity_count[min_bucket]++;
					}
					dynamic_min_decision_count++;
						dynamic_min_window_coverage_sum += recent_window_average_page_coverage();
						admission_decision_count++;
						admission_cacheline_total += admitted_cachelines;
						admission_backend_sector_total += sector_count;
						actual_backend_read_bytes += sector_count * cxl_config_para.logical_sector_size;
						uint64_t requested_new_cachelines = count_cacheline_fill_misses(
							dram, lba, decision.mask, lines_per_page);
						record_granularity_timeline_admission(
							min_load_granularity, logical_selected_cachelines, admitted_cachelines,
							requested_new_cachelines, sector_count);

						uint64_t new_lsa = lba * cxl_config_para.num_sec + first_sector;

					sqe->Command_specific[0] = (uint32_t)new_lsa;
					sqe->Command_specific[1] = (uint32_t)(new_lsa >> 32);
					sqe->Command_specific[2] = ((uint32_t)((uint16_t)sector_count)) & (uint32_t)(0x0000ffff);

					if (!cxl_config_para.has_mshr)
					{
						no_mshr_record_node n;
						n.time = Simulator->Time();
						n.rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
						if (no_mshr_requests_record.count(lba))
						{
							no_mshr_requests_record[lba].push_back(n);
							repeated_flash_access_count++;
							ofrepeated_access << lba << " " << 1 << endl;
						}
						else
						{
							list<no_mshr_record_node> l;
							l.push_back(n);
							no_mshr_requests_record.emplace(lba, l);
							ofrepeated_access << lba << " " << 0 << endl;
						}
						cache_miss = 1;
						return cache_miss;
					}
					if (mshr->isInProgress(lba))
					{
						cache_miss = 0; // 控制不去发送请求的
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						return cache_miss;
					}
					else
					{
						if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
						{
							if (prefetch_pollution_tracker.count(lba))
							{
								prefetch_pollution_count++;
								prefetch_pollution_tracker.erase(prefetch_pollution_tracker.find(lba));
							}
						}
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						// 下面是预取控制路径
						if (in_progress_prefetch_lba->count(lba))
						{
							cache_miss = 0; // 这里也是控制不去发送请求的
						}
						else
						{
							if (!is_pref_req)
							{
								cache_miss_count++;
								prefetch_decision_maker(lba, 1, prefetch_hit_count);
							}
						}
					}
				}
			//	cache_miss = 0; // 说明cacheline_dram的容量够 可以直接往里面写
				return cache_miss;
			}
		}
		else 
		{
			if (cxl_config_para.dram_mode || dram->isCacheHit(lba)) 
			{
				auto it = dram->write_map->find(lba);
				bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
				if(rw && it != dram->write_map->end() && ((it->second & (1ULL << cacheline_idx)) == 0)) //read miss  lba is in write_map, but the cacheline is not written yet, so it's a miss
				{
					if (!cxl_config_para.has_mshr)
					{
						no_mshr_record_node n;
						n.time = Simulator->Time();
						n.rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
						if (no_mshr_requests_record.count(lba))
						{
							no_mshr_requests_record[lba].push_back(n);
							repeated_flash_access_count++;
							ofrepeated_access << lba << " " << 1 << endl;
						}
						else
						{
							list<no_mshr_record_node> l;
							l.push_back(n);
							no_mshr_requests_record.emplace(lba, l);
							ofrepeated_access << lba << " " << 0 << endl;
						}
						cache_miss = 1;
						return cache_miss;
					}
					if (mshr->isInProgress(lba))
					{
						cache_miss = 0; // 控制不去发送请求的
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
					}
					else
					{
						// if (sqe->Opcode == NVME_WRITE_OPCODE) {
						//
						//	cout << "Check" << endl;
						// }
						if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
						{
							if (prefetch_pollution_tracker.count(lba))
							{
								prefetch_pollution_count++;
								prefetch_pollution_tracker.erase(prefetch_pollution_tracker.find(lba));
							}
						}
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						// 下面是预取控制路径
						if (in_progress_prefetch_lba->count(lba))
						{
							cache_miss = 0; // 这里也是控制不去发送请求的
						}
						else
						{
							if (!is_pref_req)
							{
								cache_miss_count++;
								prefetch_decision_maker(lba, 1, prefetch_hit_count);
							}
						}
						//cache_miss = 1;

					}		
					//cache_miss = 1;
					return cache_miss;
				}
				cache_miss = 0;
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_HIT, Simulator->Time()}};
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
				bool falsehit{0};
				uint64_t mask{~0ULL};
				// std::cout<<"mask: "<<std::bitset<64>(mask)<<std::endl;
				// exit(0);
				auto temp = dram->record_access_dram_dirty_bitmap->find(lba);
				if (temp != dram->record_access_dram_dirty_bitmap->end())
				{
					(*(dram->record_access_dram_dirty_bitmap))[lba] |= (1ULL << cacheline_idx);
				}
				if (it != dram->write_map->end())
				{
					(*(dram->write_map))[lba] |= (1ULL << cacheline_idx);
				}

				if (!cxl_config_para.dram_mode)
					((Host_Interface_CXL *)hi)->Update_CXL_DRAM_state(rw, lba, falsehit, mask);
				if (falsehit)
					falsehitcount++;
				if (!is_pref_req)
					cache_hit_count++;
				if (!is_pref_req && prefetched_lba->count(lba))
				{
					prefetch_hit_count++;
					PREFETCH_INFO_MAP[lba].hit_count++;
					PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
					PREFETCH_ACCURACY_INFO.emplace(lba);
					prefetch_decision_maker(lba, 0, prefetch_hit_count);

					if (!is_pref_req)
					{
						// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " ph" << endl;
					}
				}
				else
				{
					// prefetch_decision_maker(previous_unused_lba, 0, prefetch_hit_count);
					if (!is_pref_req)
					{
						// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " ch" << endl;
					}
				}
				return cache_miss;
			}
			else
			{
				if (!is_pref_req)
				{
					// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " cm" << endl;
				}
				bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
				if (sqe->Opcode == NVME_WRITE_OPCODE)
				{

					list<uint64_t> *flush_lba{new list<uint64_t>};
					uint64_t cacheline_bitmap = 1ULL << cacheline_idx;
					dram->process_miss_data_ready_new(rw, lba, flush_lba, Simulator->Time(), prefetched_lba, tagAssertedLBA, prefetch_pollution_tracker, serviced_before_lba, cacheline_bitmap);
					drain_cacheline_eviction_history();
					// mask = 1 new write only one cacheline
					if(flush_lba->size() > 0) 
					{
						if (flush_lba->size() > 1)
						{
							std::cout<<"error: flush more than 1 cacheline!"<<endl;
							while(1){}
						}
						uint64_t evcit_lba = flush_lba->front();
						flush_lba->pop_front();
						uint64_t lsa = evcit_lba * cxl_config_para.num_sec;
						sqe->Command_specific[0] = (uint32_t)lsa;
						sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
							uint64_t mask = ~0ULL;
						auto it = dram->write_map->find(evcit_lba);
						if (it != dram->write_map->end())
						{
							mask = it->second;
							dram->write_map->erase(it);
						}
						sqe->Command_specific[2] = ((uint32_t)((uint16_t)count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff); 
						sqe->Command_specific[3] = 1;
						uint64_t sector_bitmap = cacheline_mask_to_sector_mask(mask);
						sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
						sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
						sqe->Reserved = Simulator->Time();
						cache_miss = true;
						flush_count++;
					}
					else
					{
						CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_MISS, Simulator->Time()}};
						dram_request->is_writeback = true;
						((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
						cache_miss = false;
					}
				}
				else
				{
					if (!cxl_config_para.has_mshr)
					{
						no_mshr_record_node n;
						n.time = Simulator->Time();
						n.rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
						if (no_mshr_requests_record.count(lba))
						{
							no_mshr_requests_record[lba].push_back(n);
							repeated_flash_access_count++;
							ofrepeated_access << lba << " " << 1 << endl;
						}
						else
						{
							list<no_mshr_record_node> l;
							l.push_back(n);
							no_mshr_requests_record.emplace(lba, l);
							ofrepeated_access << lba << " " << 0 << endl;
						}
						cache_miss = 1;
						return cache_miss;
					}
					if (mshr->isInProgress(lba))
					{
						cache_miss = 0; // 控制不去发送请求的
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
					}
					else
					{
						// if (sqe->Opcode == NVME_WRITE_OPCODE) {
						//
						//	cout << "Check" << endl;
						// }
						if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
						{
							if (prefetch_pollution_tracker.count(lba))
							{
								prefetch_pollution_count++;
								prefetch_pollution_tracker.erase(prefetch_pollution_tracker.find(lba));
							}
						}
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						// 下面是预取控制路径
						if (in_progress_prefetch_lba->count(lba))
						{
							cache_miss = 0; // 这里也是控制不去发送请求的
						}
						else
						{
							if (!is_pref_req)
							{
								cache_miss_count++;
								prefetch_decision_maker(lba, 1, prefetch_hit_count);
							}
						}
					}

				}
				}
			}
			return cache_miss;
	}
	int CXL_Manager::count_sectors_from_cacheline_mask(uint64_t cl_mask) {
    int sector_count = 0;
    for (int s = 0; s < 8; s++) {
        uint64_t group = (cl_mask >> (s * 8)) & 0xFFULL;
        if (group != 0) {
            sector_count++;
        }
    }
    return sector_count;
	}

	uint64_t CXL_Manager::cacheline_mask_to_sector_mask(uint64_t cl_mask) {
		uint64_t sec_mask = 0;
		for (int s = 0; s < 8; s++) {
			uint64_t group = (cl_mask >> (s * 8)) & 0xFFULL;
			if (group != 0) {
				sec_mask |= (1ULL << s);
			}
		}
		return sec_mask;
	}

	uint64_t CXL_Manager::sector_mask_to_cacheline_mask(uint64_t sec_mask) {
    uint64_t cl_mask = 0;
    for (int sec = 0; sec < 8; sec++) {
        if (sec_mask & (1ULL << sec)) {
            cl_mask |= (0xFFULL << (sec * 8));
        }
    }
    return cl_mask;
}

	void CXL_Manager::request_serviced(User_Request *request)
	{
		Submission_Queue_Entry *sqe = (Submission_Queue_Entry *)request->IO_command_info;
		LHA_type lba{((((LHA_type)sqe->Command_specific[1]) << 32) | (LHA_type)sqe->Command_specific[0]) / (cxl_config_para.num_sec)};
		if (!cxl_config_para.has_cache)
		{
			unique_lba.insert(lba);

			if (flash_back_end_access_count >= flash_back_end_queue_size)
			{
				if (flash_back_end_access_count > flash_back_end_queue_size)
				{
					std::cout << "Check4" << endl;
				}
				hi->Notify_CXL_Host_flash_not_full();
			}
			flash_back_end_access_count--;

			oflat_no_cache << Simulator->Time() - request->STAT_InitiationTime << endl;

			total_number_of_accesses++;

			if (sqe->Opcode == NVME_WRITE_OPCODE)
				no_cache_flash_write_count++;
			else
				no_cache_flash_read_count++;

			float current_progress{static_cast<float>(total_number_of_accesses) / static_cast<float>(cxl_config_para.total_number_of_requets)};
			if (current_progress * 100 - perc > 1)
			{
				perc += 1;
				uint8_t number_of_bars{static_cast<uint8_t>(perc / 4)};
				std::cout << "Simulation progress: [";
				for (auto i = 0; i < number_of_bars; i++)
				{
					std::cout << "=";
				}
				for (auto i = 0; i < 25 - number_of_bars - 1; i++)
				{
					std::cout << " ";
				}
				std::cout << "] " << perc << "%" << "\r";
			}

			if (total_number_of_accesses == cxl_config_para.total_number_of_requets)
			{
				std::cout << "Simulation progress: [";
				for (auto i = 0; i < 25; i++)
				{
					std::cout << "=";
				}
				std::cout << "] " << 100 << "%" << std::endl;
				std::cout << "Flash read count: " << no_cache_flash_read_count << endl;
				std::cout << "Flash write count: " << no_cache_flash_write_count << endl;
				std::cout << "Unique LBA ratio: " << static_cast<float>(unique_lba.size()) / static_cast<float>(cxl_config_para.total_number_of_requets) << endl;
				std::cout << "Request ends at timestamp: " << static_cast<float>(Simulator->Time()) / 1000000000 << " s" << endl;
			}
			return;
		}
		if (sqe->Opcode == NVME_WRITE_OPCODE)
		{ // flush done
			if (flash_back_end_access_count >= flash_back_end_queue_size)
			{
				if (flash_back_end_access_count > flash_back_end_queue_size)
				{
					//std::cout << "Check5" << endl;
				}
				hi->Notify_CXL_Host_flash_not_full();
			}

			// if write miss write bcck
			if (sqe->Command_specific[3] == 1) // this is write miss write back
			{
				bool rw{false};
				rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_MISS, ((Submission_Queue_Entry*)request->IO_command_info)->Reserved}};
				//dram_request->is_writeback = true;
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
			
			}
			flash_back_end_access_count--;

			// flush_count++;
			// ofFlush << request->STAT_InitiationTime << " " << Simulator->Time() << endl;
			// outputf.of << "Finshed_time " << Simulator->Time()  << " Starting_time " << request->STAT_InitiationTime << " Flush_done_at " << lba <<  std::endl;
			return;
		}

		if (!cxl_config_para.has_mshr)
		{
			if (((Host_Interface_CXL *)hi)->cxl_dram->getDRAMAvailability() && no_mshr_not_yet_serviced_lba.empty())
			{
				no_mshr_record_node n{no_mshr_requests_record[lba].front()};
				no_mshr_requests_record[lba].pop_front();

				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{static_cast<unsigned int>(64), lba, n.rw, CXL_DRAM_EVENTS::CACHE_MISS, n.time}};
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);

				list<uint64_t> *flush_lba{new list<uint64_t>};

				page_status_type sector_bitmap = ((Address_Mapping_Unit_Page_Level*) ((FTL*)(hi->cache->nvm_firmware))->Address_Mapping_Unit)->domains[0]->Get_page_status(true, 0 ,lba);

				//std::cout<<std::bitset<64>(sector_bitmap)<<std::endl;
				auto* amu = static_cast<FTL*>(hi->cache->nvm_firmware)->Address_Mapping_Unit;
				PPA_type ppa;
			//	page_status_type sector_bitmap = 0;
				amu->Get_data_mapping_info_for_gc(0, lba, ppa, sector_bitmap);
				// std::cout << "lba=" << lba
				// 		<< " ppa=" << ppa
				// 		<< " sector_bitmap=" << std::bitset<64>(sector_bitmap)
				// 		<< std::endl;
				//std::cout<<std::bitset<8>(sector_bitmap)<<std::endl;

				//uint64_t cacheline_bitmap = sector_mask_to_cacheline_mask(sector_bitmap);
				//	std::cout<<std::bitset<64>(cacheline_bitmap)<<std::endl;
//uint64_t cacheline_idx = sqe->Command_specific[0] % 4096 / 64;
			   // uint64_t cacheline_idx = sqe->Reserved2 % 4096 / 64;
				   uint64_t cacheline_bitmap = 0;
					if (dram->cpara.only_cacheline_enable)
					{
						if (pending_page_fill.erase(lba) > 0) {
							dram->process_miss_data_ready_new(n.rw, lba, flush_lba, Simulator->Time(), prefetched_lba, tagAssertedLBA, prefetch_pollution_tracker, serviced_before_lba, cacheline_bitmap);
							drain_cacheline_eviction_history();
						}
						else {
							((Host_Interface_CXL*)hi)->Fill_cachelines_from_bitmap(lba, flush_lba);
						}
					}
					else
					{
						dram->process_miss_data_ready_new(n.rw, lba, flush_lba, Simulator->Time(), prefetched_lba, tagAssertedLBA, prefetch_pollution_tracker, serviced_before_lba,cacheline_bitmap);
						drain_cacheline_eviction_history();
					}

				
				if (flash_back_end_access_count >= flash_back_end_queue_size)
				{
					if (flash_back_end_access_count > flash_back_end_queue_size)
					{
						//std::cout << "Check6" << endl;
					}
					hi->Notify_CXL_Host_flash_not_full();
				}
				flash_back_end_access_count--;

				if (no_mshr_requests_record[lba].empty())
				{
					no_mshr_requests_record.erase(no_mshr_requests_record.find(lba));
				}

				while (!flush_lba->empty())
				{
					flush_count++;
					uint64_t lba = flush_lba->front();
					uint64_t lsa{lba * 8};

					flush_lba->pop_front();
					Submission_Queue_Entry *sqe{new Submission_Queue_Entry};
					sqe->Command_Identifier = 0;
					sqe->Opcode = NVME_WRITE_OPCODE;
					sqe->Command_specific[0] = (uint32_t)lsa;
					sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
					uint64_t mask = ~0ULL;
					auto it = dram->write_map->find(lba);
					if (it != dram->write_map->end())
					{
						mask = it->second;
						dram->write_map->erase(it);
					}
					//uint64_t evicted_lba = lba / 64; // lba eviceed from cacheline_cache
							// Clean only cachelines covered by this Segment writeback.
					uint64_t base_cacheline_id = lba * 64;
					uint64_t evicted_cacheline_mask = 0ULL;
					for (int i = 0 ; i < 64 ; i++)
					{
						if ((mask & (1ULL << i)) == 0)
							continue;
						//uint64_t global_cacheline_id = base_cacheline_id + i;
						uint64_t global_cacheline_id = base_cacheline_id + i;
						if (!dram->cacheline_dram->contains(global_cacheline_id, false))
							continue;
						evicted_cacheline_mask |= (1ULL << i);
						// 这里处理这个 page 里的第 i 个 cacheline
						dram->cacheline_dram->setDirty(global_cacheline_id, false); // 先把这个
						
					}
					
				
			

					sqe->Command_specific[2] = ((uint32_t)((uint16_t)count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff);
					sqe->Command_specific[3] = 0; // flush  caued by read miss;
					uint64_t sector_bitmap = cacheline_mask_to_sector_mask(mask);
					sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
					sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
					sqe->Reserved = 0;
					sqe->PRP_entry_1 = (DATA_MEMORY_REGION);		  // Dummy addresses, just to emulate data read/write access
					sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000); // Dummy addresses

					((Request_Fetch_Unit_CXL *)(((Host_Interface_CXL *)hi)->request_fetch_unit))->Fetch_next_request(0);

					((Request_Fetch_Unit_CXL *)(((Host_Interface_CXL *)hi)->request_fetch_unit))->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
				}

				delete flush_lba;
			}
			else
			{
				no_mshr_not_yet_serviced_lba.push_back(lba);
			}

			return;
		}

		if (((Host_Interface_CXL *)hi)->cxl_dram->getDRAMAvailability() && serviced_before_lba.size() == 0)
		{

			if (serviced_before_lba.size() > 0)
			{
				std::cout << Simulator->Time() << endl;
			}

			bool rw{true};
			bool completely_removed{0};
			((Host_Interface_CXL *)hi)->Update_CXL_DRAM_state_when_miss_data_ready(rw, lba, 0, completely_removed);

			if (!completely_removed)
			{
				serviced_before_lba.emplace(lba);
			}
		}
		else
		{
			((Host_Interface_CXL *)hi)->cxl_man->not_yet_serviced_lba.emplace(lba);
		}
	}

	Input_Stream_CXL::~Input_Stream_CXL()
	{
		for (auto &user_request : Waiting_user_requests)
			delete user_request;
		for (auto &user_request : Completed_user_requests)
			delete user_request;
	}

	Input_Stream_Manager_CXL::Input_Stream_Manager_CXL(Host_Interface_Base *host_interface, uint16_t queue_fetch_szie) : Input_Stream_Manager_Base(host_interface), Queue_fetch_size(queue_fetch_szie)
	{
	}

	stream_id_type Input_Stream_Manager_CXL::Create_new_stream(IO_Flow_Priority_Class priority_class, LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
															   uint64_t submission_queue_base_address, uint16_t submission_queue_size, uint64_t completion_queue_base_address, uint16_t completion_queue_size)
	{
		if (end_logical_sector_address < start_logical_sector_address)
		{
			PRINT_ERROR("Error in allocating address range to a stream in host interface: the start address should be smaller than the end address.")
		}
		Input_Stream_CXL *input_stream = new Input_Stream_CXL(priority_class, start_logical_sector_address, end_logical_sector_address,
															  submission_queue_base_address, submission_queue_size, completion_queue_base_address, completion_queue_size);
		this->input_streams.push_back(input_stream);

		return (stream_id_type)(this->input_streams.size() - 1);
	}

	inline void Input_Stream_Manager_CXL::Submission_queue_tail_pointer_update(stream_id_type stream_id, uint16_t tail_pointer_value)
	{
		//((Input_Stream_CXL*)input_streams[stream_id])->Submission_tail = tail_pointer_value;

		// if (((Input_Stream_CXL*)input_streams[stream_id])->On_the_fly_requests < Queue_fetch_size) {
		//	((Host_Interface_CXL*)host_interface)->request_fetch_unit->Fetch_next_request(stream_id);
		//	((Input_Stream_CXL*)input_streams[stream_id])->On_the_fly_requests++;
		//	((Input_Stream_CXL*)input_streams[stream_id])->Submission_head++;//Update submission queue head after starting fetch request
		//	if (((Input_Stream_CXL*)input_streams[stream_id])->Submission_head == ((Input_Stream_CXL*)input_streams[stream_id])->Submission_queue_size) {//Circular queue implementation
		//		((Input_Stream_CXL*)input_streams[stream_id])->Submission_head = 0;
		//	}
		// }
	}

	inline void Input_Stream_Manager_CXL::Completion_queue_head_pointer_update(stream_id_type stream_id, uint16_t head_pointer_value)
	{
		//((Input_Stream_CXL*)input_streams[stream_id])->Completion_head = head_pointer_value;

		////If this check is true, then the host interface couldn't send the completion queue entry, since the completion queue was full
		// if (((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.size() > 0) {
		//	User_Request* request = ((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.front();
		//	((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.pop_front();
		//	inform_host_request_completed(stream_id, request);
		// }
	}

	inline void Input_Stream_Manager_CXL::Handle_new_arrived_request(User_Request *request)
	{
		((Input_Stream_CXL *)input_streams[request->Stream_id])->Submission_head_informed_to_host++;
		if (((Input_Stream_CXL *)input_streams[request->Stream_id])->Submission_head_informed_to_host == ((Input_Stream_CXL *)input_streams[request->Stream_id])->Submission_queue_size)
		{ // Circular queue implementation
			((Input_Stream_CXL *)input_streams[request->Stream_id])->Submission_head_informed_to_host = 0;
		}
		if (request->Type == UserRequestType::READ)
		{
			((Input_Stream_CXL *)input_streams[request->Stream_id])->Waiting_user_requests.push_back(request);
			((Input_Stream_CXL *)input_streams[request->Stream_id])->STAT_number_of_read_requests++;
			segment_user_request(request);

			((Host_Interface_CXL *)host_interface)->broadcast_user_request_arrival_signal(request);
		}
		else
		{ // This is a write request
			((Input_Stream_CXL *)input_streams[request->Stream_id])->Waiting_user_requests.push_back(request);
			((Input_Stream_CXL *)input_streams[request->Stream_id])->STAT_number_of_write_requests++;
			((Host_Interface_CXL *)host_interface)->request_fetch_unit->Fetch_write_data(request);
		}
	}

	inline void Input_Stream_Manager_CXL::Handle_arrived_write_data(User_Request *request)
	{
		segment_user_request(request);
		((Host_Interface_CXL *)host_interface)->broadcast_user_request_arrival_signal(request);
	}

	inline void Input_Stream_Manager_CXL::Handle_serviced_request(User_Request *request)
	{
		stream_id_type stream_id = request->Stream_id;
		((Input_Stream_CXL *)input_streams[request->Stream_id])->Waiting_user_requests.remove(request);
		((Input_Stream_CXL *)input_streams[stream_id])->On_the_fly_requests--;

		// list<uint64_t>* flush_lba{ new list<uint64_t> };
		((Host_Interface_CXL *)host_interface)->cxl_man->request_serviced(request);

		DEBUG("** Host Interface: Request #" << request->ID << " from stream #" << request->Stream_id << " is finished")
		////If this is a read request, then the read data should be written to host memory
		// if (request->Type == UserRequestType::READ) {
		//	((Host_Interface_CXL*)host_interface)->request_fetch_unit->Send_read_data(request);
		// }

		////there are waiting requests in the submission queue but have not been fetched, due to Queue_fetch_size limit
		// if (((Input_Stream_CXL*)input_streams[stream_id])->Submission_head != ((Input_Stream_CXL*)input_streams[stream_id])->Submission_tail) {
		//	((Host_Interface_CXL*)host_interface)->request_fetch_unit->Fetch_next_request(stream_id);
		//	((Input_Stream_CXL*)input_streams[stream_id])->On_the_fly_requests++;
		//	((Input_Stream_CXL*)input_streams[stream_id])->Submission_head++;//Update submission queue head after starting fetch request
		//	if (((Input_Stream_CXL*)input_streams[stream_id])->Submission_head == ((Input_Stream_CXL*)input_streams[stream_id])->Submission_queue_size) {//Circular queue implementation
		//		((Input_Stream_CXL*)input_streams[stream_id])->Submission_head = 0;
		//	}
		// }

		////Check if completion queue is full
		// if (((Input_Stream_CXL*)input_streams[stream_id])->Completion_head > ((Input_Stream_CXL*)input_streams[stream_id])->Completion_tail) {
		//	//completion queue is full
		//	if (((Input_Stream_CXL*)input_streams[stream_id])->Completion_tail + 1 == ((Input_Stream_CXL*)input_streams[stream_id])->Completion_head) {
		//		((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.push_back(request);//Wait while the completion queue is full
		//		return;
		//	}
		// }
		// else if (((Input_Stream_CXL*)input_streams[stream_id])->Completion_tail - ((Input_Stream_CXL*)input_streams[stream_id])->Completion_head
		//	== ((Input_Stream_CXL*)input_streams[stream_id])->Completion_queue_size - 1) {
		//	((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.push_back(request);//Wait while the completion queue is full
		//	return;
		// }

		// inform_host_request_completed(stream_id, request);//Completion queue is not full, so the device can DMA the completion queue entry to the host
		DELETE_REQUEST_NVME(request);
	}

	uint16_t Input_Stream_Manager_CXL::Get_submission_queue_depth(stream_id_type stream_id)
	{
		return ((Input_Stream_CXL *)this->input_streams[stream_id])->Submission_queue_size;
	}

	uint16_t Input_Stream_Manager_CXL::Get_completion_queue_depth(stream_id_type stream_id)
	{
		return ((Input_Stream_CXL *)this->input_streams[stream_id])->Completion_queue_size;
	}

	IO_Flow_Priority_Class Input_Stream_Manager_CXL::Get_priority_class(stream_id_type stream_id)
	{
		return ((Input_Stream_CXL *)this->input_streams[stream_id])->Priority_class;
	}

	inline void Input_Stream_Manager_CXL::inform_host_request_completed(stream_id_type stream_id, User_Request *request)
	{
	}

	void Input_Stream_Manager_CXL::segment_user_request(User_Request *user_request)
	{
		LHA_type lsa = user_request->Start_LBA;
		LHA_type lsa2 = user_request->Start_LBA;
		unsigned int req_size = user_request->SizeInSectors;
		page_status_type access_status_bitmap = 0;
		unsigned int hanled_sectors_count = 0;
		unsigned int transaction_size = 0;
		if (user_request->Type == UserRequestType::WRITE && user_request->Sector_bitmap != 0)
		{
			unsigned int sector_cnt = count_sector_no_from_status_bitmap(user_request->Sector_bitmap);
			NVM_Transaction_Flash_WR *transaction =
				new NVM_Transaction_Flash_WR(
					Transaction_Source_Type::USERIO,
					user_request->Stream_id,
					sector_cnt * SECTOR_SIZE_IN_BYTE,
					user_request->Start_LBA/ host_interface->sectors_per_subpage,
					user_request,
					0,
					user_request->Sector_bitmap,
					CurrentTimeStamp);
			user_request->Transaction_list.push_back(transaction);
			return;
		}

		while (hanled_sectors_count < req_size)
		{
			// Check if LSA is in the correct range allocted to the stream
			if (lsa < ((Input_Stream_CXL *)input_streams[user_request->Stream_id])->Start_logical_sector_address || lsa > ((Input_Stream_CXL *)input_streams[user_request->Stream_id])->End_logical_sector_address)
			{
				lsa = ((Input_Stream_CXL *)input_streams[user_request->Stream_id])->Start_logical_sector_address + (lsa % (((Input_Stream_CXL *)input_streams[user_request->Stream_id])->End_logical_sector_address - (((Input_Stream_CXL *)input_streams[user_request->Stream_id])->Start_logical_sector_address)));
			}
			LHA_type internal_lsa = lsa - ((Input_Stream_CXL *)input_streams[user_request->Stream_id])->Start_logical_sector_address; // For each flow, all lsa's should be translated into a range starting from zero

#if PATCH_SEGMENT_REQ
			LPA_type lpa;
			if (user_request->Type == UserRequestType::READ)
			{
				transaction_size = host_interface->sectors_per_subpage - (unsigned int)(lsa % host_interface->sectors_per_subpage);
				if (hanled_sectors_count + transaction_size >= req_size)
				{
					transaction_size = req_size - hanled_sectors_count;
				}
				lpa = internal_lsa / host_interface->sectors_per_subpage;

				page_status_type temp = ~(0xffffffffffffffff << (int)transaction_size); //
				access_status_bitmap = temp << (int)(internal_lsa % host_interface->sectors_per_subpage);
				//std::cout<<transaction_size<<" "<< count_sector_no_from_status_bitmap(access_status_bitmap) << std::endl;
				// printf("access_status_bitmap: %lx", access_status_bitmap);
			}
			else if (user_request->Type == UserRequestType::WRITE)
			{
				transaction_size = host_interface->sectors_per_subpage - (unsigned int)(lsa % host_interface->sectors_per_subpage);
				if (hanled_sectors_count + transaction_size >= req_size)
				{
					transaction_size = req_size - hanled_sectors_count;
				}
				lpa = internal_lsa / host_interface->sectors_per_subpage;

				page_status_type temp = ~(0xffffffffffffffff << (int)transaction_size);					  //
				access_status_bitmap = temp << (int)(internal_lsa % host_interface->sectors_per_subpage); // write_sectors_bitmap
																										  // printf("access_status_bitmap: %lx", access_status_bitmap);
			}
		// 	std::cout << "[READ_SEG] lsa=" << lsa
        //   << " internal_lsa=" << internal_lsa
        //   << " tx_size=" << transaction_size
        //   << " bitmap=" << std::bitset<64>(access_status_bitmap)
        //   << std::endl;
			// std::cout << "tr size: " << transaction_size << std::endl;
#else
			transaction_size = host_interface->sectors_per_page - (unsigned int)(lsa % host_interface->sectors_per_page);
			if (hanled_sectors_count + transaction_size >= req_size)
			{
				transaction_size = req_size - hanled_sectors_count;
			}
			LPA_type lpa = internal_lsa / host_interface->sectors_per_page;

			page_status_type temp = ~(0xffffffffffffffff << (int)transaction_size);				   //
			access_status_bitmap = temp << (int)(internal_lsa % host_interface->sectors_per_page); // write_sectors_bitmap
#endif
			if (user_request->Type == UserRequestType::READ)
			{
				NVM_Transaction_Flash_RD *transaction = new NVM_Transaction_Flash_RD(Transaction_Source_Type::USERIO, user_request->Stream_id,
				transaction_size * SECTOR_SIZE_IN_BYTE, lpa, NO_PPA, user_request, 0, access_status_bitmap, CurrentTimeStamp);
				user_request->Transaction_list.push_back(transaction);
				input_streams[user_request->Stream_id]->STAT_number_of_read_transactions++;
			}
			else
			{ // user_request->Type == UserRequestType::WRITE
				// cout << user_request->Start_LBA << endl;
				NVM_Transaction_Flash_WR *transaction = new NVM_Transaction_Flash_WR(Transaction_Source_Type::USERIO, user_request->Stream_id,
																					 transaction_size * SECTOR_SIZE_IN_BYTE, lpa, user_request, 0, access_status_bitmap, CurrentTimeStamp);
				user_request->Transaction_list.push_back(transaction);
				input_streams[user_request->Stream_id]->STAT_number_of_write_transactions++;
			}

			lsa = lsa + transaction_size;
			hanled_sectors_count += transaction_size;
		}
	}

	Request_Fetch_Unit_CXL::Request_Fetch_Unit_CXL(Host_Interface_Base *host_interface) : Request_Fetch_Unit_Base(host_interface), current_phase(0xffff), number_of_sent_cqe(0) {}

	void Request_Fetch_Unit_CXL::Process_pcie_write_message(uint64_t address, void *payload, unsigned int payload_size)
	{
		/*Host_Interface_CXL* hi = (Host_Interface_CXL*)host_interface;
		uint64_t val = (uint64_t)payload;
		switch (address)
		{
		case SUBMISSION_QUEUE_REGISTER_1:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(0, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_1:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(0, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_2:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(1, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_2:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(1, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_3:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(2, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_3:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(2, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_4:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(3, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_4:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(3, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_5:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(4, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_5:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(4, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_6:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(5, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_6:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(5, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_7:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(6, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_7:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(6, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_8:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(7, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_8:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(7, (uint16_t)val);
			break;
		default:
			throw std::invalid_argument("Unknown register is written!");
		}*/
	}

	void Request_Fetch_Unit_CXL::Process_pcie_read_message(uint64_t address, void *payload, unsigned int payload_size)
	{
		Host_Interface_CXL *hi = (Host_Interface_CXL *)host_interface;

		DMA_Req_Item *dma_req_item = dma_list.front();
		dma_list.pop_front();

		switch (dma_req_item->Type)
		{
		case DMA_Req_Type::REQUEST_INFO:
		{

			User_Request *new_reqeust = new User_Request;
			new_reqeust->IO_command_info = payload;
			new_reqeust->Stream_id = (stream_id_type)((uint64_t)(dma_req_item->object));
			new_reqeust->Priority_class = ((Input_Stream_Manager_CXL *)host_interface->input_stream_manager)->Get_priority_class(new_reqeust->Stream_id);
			new_reqeust->STAT_InitiationTime = Simulator->Time();
			Submission_Queue_Entry *sqe = (Submission_Queue_Entry *)payload;
			new_reqeust->ToBeIgnored = (sqe->Command_Identifier == INTERNAL_FLUSH_CMDID);
			new_reqeust->Sector_bitmap = ((uint64_t)sqe->Command_specific[5] << 32) | (uint64_t)sqe->Command_specific[4];
			
			switch (sqe->Opcode)
			{
			case NVME_READ_OPCODE:
				new_reqeust->Type = UserRequestType::READ;
				new_reqeust->Start_LBA = (((LHA_type)sqe->Command_specific[1]) << 32) | (LHA_type)sqe->Command_specific[0]; // Command Dword 10 and Command Dword 11
				new_reqeust->SizeInSectors = sqe->Command_specific[2] & (LHA_type)(0x0000ffff);
				//new_reqeust->SizeInSectors = ((Host_Interface_CXL*)host_interface)->cxl_man->cxl_config_para.num_sec & (LHA_type)(0x0000ffff);
				new_reqeust->Size_in_byte = new_reqeust->SizeInSectors * SECTOR_SIZE_IN_BYTE;
				hi->cxl_man->flash_read_count++;
				hi->cxl_man->flash_back_end_access_count++;
				break;
			case NVME_WRITE_OPCODE:
				new_reqeust->Type = UserRequestType::WRITE;
				new_reqeust->Start_LBA = (((LHA_type)sqe->Command_specific[1]) << 32) | (LHA_type)sqe->Command_specific[0]; // Command Dword 10 and Command Dword 11
				// cout << new_reqeust->Start_LBA << endl;
				new_reqeust->SizeInSectors = sqe->Command_specific[2] & (LHA_type)(0x0000ffff);
				new_reqeust->Size_in_byte = new_reqeust->SizeInSectors * SECTOR_SIZE_IN_BYTE;
				hi->cxl_man->flash_back_end_access_count++;
			
				break;
			default:
				throw std::invalid_argument("NVMe command is not supported!");
			}
			if (hi->cxl_man->flash_back_end_access_count >= hi->cxl_man->flash_back_end_queue_size)
			{
				hi->Notify_CXL_Host_flash_full();
			}

			((Input_Stream_Manager_CXL *)(hi->input_stream_manager))->Handle_new_arrived_request(new_reqeust);
			break;
		}
		case DMA_Req_Type::WRITE_DATA:
			COPYDATA(((User_Request *)dma_req_item->object)->Data, payload, payload_size);
			((Input_Stream_Manager_CXL *)(hi->input_stream_manager))->Handle_arrived_write_data((User_Request *)dma_req_item->object);
			break;
		default:
			break;
		}
		delete dma_req_item;
	}

	void Request_Fetch_Unit_CXL::Fetch_next_request(stream_id_type stream_id)
	{
		DMA_Req_Item *dma_req_item = new DMA_Req_Item;
		dma_req_item->Type = DMA_Req_Type::REQUEST_INFO;
		dma_req_item->object = (void *)(intptr_t)stream_id;
		dma_list.push_back(dma_req_item);

		Host_Interface_CXL *hi = (Host_Interface_CXL *)host_interface;
		Input_Stream_CXL *im = ((Input_Stream_CXL *)hi->input_stream_manager->input_streams[stream_id]);
		// host_interface->Send_read_message_to_host(im->Submission_queue_base_address + im->Submission_head * sizeof(Submission_Queue_Entry), sizeof(Submission_Queue_Entry));
	}

	void Request_Fetch_Unit_CXL::Fetch_write_data(User_Request *request)
	{
		DMA_Req_Item *dma_req_item = new DMA_Req_Item;
		dma_req_item->Type = DMA_Req_Type::WRITE_DATA;
		dma_req_item->object = (void *)request;
		dma_list.push_back(dma_req_item);

		Submission_Queue_Entry *sqe = (Submission_Queue_Entry *)request->IO_command_info;

		Process_pcie_read_message(0, NULL, 4096); // immediate send data

		// host_interface->Send_read_message_to_host((sqe->PRP_entry_2 << 31) | sqe->PRP_entry_1, request->Size_in_byte);
	}

	void Request_Fetch_Unit_CXL::Send_completion_queue_element(User_Request *request, uint16_t sq_head_value)
	{
		// Host_Interface_CXL* hi = (Host_Interface_CXL*)host_interface;
		// Completion_Queue_Entry* cqe = new Completion_Queue_Entry;
		// cqe->SQ_Head = sq_head_value;
		// cqe->SQ_ID = FLOW_ID_TO_Q_ID(request->Stream_id);
		// cqe->SF_P = 0x0001 & current_phase;
		// cqe->Command_Identifier = ((Submission_Queue_Entry*)request->IO_command_info)->Command_Identifier;
		// Input_Stream_CXL* im = ((Input_Stream_CXL*)hi->input_stream_manager->input_streams[request->Stream_id]);
		// host_interface->Send_write_message_to_host(im->Completion_queue_base_address + im->Completion_tail * sizeof(Completion_Queue_Entry), cqe, sizeof(Completion_Queue_Entry));
		// number_of_sent_cqe++;
		// if (number_of_sent_cqe % im->Completion_queue_size == 0)
		//{
		//	//According to protocol specification, the value of the Phase Tag is inverted each pass through the Completion Queue
		//	if (current_phase == 0xffff) {
		//		current_phase = 0xfffe;
		//	}
		//	else {
		//		current_phase = 0xffff;
		//	}
		// }
	}

	void Request_Fetch_Unit_CXL::Send_read_data(User_Request *request)
	{
		// Submission_Queue_Entry* sqe = (Submission_Queue_Entry*)request->IO_command_info;
		// host_interface->Send_write_message_to_host(sqe->PRP_entry_1, request->Data, request->Size_in_byte);
	}

	Host_Interface_CXL::Host_Interface_CXL(const sim_object_id_type &id,
										   LHA_type max_logical_sector_address, uint16_t submission_queue_depth, uint16_t completion_queue_depth,
										   unsigned int no_of_input_streams, uint16_t queue_fetch_size, unsigned int sectors_per_page, Data_Cache_Manager_Base *cache, CXL_DRAM_Model *cxl_dram) : Host_Interface_Base(id, HostInterface_Types::NVME, max_logical_sector_address, sectors_per_page, cache),
																																																   submission_queue_depth(submission_queue_depth), completion_queue_depth(completion_queue_depth), no_of_input_streams(no_of_input_streams), cxl_dram(cxl_dram)
	{
		this->input_stream_manager = new Input_Stream_Manager_CXL(this, queue_fetch_size);
		this->request_fetch_unit = new Request_Fetch_Unit_CXL(this);

		this->cxl_man = new CXL_Manager((Host_Interface_CXL *)this);
	}

	Host_Interface_CXL::~Host_Interface_CXL()
	{
		delete cxl_man;
		delete cxl_dram;
	}

	stream_id_type Host_Interface_CXL::Create_new_stream(IO_Flow_Priority_Class priority_class, LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
	uint64_t submission_queue_base_address, uint64_t completion_queue_base_address)
{
		return ((Input_Stream_Manager_CXL *)input_stream_manager)->Create_new_stream(priority_class, start_logical_sector_address, end_logical_sector_address, submission_queue_base_address, submission_queue_depth, completion_queue_base_address, completion_queue_depth);
	}

	void Host_Interface_CXL::Validate_simulation_config()
	{
		Host_Interface_Base::Validate_simulation_config();
		if (this->input_stream_manager == NULL)
		{
			throw std::logic_error("Input stream manager is not set for Host Interface");
		}
		if (this->request_fetch_unit == NULL)
		{
			throw std::logic_error("Request fetch unit is not set for Host Interface");
		}
	}

	void Host_Interface_CXL::Start_simulation()
	{
	}

	void Host_Interface_CXL::Execute_simulator_event(MQSimEngine::Sim_Event *event)
	{
		// this->request_fetch_unit->Process_pcie_read_message(0, NULL, 4096); //initiate when there is a write request

		// if (Simulator->Time() == 210411353) {
		//	cout << " Check" << endl;
		// }

		if (!cxl_man->cxl_config_para.has_mshr)
		{
			while (cxl_dram->getDRAMAvailability() > 0)
			{
				if (cxl_man->no_mshr_not_yet_serviced_lba.empty())
					break;
				uint64_t lba{cxl_man->no_mshr_not_yet_serviced_lba.front()};
				cxl_man->no_mshr_not_yet_serviced_lba.pop_front();
				no_mshr_record_node n{cxl_man->no_mshr_requests_record[lba].front()};
				cxl_man->no_mshr_requests_record[lba].pop_front();
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{static_cast<unsigned int>(64), lba, n.rw, CXL_DRAM_EVENTS::CACHE_MISS, n.time}};
				Send_request_to_CXL_DRAM(dram_request);

				list<uint64_t> *flush_lba{new list<uint64_t>};

				page_status_type sector_bitmap = ((Address_Mapping_Unit_Page_Level*) ((FTL*)(this->cxl_man->hi->cache->nvm_firmware))->Address_Mapping_Unit)->domains[0]->Get_page_status(true, 0 ,lba);
			
				//uint64_t cacheline_bitmap = this->cxl_man->sector_mask_to_cacheline_mask(sector_bitmap);

				uint64_t cacheline_bitmap = 0;
		//	uint64_t byte_id = first_entry->sqe->Reserved2;
			
			//uint64_t cacheline_idx = (byte_id % 4096) / 64;
					if (this->cxl_man->dram->cpara.only_cacheline_enable)
					{
						if (this->cxl_man->pending_page_fill.erase(lba) > 0) {
							this->cxl_man->dram->process_miss_data_ready_new(n.rw, lba, flush_lba, Simulator->Time(), this->cxl_man->prefetched_lba, this->cxl_man->tagAssertedLBA, this->cxl_man->prefetch_pollution_tracker, cxl_man->serviced_before_lba, cacheline_bitmap);
							this->cxl_man->drain_cacheline_eviction_history();
						}
						else {
							Fill_cachelines_from_bitmap(lba, flush_lba);
						}
					}
					else
					{
						this->cxl_man->dram->process_miss_data_ready_new(n.rw, lba, flush_lba, Simulator->Time(), this->cxl_man->prefetched_lba, this->cxl_man->tagAssertedLBA, this->cxl_man->prefetch_pollution_tracker, cxl_man->serviced_before_lba,cacheline_bitmap);
						this->cxl_man->drain_cacheline_eviction_history();

				}
				
				if (cxl_man->flash_back_end_access_count >= cxl_man->flash_back_end_queue_size)
				{
					Notify_CXL_Host_flash_not_full();
				}
				cxl_man->flash_back_end_access_count--;

				if (cxl_man->no_mshr_requests_record[lba].empty())
				{
					cxl_man->no_mshr_requests_record.erase(cxl_man->no_mshr_requests_record.find(lba));
				}

				while (!flush_lba->empty())
				{

					// ofFlush << static_cast<float>(cxl_man->total_number_of_accesses) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) * 100 << endl;

					cxl_man->flush_count++;
					uint64_t lba{flush_lba->front()};

					
					uint64_t lsa{lba * 8};

					flush_lba->pop_front();
					Submission_Queue_Entry *sqe{new Submission_Queue_Entry};
					sqe->Command_Identifier = INTERNAL_FLUSH_CMDID;
					sqe->Opcode = NVME_WRITE_OPCODE;

					sqe->Command_specific[0] = (uint32_t)lsa;
					sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
					uint64_t mask = ~0ULL;
					auto it = cxl_man->dram->write_map->find(lba);
					if (it != cxl_man->dram->write_map->end())
					{
						mask = it->second;
						cxl_man->dram->write_map->erase(it);
					}
					sqe->Command_specific[2] = ((uint32_t)((uint16_t)cxl_man->count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff);

					sqe->Command_specific[3] = 0; // flush  caued by read miss;
					uint64_t sector_bitmap = cxl_man->cacheline_mask_to_sector_mask(mask);
					sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
					sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
					sqe->Reserved = 0;
					sqe->PRP_entry_1 = (DATA_MEMORY_REGION);		  // Dummy addresses, just to emulate data read/write access
					sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000); // Dummy addresses
					((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Fetch_next_request(0);

					((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
				}

				delete flush_lba;
			}

			if (cxl_dram->getDRAMAvailability() > 0)
			{
				Notify_host_DRAM_is_free();
			}

			return;
		}

		while (cxl_dram->getDRAMAvailability() > 0)
		{
			if (cxl_man->serviced_before_lba.size() > 0)
			{
				uint64_t lba{*(cxl_man->serviced_before_lba.begin())};
				bool completely_removed{0};
				Update_CXL_DRAM_state_when_miss_data_ready(1, lba, 1, completely_removed);
				if (completely_removed)
				{
					cxl_man->serviced_before_lba.erase(cxl_man->serviced_before_lba.find(lba));
				}
			}
			else if (cxl_man->not_yet_serviced_lba.size() > 0)
			{
				uint64_t lba{*(cxl_man->not_yet_serviced_lba.begin())};
				bool completely_removed{0};
				Update_CXL_DRAM_state_when_miss_data_ready(1, lba, 0, completely_removed);
				cxl_man->not_yet_serviced_lba.erase(cxl_man->not_yet_serviced_lba.find(lba));
				if (!completely_removed)
				{
					cxl_man->serviced_before_lba.emplace(lba);
				}
			}

			if (cxl_man->serviced_before_lba.size() == 0 && cxl_man->not_yet_serviced_lba.size() == 0)
			{
				break;
			}
		}

		if (cxl_dram->getDRAMAvailability() > 0)
		{
			Notify_host_DRAM_is_free();
		}
	}

	uint16_t Host_Interface_CXL::Get_submission_queue_depth()
	{
		return submission_queue_depth;
	}

	uint16_t Host_Interface_CXL::Get_completion_queue_depth()
	{
		return completion_queue_depth;
	}

	void Host_Interface_CXL::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter &xmlwriter)
	{
		std::string tmp = name_prefix + ".HostInterface";
		xmlwriter.Write_open_tag(tmp);

		std::string attr = "Name";
		std::string val = ID();
		xmlwriter.Write_attribute_string(attr, val);

		for (unsigned int stream_id = 0; stream_id < no_of_input_streams; stream_id++)
		{
			std::string tmp = name_prefix + ".IO_Stream";
			xmlwriter.Write_open_tag(tmp);

			attr = "Stream_ID";
			val = std::to_string(stream_id);
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Read_Transaction_Turnaround_Time";
			val = std::to_string(input_stream_manager->Get_average_read_transaction_turnaround_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Read_Transaction_Execution_Time";
			val = std::to_string(input_stream_manager->Get_average_read_transaction_execution_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Read_Transaction_Transfer_Time";
			val = std::to_string(input_stream_manager->Get_average_read_transaction_transfer_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Read_Transaction_Waiting_Time";
			val = std::to_string(input_stream_manager->Get_average_read_transaction_waiting_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Write_Transaction_Turnaround_Time";
			val = std::to_string(input_stream_manager->Get_average_write_transaction_turnaround_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Write_Transaction_Execution_Time";
			val = std::to_string(input_stream_manager->Get_average_write_transaction_execution_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Write_Transaction_Transfer_Time";
			val = std::to_string(input_stream_manager->Get_average_write_transaction_transfer_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Write_Transaction_Waiting_Time";
			val = std::to_string(input_stream_manager->Get_average_write_transaction_waiting_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			xmlwriter.Write_close_tag();
		}

		xmlwriter.Write_close_tag();
	}
	void Host_Interface_CXL::print_prefetch_info()
	{
		cxl_man->finalize_granularity_timeline();
		std::cout << "Flush count: " << cxl_man->flush_count << endl;
		of_overall << "Flush count: " << cxl_man->flush_count << endl;
		std::cout << "Admission granularity count (1/2/4/8/16/32/64 cachelines): "
			<< cxl_man->admission_granularity_count[0] << ", "
			<< cxl_man->admission_granularity_count[1] << ", "
			<< cxl_man->admission_granularity_count[2] << ", "
			<< cxl_man->admission_granularity_count[3] << ", "
			<< cxl_man->admission_granularity_count[4] << ", "
			<< cxl_man->admission_granularity_count[5] << ", "
			<< cxl_man->admission_granularity_count[6] << endl;
		of_overall << "Admission granularity count (1/2/4/8/16/32/64 cachelines): "
			<< cxl_man->admission_granularity_count[0] << ", "
			<< cxl_man->admission_granularity_count[1] << ", "
			<< cxl_man->admission_granularity_count[2] << ", "
			<< cxl_man->admission_granularity_count[3] << ", "
			<< cxl_man->admission_granularity_count[4] << ", "
			<< cxl_man->admission_granularity_count[5] << ", "
			<< cxl_man->admission_granularity_count[6] << endl;
		std::cout << "Dynamic min load granularity count (1/2/4/8/16/32/64 cachelines): "
			<< cxl_man->dynamic_min_granularity_count[0] << ", "
			<< cxl_man->dynamic_min_granularity_count[1] << ", "
			<< cxl_man->dynamic_min_granularity_count[2] << ", "
			<< cxl_man->dynamic_min_granularity_count[3] << ", "
			<< cxl_man->dynamic_min_granularity_count[4] << ", "
			<< cxl_man->dynamic_min_granularity_count[5] << ", "
			<< cxl_man->dynamic_min_granularity_count[6] << endl;
		of_overall << "Dynamic min load granularity count (1/2/4/8/16/32/64 cachelines): "
			<< cxl_man->dynamic_min_granularity_count[0] << ", "
			<< cxl_man->dynamic_min_granularity_count[1] << ", "
			<< cxl_man->dynamic_min_granularity_count[2] << ", "
			<< cxl_man->dynamic_min_granularity_count[3] << ", "
			<< cxl_man->dynamic_min_granularity_count[4] << ", "
			<< cxl_man->dynamic_min_granularity_count[5] << ", "
			<< cxl_man->dynamic_min_granularity_count[6] << endl;
		double avg_dynamic_min_window_coverage = cxl_man->dynamic_min_decision_count == 0 ? 0.0 :
			static_cast<double>(cxl_man->dynamic_min_window_coverage_sum
				/ static_cast<long double>(cxl_man->dynamic_min_decision_count));
		std::cout << "Dynamic min load decision count: " << cxl_man->dynamic_min_decision_count << endl;
		of_overall << "Dynamic min load decision count: " << cxl_man->dynamic_min_decision_count << endl;
		std::cout << "Dynamic min load average window coverage: " << avg_dynamic_min_window_coverage << endl;
		of_overall << "Dynamic min load average window coverage: " << avg_dynamic_min_window_coverage << endl;
		std::cout << "Dynamic min load final window coverage: " << cxl_man->recent_window_average_page_coverage() << endl;
		of_overall << "Dynamic min load final window coverage: " << cxl_man->recent_window_average_page_coverage() << endl;
		double avg_admission_cachelines = cxl_man->admission_decision_count == 0 ? 0.0 :
			(double)cxl_man->admission_cacheline_total / (double)cxl_man->admission_decision_count;
		double avg_admission_backend_sectors = cxl_man->admission_decision_count == 0 ? 0.0 :
			(double)cxl_man->admission_backend_sector_total / (double)cxl_man->admission_decision_count;
		std::cout << "Admission decision count: " << cxl_man->admission_decision_count << endl;
		of_overall << "Admission decision count: " << cxl_man->admission_decision_count << endl;
		std::cout << "Admission average cachelines: " << avg_admission_cachelines << endl;
		of_overall << "Admission average cachelines: " << avg_admission_cachelines << endl;
		std::cout << "Admission backend sector total: " << cxl_man->admission_backend_sector_total << endl;
		of_overall << "Admission backend sector total: " << cxl_man->admission_backend_sector_total << endl;
		std::cout << "Admission average backend sectors: " << avg_admission_backend_sectors << endl;
		of_overall << "Admission average backend sectors: " << avg_admission_backend_sectors << endl;
		std::cout << "Admission inserted cachelines: " << cxl_man->admission_inserted_cacheline_count << endl;
		of_overall << "Admission inserted cachelines: " << cxl_man->admission_inserted_cacheline_count << endl;
		double admission_history_hit_ratio = cxl_man->admission_history_lookup_count == 0 ? 0.0 :
			(double)cxl_man->admission_history_hit_count / (double)cxl_man->admission_history_lookup_count;
		std::cout << "Admission history lookup count: " << cxl_man->admission_history_lookup_count << endl;
		of_overall << "Admission history lookup count: " << cxl_man->admission_history_lookup_count << endl;
		std::cout << "Admission history hit count: " << cxl_man->admission_history_hit_count << endl;
		of_overall << "Admission history hit count: " << cxl_man->admission_history_hit_count << endl;
		std::cout << "Admission history hit ratio: " << admission_history_hit_ratio << endl;
		of_overall << "Admission history hit ratio: " << admission_history_hit_ratio << endl;
		std::cout << "Admission history replace count: " << cxl_man->admission_history_replace_count << endl;
		of_overall << "Admission history replace count: " << cxl_man->admission_history_replace_count << endl;
		const double mib = 1024.0 * 1024.0;
		cxl_man->finalize_metadata_overhead_request_samples();
		double average_metadata_mb = cxl_man->average_metadata_overhead_request_sample_bytes() / mib;
		std::cout << "Cache metadata entry bytes: " << cxl_man->cxl_config_para.cache_metadata_entry_bytes << endl;
		of_overall << "Cache metadata entry bytes: " << cxl_man->cxl_config_para.cache_metadata_entry_bytes << endl;
		std::cout << "Metadata overhead average (MB): " << average_metadata_mb << endl;
		of_overall << "Metadata overhead average (MB): " << average_metadata_mb << endl;
		std::cout << "Managed cacheline entries (1/2/4/8/16/32/64): "
			<< cxl_man->dram->managed_cacheline_entry_count(1) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(2) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(4) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(8) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(16) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(32) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(64) << endl;
		of_overall << "Managed cacheline entries (1/2/4/8/16/32/64): "
			<< cxl_man->dram->managed_cacheline_entry_count(1) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(2) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(4) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(8) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(16) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(32) << ", "
			<< cxl_man->dram->managed_cacheline_entry_count(64) << endl;
			std::cout << "Ghost metadata average (MB): " << static_cast<double>(cxl_man->dram->ghost_metadata_bytes()) / mib << endl;
			of_overall << "Ghost metadata average (MB): " << static_cast<double>(cxl_man->dram->ghost_metadata_bytes()) / mib << endl;
			std::cout << "Admission policy: Resident+History Community Dynamic Admission + Metadata-aware Segment Coalescing" << endl;
			of_overall << "Admission policy: Resident+History Community Dynamic Admission + Metadata-aware Segment Coalescing" << endl;
			std::cout << "Simulated hash tables: 7 parallel granularity probes" << endl;
			of_overall << "Simulated hash tables: 7 parallel granularity probes" << endl;
			std::cout << "Hash metadata counted in capacity: 1" << endl;
			of_overall << "Hash metadata counted in capacity: 1" << endl;
			std::cout << "Segment metadata bytes incl. hash (1/2/4/8/16/32/64): 29, 29, 29, 30, 31, 33, 37" << endl;
			of_overall << "Segment metadata bytes incl. hash (1/2/4/8/16/32/64): 29, 29, 29, 30, 31, 33, 37" << endl;
			std::cout << "Community read miss count: " << cxl_man->community_read_miss_count << endl;
			of_overall << "Community read miss count: " << cxl_man->community_read_miss_count << endl;
			std::cout << "Community R empty count: " << cxl_man->community_r_empty_count << endl;
			of_overall << "Community R empty count: " << cxl_man->community_r_empty_count << endl;
			std::cout << "Community R nonempty count: " << cxl_man->community_r_nonempty_count << endl;
			of_overall << "Community R nonempty count: " << cxl_man->community_r_nonempty_count << endl;
			double avg_resident_cl = cxl_man->community_read_miss_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->community_resident_cl_sum / cxl_man->community_read_miss_count);
			double avg_resident_runs = cxl_man->community_read_miss_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->community_resident_run_sum / cxl_man->community_read_miss_count);
			double avg_candidates = cxl_man->community_read_miss_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->community_candidate_count) / static_cast<double>(cxl_man->community_read_miss_count);
			double avg_q_best = cxl_man->community_read_miss_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->community_q_best_sum / cxl_man->community_read_miss_count);
			std::cout << "Average resident CL count on read miss: " << avg_resident_cl << endl;
			of_overall << "Average resident CL count on read miss: " << avg_resident_cl << endl;
			std::cout << "Average resident run count on read miss: " << avg_resident_runs << endl;
			of_overall << "Average resident run count on read miss: " << avg_resident_runs << endl;
			std::cout << "Community total candidate count: " << cxl_man->community_candidate_count << endl;
			of_overall << "Community total candidate count: " << cxl_man->community_candidate_count << endl;
			std::cout << "Community average candidates per read miss: " << avg_candidates << endl;
			of_overall << "Community average candidates per read miss: " << avg_candidates << endl;
			std::cout << "Community accept count: " << cxl_man->community_accept_count << endl;
			of_overall << "Community accept count: " << cxl_man->community_accept_count << endl;
			std::cout << "Community fallback count: " << cxl_man->community_fallback_count << endl;
			of_overall << "Community fallback count: " << cxl_man->community_fallback_count << endl;
			std::cout << "Average Q_best: " << avg_q_best << endl;
			of_overall << "Average Q_best: " << avg_q_best << endl;
			std::cout << "Max Q_best: " << cxl_man->community_q_best_max << endl;
			of_overall << "Max Q_best: " << cxl_man->community_q_best_max << endl;
			std::cout << "Community logical size distribution 1..64: ";
			of_overall << "Community logical size distribution 1..64: ";
			for (int i = 1; i <= 64; i++) {
				std::cout << cxl_man->community_logical_size_count[i] << (i == 64 ? "" : ", ");
				of_overall << cxl_man->community_logical_size_count[i] << (i == 64 ? "" : ", ");
			}
			std::cout << endl;
			of_overall << endl;
			std::cout << "Merge attempt count: " << cxl_man->merge_attempt_count << endl;
			of_overall << "Merge attempt count: " << cxl_man->merge_attempt_count << endl;
			std::cout << "Merge success count: " << cxl_man->merge_success_count << endl;
			of_overall << "Merge success count: " << cxl_man->merge_success_count << endl;
			std::cout << "Merge reject count: " << cxl_man->merge_reject_count << endl;
			of_overall << "Merge reject count: " << cxl_man->merge_reject_count << endl;
			double merge_success_rate = cxl_man->merge_attempt_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->merge_success_count) / static_cast<double>(cxl_man->merge_attempt_count);
			std::cout << "Merge success rate: " << merge_success_rate << endl;
			of_overall << "Merge success rate: " << merge_success_rate << endl;
			std::cout << "Merge total absorbed segment entries: " << cxl_man->merge_total_absorbed_segment_entries << endl;
			of_overall << "Merge total absorbed segment entries: " << cxl_man->merge_total_absorbed_segment_entries << endl;
			double avg_absorbed = cxl_man->merge_success_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->merge_total_absorbed_segment_entries) / static_cast<double>(cxl_man->merge_success_count);
			std::cout << "Average absorbed entries per merge: " << avg_absorbed << endl;
			of_overall << "Average absorbed entries per merge: " << avg_absorbed << endl;
			std::cout << "Max absorbed entries per merge: " << cxl_man->merge_max_absorbed_segment_entries << endl;
			of_overall << "Max absorbed entries per merge: " << cxl_man->merge_max_absorbed_segment_entries << endl;
			std::cout << "Merge total metadata bytes saved: " << cxl_man->merge_total_metadata_bytes_saved << endl;
			of_overall << "Merge total metadata bytes saved: " << cxl_man->merge_total_metadata_bytes_saved << endl;
			std::cout << "Merge total hole CLs: " << cxl_man->merge_total_hole_cls << endl;
			of_overall << "Merge total hole CLs: " << cxl_man->merge_total_hole_cls << endl;
			std::cout << "Merge total hole bytes: " << cxl_man->merge_total_hole_bytes << endl;
			of_overall << "Merge total hole bytes: " << cxl_man->merge_total_hole_bytes << endl;
			std::cout << "Merge total gain bytes: " << cxl_man->merge_total_gain_bytes << endl;
			of_overall << "Merge total gain bytes: " << cxl_man->merge_total_gain_bytes << endl;
			double avg_merge_gain = cxl_man->merge_success_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->merge_total_gain_bytes) / static_cast<double>(cxl_man->merge_success_count);
			std::cout << "Average merge gain bytes: " << avg_merge_gain << endl;
			of_overall << "Average merge gain bytes: " << avg_merge_gain << endl;
			std::cout << "Max merge gain bytes: " << cxl_man->merge_max_gain_bytes << endl;
			of_overall << "Max merge gain bytes: " << cxl_man->merge_max_gain_bytes << endl;
			std::cout << "Merge to granularity count (1/2/4/8/16/32/64): "
				<< cxl_man->merge_to_granularity_count[0] << ", "
				<< cxl_man->merge_to_granularity_count[1] << ", "
				<< cxl_man->merge_to_granularity_count[2] << ", "
				<< cxl_man->merge_to_granularity_count[3] << ", "
				<< cxl_man->merge_to_granularity_count[4] << ", "
				<< cxl_man->merge_to_granularity_count[5] << ", "
				<< cxl_man->merge_to_granularity_count[6] << endl;
			of_overall << "Merge to granularity count (1/2/4/8/16/32/64): "
				<< cxl_man->merge_to_granularity_count[0] << ", "
				<< cxl_man->merge_to_granularity_count[1] << ", "
				<< cxl_man->merge_to_granularity_count[2] << ", "
				<< cxl_man->merge_to_granularity_count[3] << ", "
				<< cxl_man->merge_to_granularity_count[4] << ", "
				<< cxl_man->merge_to_granularity_count[5] << ", "
				<< cxl_man->merge_to_granularity_count[6] << endl;
			std::cout << "Demand read bytes: " << cxl_man->demand_read_bytes << endl;
			of_overall << "Demand read bytes: " << cxl_man->demand_read_bytes << endl;
			std::cout << "Community admission bytes: " << cxl_man->community_admission_bytes << endl;
			of_overall << "Community admission bytes: " << cxl_man->community_admission_bytes << endl;
			std::cout << "Hole fill bytes: " << cxl_man->hole_fill_bytes << endl;
			of_overall << "Hole fill bytes: " << cxl_man->hole_fill_bytes << endl;
			std::cout << "Actual backend read bytes: " << cxl_man->actual_backend_read_bytes << endl;
			of_overall << "Actual backend read bytes: " << cxl_man->actual_backend_read_bytes << endl;
			std::cout << "Total cache lookups: " << cxl_man->total_cache_lookups << endl;
			of_overall << "Total cache lookups: " << cxl_man->total_cache_lookups << endl;
			std::cout << "Parallel hash logical probes per lookup: 7" << endl;
			of_overall << "Parallel hash logical probes per lookup: 7" << endl;
			std::cout << "Hash hit count (1/2/4/8/16/32/64): "
				<< cxl_man->hash_hit_count[0] << ", "
				<< cxl_man->hash_hit_count[1] << ", "
				<< cxl_man->hash_hit_count[2] << ", "
				<< cxl_man->hash_hit_count[3] << ", "
				<< cxl_man->hash_hit_count[4] << ", "
				<< cxl_man->hash_hit_count[5] << ", "
				<< cxl_man->hash_hit_count[6] << endl;
			of_overall << "Hash hit count (1/2/4/8/16/32/64): "
				<< cxl_man->hash_hit_count[0] << ", "
				<< cxl_man->hash_hit_count[1] << ", "
				<< cxl_man->hash_hit_count[2] << ", "
				<< cxl_man->hash_hit_count[3] << ", "
				<< cxl_man->hash_hit_count[4] << ", "
				<< cxl_man->hash_hit_count[5] << ", "
				<< cxl_man->hash_hit_count[6] << endl;
			std::cout << "Multi-hash hit error count: " << cxl_man->multi_hash_hit_error_count << endl;
			of_overall << "Multi-hash hit error count: " << cxl_man->multi_hash_hit_error_count << endl;
			uint64_t segment_index_invariant_errors = cxl_man->dram->cacheline_dram == nullptr
				? 0 : cxl_man->dram->cacheline_dram->invariant_error_count();
			std::cout << "Segment index invariant error count: " << segment_index_invariant_errors << endl;
			of_overall << "Segment index invariant error count: " << segment_index_invariant_errors << endl;
			std::cout << "Total segment evictions: " << cxl_man->total_segment_evictions << endl;
			of_overall << "Total segment evictions: " << cxl_man->total_segment_evictions << endl;
			std::cout << "Total evicted CLs: " << cxl_man->total_evicted_cls << endl;
			of_overall << "Total evicted CLs: " << cxl_man->total_evicted_cls << endl;
			double avg_victim_segment_size = cxl_man->total_segment_evictions == 0 ? 0.0 :
				static_cast<double>(cxl_man->total_evicted_cls) / static_cast<double>(cxl_man->total_segment_evictions);
			std::cout << "Average victim segment size: " << avg_victim_segment_size << endl;
			of_overall << "Average victim segment size: " << avg_victim_segment_size << endl;
			std::cout << "Victim size distribution (1/2/4/8/16/32/64): "
				<< cxl_man->victim_granularity_count[0] << ", "
				<< cxl_man->victim_granularity_count[1] << ", "
				<< cxl_man->victim_granularity_count[2] << ", "
				<< cxl_man->victim_granularity_count[3] << ", "
				<< cxl_man->victim_granularity_count[4] << ", "
				<< cxl_man->victim_granularity_count[5] << ", "
				<< cxl_man->victim_granularity_count[6] << endl;
			of_overall << "Victim size distribution (1/2/4/8/16/32/64): "
				<< cxl_man->victim_granularity_count[0] << ", "
				<< cxl_man->victim_granularity_count[1] << ", "
				<< cxl_man->victim_granularity_count[2] << ", "
				<< cxl_man->victim_granularity_count[3] << ", "
				<< cxl_man->victim_granularity_count[4] << ", "
				<< cxl_man->victim_granularity_count[5] << ", "
				<< cxl_man->victim_granularity_count[6] << endl;
			std::cout << "Dirty segment eviction count: " << cxl_man->dirty_segment_eviction_count << endl;
			of_overall << "Dirty segment eviction count: " << cxl_man->dirty_segment_eviction_count << endl;
			std::cout << "Clean segment eviction count: " << cxl_man->clean_segment_eviction_count << endl;
			of_overall << "Clean segment eviction count: " << cxl_man->clean_segment_eviction_count << endl;
			std::cout << "Dirty writeback bytes: " << cxl_man->dirty_writeback_bytes << endl;
			of_overall << "Dirty writeback bytes: " << cxl_man->dirty_writeback_bytes << endl;
			std::cout << "Released data bytes: " << cxl_man->released_data_bytes << endl;
			of_overall << "Released data bytes: " << cxl_man->released_data_bytes << endl;
			std::cout << "Released metadata bytes: " << cxl_man->released_metadata_bytes << endl;
			of_overall << "Released metadata bytes: " << cxl_man->released_metadata_bytes << endl;
			double avg_dirty_victim_size = cxl_man->dirty_segment_eviction_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->dirty_writeback_bytes) / 64.0
				/ static_cast<double>(cxl_man->dirty_segment_eviction_count);
			std::cout << "Average dirty victim size: " << avg_dirty_victim_size << endl;
			of_overall << "Average dirty victim size: " << avg_dirty_victim_size << endl;
			double runtime_avg_metadata_bytes = cxl_man->runtime_metadata_sample_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->runtime_metadata_bytes_sum / cxl_man->runtime_metadata_sample_count);
			double runtime_avg_metadata_ratio = cxl_man->runtime_metadata_sample_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->runtime_metadata_ratio_sum / cxl_man->runtime_metadata_sample_count);
			double runtime_avg_segment_entries = cxl_man->runtime_metadata_sample_count == 0 ? 0.0 :
				static_cast<double>(cxl_man->runtime_segment_entry_count_sum / cxl_man->runtime_metadata_sample_count);
			std::cout << "Runtime metadata sample count: " << cxl_man->runtime_metadata_sample_count << endl;
			of_overall << "Runtime metadata sample count: " << cxl_man->runtime_metadata_sample_count << endl;
			std::cout << "Runtime metadata bytes average/min/max: "
				<< runtime_avg_metadata_bytes << ", "
				<< (cxl_man->runtime_metadata_sample_count == 0 ? 0 : cxl_man->runtime_metadata_bytes_min) << ", "
				<< cxl_man->runtime_metadata_bytes_max << endl;
			of_overall << "Runtime metadata bytes average/min/max: "
				<< runtime_avg_metadata_bytes << ", "
				<< (cxl_man->runtime_metadata_sample_count == 0 ? 0 : cxl_man->runtime_metadata_bytes_min) << ", "
				<< cxl_man->runtime_metadata_bytes_max << endl;
			std::cout << "Runtime metadata ratio average/min/max: "
				<< runtime_avg_metadata_ratio << ", "
				<< (cxl_man->runtime_metadata_sample_count == 0 ? 0.0 : cxl_man->runtime_metadata_ratio_min) << ", "
				<< cxl_man->runtime_metadata_ratio_max << endl;
			of_overall << "Runtime metadata ratio average/min/max: "
				<< runtime_avg_metadata_ratio << ", "
				<< (cxl_man->runtime_metadata_sample_count == 0 ? 0.0 : cxl_man->runtime_metadata_ratio_min) << ", "
				<< cxl_man->runtime_metadata_ratio_max << endl;
			std::cout << "Runtime segment entry count average/min/max: "
				<< runtime_avg_segment_entries << ", "
				<< (cxl_man->runtime_metadata_sample_count == 0 ? 0 : cxl_man->runtime_segment_entry_count_min) << ", "
				<< cxl_man->runtime_segment_entry_count_max << endl;
			of_overall << "Runtime segment entry count average/min/max: "
				<< runtime_avg_segment_entries << ", "
				<< (cxl_man->runtime_metadata_sample_count == 0 ? 0 : cxl_man->runtime_segment_entry_count_min) << ", "
				<< cxl_man->runtime_segment_entry_count_max << endl;
			std::cout << "Dynamic data capacity (cachelines): " << cxl_man->dram->dynamic_data_cacheline_capacity() << endl;
			of_overall << "Dynamic data capacity (cachelines): " << cxl_man->dram->dynamic_data_cacheline_capacity() << endl;
			std::cout << "Current data occupancy (cachelines): " << cxl_man->dram->total_data_cacheline_occupancy() << endl;
			of_overall << "Current data occupancy (cachelines): " << cxl_man->dram->total_data_cacheline_occupancy() << endl;
			if (cxl_man->dram->cacheline_dram != nullptr) {
				std::cout << "Clean-first eviction scan fraction: " << cxl_man->dram->cacheline_dram->clean_first_eviction_scan_fraction() << endl;
				of_overall << "Clean-first eviction scan fraction: " << cxl_man->dram->cacheline_dram->clean_first_eviction_scan_fraction() << endl;
				std::cout << "Clean-first eviction scan count: " << cxl_man->dram->cacheline_dram->clean_first_scan_count() << endl;
				of_overall << "Clean-first eviction scan count: " << cxl_man->dram->cacheline_dram->clean_first_scan_count() << endl;
				std::cout << "Clean-first eviction average scanned entries: " << cxl_man->dram->cacheline_dram->average_clean_first_scan_entries() << endl;
				of_overall << "Clean-first eviction average scanned entries: " << cxl_man->dram->cacheline_dram->average_clean_first_scan_entries() << endl;
				std::cout << "Clean-first eviction clean-page selections: " << cxl_man->dram->cacheline_dram->clean_page_eviction_selection_count() << endl;
				of_overall << "Clean-first eviction clean-page selections: " << cxl_man->dram->cacheline_dram->clean_page_eviction_selection_count() << endl;
				std::cout << "Clean-first eviction clean-segment selections: " << cxl_man->dram->cacheline_dram->clean_segment_eviction_selection_count() << endl;
				of_overall << "Clean-first eviction clean-segment selections: " << cxl_man->dram->cacheline_dram->clean_segment_eviction_selection_count() << endl;
				of_overall << "Replacement variant: " << MYWORK_REPLACEMENT_VARIANT << endl;
				of_overall << "Refill LRU policy: partial refill preserves age; merge inherits older position; full-range refill and demand hit refresh" << endl;
#if MYWORK_REPLACEMENT_VARIANT >= 2
				of_overall << "Eviction policy: clean unused tail pruning; at most two retained children; positive net bytes; whole-tail fallback" << endl;
#else
				of_overall << "Eviction policy: whole LRU tail only; page-coalesced writeback; unused reclamation disabled" << endl;
#endif
				of_overall << "Tail prune attempts: " << cxl_man->dram->cacheline_dram->tail_prune_attempts() << endl;
				of_overall << "Tail prune fragmentation rejects: " << cxl_man->dram->cacheline_dram->tail_prune_fragmentation_rejects() << endl;
				of_overall << "Tail prune nonpositive net rejects: " << cxl_man->dram->cacheline_dram->tail_prune_nonpositive_rejects() << endl;
				of_overall << "Tail prune reclaimed cachelines: " << cxl_man->dram->cacheline_dram->tail_prune_reclaimed_cl() << endl;
				of_overall << "Tail prune net freed bytes: " << cxl_man->dram->cacheline_dram->tail_prune_net_bytes() << endl;
				of_overall << "Unused CL reclamation events: " << cxl_man->dram->cacheline_dram->unused_reclamation_count() << endl;
				of_overall << "Clean-first whole clean Segment selections: " << cxl_man->dram->cacheline_dram->clean_whole_segment_selection_count() << endl;
				std::cout << "Clean-first eviction LRU fallback selections: " << cxl_man->dram->cacheline_dram->lru_fallback_eviction_selection_count() << endl;
				of_overall << "Clean-first eviction LRU fallback selections: " << cxl_man->dram->cacheline_dram->lru_fallback_eviction_selection_count() << endl;
			}
			std::cout << "Dynamic page cache capacity (pages): " << cxl_man->dram->dynamic_page_cache_capacity() << endl;
			of_overall << "Dynamic page cache capacity (pages): " << cxl_man->dram->dynamic_page_cache_capacity() << endl;
		std::cout << "Current page cache entries: " << cxl_man->dram->current_page_count() << endl;
		of_overall << "Current page cache entries: " << cxl_man->dram->current_page_count() << endl;
		double avg_host_response_us = 0.0;
		double avg_host_read_response_us = 0.0;
		double avg_host_write_response_us = 0.0;
		// if (cxl_man->host_request_latency_count > 0)
		// {
		// 	avg_host_response_us = (double)cxl_man->host_request_latency_sum / (double)cxl_man->host_request_latency_count / (double)SIM_TIME_TO_MICROSECONDS_COEFF;
		// }
		if (cxl_man->host_read_request_latency_count > 0)
		{
			avg_host_read_response_us = (double)cxl_man->host_read_request_latency_sum / (double)cxl_man->host_read_request_latency_count / (double)SIM_TIME_TO_MICROSECONDS_COEFF;
		}
		if (cxl_man->host_write_request_latency_count > 0)
		{
			avg_host_write_response_us = (double)cxl_man->host_write_request_latency_sum / (double)cxl_man->host_write_request_latency_count / (double)SIM_TIME_TO_MICROSECONDS_COEFF;
		}

		if (cxl_man->host_write_request_latency_count +cxl_man->host_read_request_latency_count > 0)
		{
			avg_host_response_us = (double)(cxl_man->host_write_request_latency_sum+cxl_man->host_read_request_latency_sum ) / (cxl_man->host_write_request_latency_count +cxl_man->host_read_request_latency_count)/(double)1000;
		}
		std::cout << "Average host request response time (us): " << avg_host_response_us << endl;
		of_overall << "Average host request response time (us): " << avg_host_response_us << endl;
		std::cout << "Average host read response time (us): " << avg_host_read_response_us << endl;
		of_overall << "Average host read response time (us): " << avg_host_read_response_us << endl;
		std::cout << "Average host write response time (us): " << avg_host_write_response_us << endl;
		of_overall << "Average host write response time (us): " << avg_host_write_response_us << endl;
		std::cout << "Eviction count: " << cxl_man->dram->eviction_count << endl;
		of_overall << "Eviction count: " << cxl_man->dram->eviction_count << endl;

		std::cout<<"average_access_interval: "<<(double)cxl_man->average_interval/1000/1000 / ((double)cxl_man->interval_count-1)<< " us" << endl;

		of_overall<<"average_access_interval: "<<(double)cxl_man->average_interval/1000/1000 / ((double)cxl_man->interval_count-1)<< " us" << endl;

		std::cout<<"read ratio: "<<(double)cxl_man->host_read_request_latency_count/(double)(cxl_man->host_read_request_latency_count+cxl_man->host_write_request_latency_count)<<endl;

		of_overall<<"read ratio: "<<(double)cxl_man->host_read_request_latency_count/(double)(cxl_man->host_read_request_latency_count+cxl_man->host_write_request_latency_count)<<endl;

		std::cout<<"total requests: "<< cxl_man->host_read_request_latency_count+cxl_man->host_write_request_latency_count<<std::endl;

		of_overall<<"total requests: "<< cxl_man->host_read_request_latency_count+cxl_man->host_write_request_latency_count<<std::endl;

		double avg_gc_page_movements = Stats::Total_gc_executions == 0 ? 0.0 :
			static_cast<double>(Stats::Total_page_movements_for_gc) / static_cast<double>(Stats::Total_gc_executions);
		std::cout << "GC count: " << Stats::GC_count << endl;
		of_overall << "GC count: " << Stats::GC_count << endl;
		std::cout << "GC executions: " << Stats::Total_gc_executions << endl;
		of_overall << "GC executions: " << Stats::Total_gc_executions << endl;
		std::cout << "GC page movements: " << Stats::Total_page_movements_for_gc << endl;
		of_overall << "GC page movements: " << Stats::Total_page_movements_for_gc << endl;
		std::cout << "Average GC page movements: " << avg_gc_page_movements << endl;
		of_overall << "Average GC page movements: " << avg_gc_page_movements << endl;


		std::cout << "Request ends at timestamp: " << static_cast<float>(Simulator->Time()) / 1000000000 << " s" << endl;
		of_overall << "Request ends at timestamp: " << static_cast<float>(Simulator->Time()) / 1000000000 << " s" << endl;
		std::cout << "Repeated flash access count: " << cxl_man->repeated_flash_access_count << endl;
		of_overall << "Repeated flash access count: " << cxl_man->repeated_flash_access_count << endl;
		if (PREFETCH_INFO_MAP.size() == 0)
			return;

		uint64_t accurate_prefetch{0};
		uint64_t late_prefetch{0};

		for (auto &i : PREFETCH_INFO_MAP)
		{
			if (i.second.hit_count > 0)
			{
				accurate_prefetch++;
				if (i.second.late)
				{
					late_prefetch++;
				}
			}
		}
		// uint64_t accurate_prefetch_sample{ 0 };
		// uint64_t late_prefetch_sample{ 0 };
		// for (auto& i : PREFETCH_INFO_MAP_SAMPLE) {
		//	if (i.second.hit_count > 0) {
		//		accurate_prefetch_sample++;
		//		if (i.second.late) {
		//			late_prefetch_sample++;
		//		}
		//	}
		// }
		/*of_overall << "Prefetch Coverage (sampled): " << static_cast<float>(cxl_man->prefetch_hit_count - LAST_PREFETCH) / static_cast<float>(43000000)<<endl;
		of_overall << "Prefetch Accuracy (sampled): " << static_cast<float>(accurate_prefetch_sample) / static_cast<float>(PREFETCH_INFO_MAP_SAMPLE.size()+1) << endl;
		of_overall << "Prefetch Lateness (sampled): " << static_cast<float>(late_prefetch_sample) / static_cast<float>(accurate_prefetch_sample+1) << endl;
		of_overall << "Prefetch Pollution (sampled): " << static_cast<float>(cxl_man->prefetch_pollution_count - LAST_POLLUTION) / static_cast<float>(cxl_man->cache_miss_count - LAST_CACHE_MISS +1) << endl;
		LAST_POLLUTION = cxl_man->prefetch_pollution_count;
		LAST_CACHE_MISS = cxl_man->cache_miss_count;
		LAST_PREFETCH = cxl_man->prefetch_hit_count;
		PREFETCH_INFO_MAP_SAMPLE.clear();*/

		std::cout << "Prefetch Coverage: " << static_cast<float>(cxl_man->prefetch_hit_count) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) << endl;
		of_overall << "Prefetch Coverage: " << static_cast<float>(cxl_man->prefetch_hit_count) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) << endl;
		std::cout << "Prefetch Accuracy: " << static_cast<float>(accurate_prefetch) / static_cast<float>(PREFETCH_INFO_MAP.size()) << endl;
		of_overall << "Prefetch Accuracy: " << static_cast<float>(accurate_prefetch) / static_cast<float>(PREFETCH_INFO_MAP.size()) << endl;
		std::cout << "Prefetch Lateness: " << static_cast<float>(late_prefetch) / static_cast<float>(accurate_prefetch) << endl;
		of_overall << "Prefetch Lateness: " << static_cast<float>(late_prefetch) / static_cast<float>(accurate_prefetch) << endl;
		std::cout << "Prefetch Pollution: " << static_cast<float>(cxl_man->prefetch_pollution_count) / static_cast<float>(cxl_man->cache_miss_count) << endl;
		of_overall << "Prefetch Pollution: " << static_cast<float>(cxl_man->prefetch_pollution_count) / static_cast<float>(cxl_man->cache_miss_count) << endl;
	}
	void Host_Interface_CXL::Fill_cachelines_from_bitmap(uint64_t lba, list<uint64_t>* flush_lba)
		{
			uint64_t bitmap = 0;
			auto it = cxl_man->pending_fill_bitmap.find(lba);
			if (it == cxl_man->pending_fill_bitmap.end()) {
				return;
			}

			bitmap = it->second;
			cxl_man->pending_fill_bitmap.erase(it);
			uint64_t used_bitmap = 0;
			auto used_it = cxl_man->pending_fill_used_bitmap.find(lba);
			if (used_it != cxl_man->pending_fill_used_bitmap.end()) {
				used_bitmap = used_it->second;
				cxl_man->pending_fill_used_bitmap.erase(used_it);
			}

			const uint64_t lines_per_page = cxl_man->cxl_config_para.ssd_page_size / 64;

				uint64_t remaining = bitmap;
				const uint64_t granularities[] = {64, 32, 16, 8, 4, 2, 1};
				for (uint64_t length : granularities) {
					for (uint64_t start = 0; start < lines_per_page; start += length) {
						uint64_t chunk_mask = length == 64 ? ~0ULL : ((1ULL << length) - 1) << start;
						if ((remaining & chunk_mask) != chunk_mask) {
							continue;
						}

						uint64_t missing_count = cacheline_count_from_mask(chunk_mask
							& ~cxl_man->dram->cacheline_dram->resident_mask_for_page(lba));

						auto static_evicted = cxl_man->dram->cacheline_dram->put_segment(lba, start, length, false);
						cxl_man->admission_inserted_cacheline_count += missing_count;
						if (static_evicted) {
							cxl_man->record_cacheline_eviction_history(*static_evicted);
						}
						if (static_evicted && static_evicted->dirty) {
							uint64_t dirty_mask = static_evicted->mask | cxl_man->dram->cacheline_dram->dirty_mask_for_page(static_evicted->lba);
							cxl_man->dram->cacheline_dram->mark_page_clean(static_evicted->lba);
							(*cxl_man->dram->write_map)[static_evicted->lba] |= dirty_mask;
							if (std::find(flush_lba->begin(), flush_lba->end(), static_evicted->lba) == flush_lba->end()) {
								flush_lba->push_back(static_evicted->lba);
							}
						}
						remaining &= ~chunk_mask;
					}
				}
				cxl_man->dram->cacheline_dram->mark_used_mask(lba, used_bitmap);
				cxl_man->dram->enforce_dynamic_cache_capacity(flush_lba);
				cxl_man->drain_cacheline_eviction_history();
			}

	void Host_Interface_CXL::Update_CXL_DRAM_state_when_miss_data_ready(bool rw, uint64_t lba, bool serviced_before, bool &completed_removed_from_mshr)
	{
		list<uint64_t> readcount, writecount;
		mshr_request *first_entry{NULL};
		bool wasfull{0};
		uint64_t dram_avail{this->cxl_dram->getDRAMAvailability()};
		bool completely_removed{0};

		if (this->cxl_man->mshr->isInProgress(lba))
		{
			first_entry = this->cxl_man->mshr->removeRequestNew(lba, readcount, writecount, wasfull, dram_avail, serviced_before, completely_removed);
			if (wasfull)
			{
				// notify cxl pcie  no longer full
				this->Notify_CXL_Host_mshr_not_full();
			}
		}
		else
		{
			completely_removed = 1; // for prefetched data
		}

		if (!serviced_before)
		{
			// CXL_DRAM_EVENTS evt{ CXL_DRAM_EVENTS::CACHE_MISS };
			// if (this->cxl_man->in_progress_prefetch_lba->count(lba)) {
			//	evt = CXL_DRAM_EVENTS::PREFETCH_READY;
			// }

			CXL_DRAM_EVENTS evt{CXL_DRAM_EVENTS::CACHE_MISS};
			if (first_entry == NULL)
			{ // disregard slow prefetch
				evt = CXL_DRAM_EVENTS::PREFETCH_READY;

				if (PREFETCH_INFO_MAP.count(lba) == 0)
				{
					prefetch_info_node n;
					PREFETCH_INFO_MAP.emplace(lba, n);
				}
				if (PREFETCH_INFO_MAP_SAMPLE.count(lba) == 0)
				{
					prefetch_info_node n;
					PREFETCH_INFO_MAP_SAMPLE.emplace(lba, n);
				}
			}
			else
			{
				if (this->cxl_man->in_progress_prefetch_lba->count(lba))
				{
					evt = CXL_DRAM_EVENTS::SLOW_PREFETCH;
					oflatep << first_entry->time << " " << Simulator->Time() << endl;
					if (PREFETCH_INFO_MAP.count(lba) == 0)
					{
						prefetch_info_node n;
						n.late = 1;
						n.hit_count++;
						PREFETCH_INFO_MAP.emplace(lba, n);
						PREFETCH_ACCURACY_INFO.emplace(lba);
						PREFETCH_LATE_INFO.emplace(lba);
					}
					else
					{
						PREFETCH_INFO_MAP[lba].late = 1;
						PREFETCH_INFO_MAP[lba].hit_count++;
						PREFETCH_ACCURACY_INFO.emplace(lba);
						PREFETCH_LATE_INFO.emplace(lba);
					}
					if (PREFETCH_INFO_MAP_SAMPLE.count(lba) == 0)
					{
						prefetch_info_node n;
						n.late = 1;
						n.hit_count++;
						PREFETCH_INFO_MAP_SAMPLE.emplace(lba, n);
					}
					else
					{
						PREFETCH_INFO_MAP_SAMPLE[lba].late = 1;
						PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
					}
				}
			}

			sim_time_type itime{0};
			if (first_entry == NULL)
			{
				itime = (*(this->cxl_man->in_progress_prefetch_lba))[lba];
			}
			else
			{

				itime = first_entry->time;
			}

			CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{static_cast<unsigned int>(64), lba, rw, evt, itime}};
			Send_request_to_CXL_DRAM(dram_request);

			if (first_entry)
				rw = (first_entry->sqe->Opcode == NVME_READ_OPCODE) ? 1 : 0;
			else
				rw = 1;

			list<uint64_t> *flush_lba{new list<uint64_t>};

			page_status_type sector_bitmap = ((Address_Mapping_Unit_Page_Level*) ((FTL*)(this->cxl_man->hi->cache->nvm_firmware))->Address_Mapping_Unit)->domains[0]->Get_page_status(true, 0 ,lba);

			//uint64_t cacheline_bitmap = this->cxl_man->sector_mask_to_cacheline_mask(sector_bitmap);
			uint64_t cacheline_bitmap = 0;
		//	uint64_t byte_id = first_entry->sqe->Reserved2;
			
				//uint64_t cacheline_idx = (byte_id % 4096) / 64;
				if (this->cxl_man->dram->cpara.only_cacheline_enable)
				{
					if (this->cxl_man->pending_page_fill.erase(lba) > 0) {
						this->cxl_man->dram->process_miss_data_ready_new(rw, lba, flush_lba, Simulator->Time(), this->cxl_man->prefetched_lba, this->cxl_man->tagAssertedLBA, this->cxl_man->prefetch_pollution_tracker, cxl_man->serviced_before_lba, cacheline_bitmap);
						this->cxl_man->drain_cacheline_eviction_history();
					}
					else {
						Fill_cachelines_from_bitmap(lba, flush_lba);
					}
				}
				else
				{
					this->cxl_man->dram->process_miss_data_ready_new(rw, lba, flush_lba, Simulator->Time(), this->cxl_man->prefetched_lba, this->cxl_man->tagAssertedLBA, this->cxl_man->prefetch_pollution_tracker, cxl_man->serviced_before_lba,cacheline_bitmap);
					this->cxl_man->drain_cacheline_eviction_history();
				}

			if (cxl_man->flash_back_end_access_count >= cxl_man->flash_back_end_queue_size)
			{
				Notify_CXL_Host_flash_not_full();
			}
			cxl_man->flash_back_end_access_count--;

			if (this->cxl_man->in_progress_prefetch_lba->count(lba))
			{
				this->cxl_man->in_progress_prefetch_lba->erase(this->cxl_man->in_progress_prefetch_lba->find(lba));
			}

			while (!flush_lba->empty())
			{

				// ofFlush << static_cast<float>(cxl_man->total_number_of_accesses) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) * 100 << endl;
				cxl_man->flush_count++;
				uint64_t lba{flush_lba->front()};
				uint64_t lsa{lba * 8};

				flush_lba->pop_front();
				Submission_Queue_Entry *sqe{new Submission_Queue_Entry};
				sqe->Command_Identifier = 0;
				sqe->Opcode = NVME_WRITE_OPCODE;
				sqe->Command_specific[0] = (uint32_t)lsa;
				sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
				uint64_t mask = ~0ULL;
				auto it = cxl_man->dram->write_map->find(lba);
				if (it != cxl_man->dram->write_map->end())
				{
					mask = it->second;
					cxl_man->dram->write_map->erase(it);
				}
				sqe->Command_specific[2] = ((uint32_t)((uint16_t)cxl_man->count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff);
				sqe->Command_specific[3] = 0; // flush  caued by read miss;
				uint64_t sector_bitmap = cxl_man->cacheline_mask_to_sector_mask(mask);
				sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
				sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
				sqe->Reserved = 0;
				sqe->PRP_entry_1 = (DATA_MEMORY_REGION);		  // Dummy addresses, just to emulate data read/write access
				sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000); // Dummy addresses

				((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Fetch_next_request(0);

				((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
			}

			delete flush_lba;
			delete first_entry;
		}

		list<uint64_t>* flush_lba = new list<uint64_t>;

		if (serviced_before && this->cxl_man->dram->cpara.only_cacheline_enable) {
			if (this->cxl_man->pending_page_fill.erase(lba) > 0) {
				this->cxl_man->dram->process_miss_data_ready_new(rw, lba, flush_lba, Simulator->Time(), this->cxl_man->prefetched_lba, this->cxl_man->tagAssertedLBA, this->cxl_man->prefetch_pollution_tracker, cxl_man->serviced_before_lba, 0);
				this->cxl_man->drain_cacheline_eviction_history();
			}
			else if (this->cxl_man->pending_fill_bitmap.count(lba) > 0) {
				Fill_cachelines_from_bitmap(lba, flush_lba);
			}
		}
		while (!flush_lba->empty())
			{

				// ofFlush << static_cast<float>(cxl_man->total_number_of_accesses) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) * 100 << endl;
				cxl_man->flush_count++;
				uint64_t lba{flush_lba->front()};
				uint64_t lsa{lba * 8};

				flush_lba->pop_front();
				Submission_Queue_Entry *sqe{new Submission_Queue_Entry};
				sqe->Command_Identifier = 0;
				sqe->Opcode = NVME_WRITE_OPCODE;
				sqe->Command_specific[0] = (uint32_t)lsa;
				sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
				uint64_t mask = ~0ULL;
				auto it = cxl_man->dram->write_map->find(lba);
				if (it != cxl_man->dram->write_map->end())
				{
					mask = it->second;
					cxl_man->dram->write_map->erase(it);
				}
				sqe->Command_specific[2] = ((uint32_t)((uint16_t)cxl_man->count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff);
				sqe->Command_specific[3] = 0; // flush  caued by read miss;
				uint64_t sector_bitmap = cxl_man->cacheline_mask_to_sector_mask(mask);
				sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
				sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
				sqe->Reserved = 0;
				sqe->PRP_entry_1 = (DATA_MEMORY_REGION);		  // Dummy addresses, just to emulate data read/write access
				sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000); // Dummy addresses

				((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Fetch_next_request(0);

				((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
			}

		delete flush_lba;


		for (auto i : readcount)
		{
			CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, 1, CXL_DRAM_EVENTS::CACHE_HIT_UNDER_MISS, i}};
			this->Send_request_to_CXL_DRAM(dram_request);

			bool falsehit{0};
			Update_CXL_DRAM_state(1, lba, falsehit, (~0ULL));
			if (falsehit)
				cxl_man->falsehitcount++;
			if (cxl_man->prefetched_lba->count(lba))
			{
				PREFETCH_INFO_MAP[lba].hit_count++;
				PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
				PREFETCH_ACCURACY_INFO.emplace(lba);
			}
		}

		for (auto i : writecount)
		{
			CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, 0, CXL_DRAM_EVENTS::CACHE_HIT_UNDER_MISS, i}};
			this->Send_request_to_CXL_DRAM(dram_request);

			bool falsehit{0};
			Update_CXL_DRAM_state(0, lba, falsehit, (~0ULL));
			if (falsehit)
				cxl_man->falsehitcount++;

			if (cxl_man->prefetched_lba->count(lba))
			{
				PREFETCH_INFO_MAP[lba].hit_count++;
				PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
				PREFETCH_ACCURACY_INFO.emplace(lba);
			}
		}
		completed_removed_from_mshr = completely_removed;
	}

	void Host_Interface_CXL::process_CXL_prefetch_requests(list<uint64_t> prefetchlba)
	{

		while (!prefetchlba.empty())
		{
			uint64_t lba{prefetchlba.front()};

			prefetchlba.pop_front();

			Submission_Queue_Entry *sqe = new Submission_Queue_Entry;
			sqe->Command_Identifier = 0;
			sqe->Opcode = NVME_READ_OPCODE;
			sqe->Command_specific[0] = (uint32_t)(lba * cxl_man->cxl_config_para.num_sec); // cxl_man->process_requests will do a translation
			sqe->Command_specific[1] = (uint32_t)((lba * cxl_man->cxl_config_para.num_sec) >> 32);
			sqe->Command_specific[2] = ((uint32_t)((uint16_t)cxl_man->cxl_config_para.num_sec)) & (uint32_t)(0x0000ffff); // magic number
			sqe->PRP_entry_1 = (DATA_MEMORY_REGION);																	  // Dummy addresses, just to emulate data read/write access
			sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000);															  // Dummy addresses

			cxl_man->in_progress_prefetch_lba->emplace(lba, Simulator->Time());
			request_fetch_unit->Fetch_next_request(0);

			request_fetch_unit->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
		}
	}

}
