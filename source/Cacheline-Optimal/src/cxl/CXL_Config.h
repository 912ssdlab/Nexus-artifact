#pragma once
#include<iostream>
#include<fstream>
#include <cstdint>

using namespace std;

typedef enum class cachepolicy {
	random,
	lru2,
	fifo,
	lfu,
	lru,
	cflru,
	lrfu,//not working
	cpu//no need of it
}cachepolicy;

typedef enum class prefetchertype {
	no,
	tagged,
	bo,
	stms,
	leap,
	readahead,
	feedback_direct
}prefetchertype;


class cxl_config {
public:
	static constexpr uint64_t default_cache_metadata_payload_bytes{ 16 };
	static constexpr uint64_t default_cache_metadata_lru_link_bytes{ 8 };
	static constexpr uint64_t default_cache_metadata_entry_bytes{
		default_cache_metadata_payload_bytes + default_cache_metadata_lru_link_bytes
	};

	uint64_t dram_size;
	bool mix_mode{1};
	uint64_t cache_portion_size;
	uint64_t current_size;
	uint64_t cacheline_cache_size;
	uint64_t prefetch_portion_size;
	uint64_t ssd_page_size;
	uint64_t logical_sector_size{ 512 };
	cachepolicy cpolicy;
	cachepolicy pref_cpolicy;
	double lrfu_p;
	double lrfu_lambda;
	uint64_t set_associativity;
	prefetchertype prefetch_policy;
	uint64_t total_number_of_requets;
	bool has_mshr;
	bool has_cache;
	bool only_cacheline_enable{1};
	bool dram_mode{ 0 };
	uint64_t num_sec{ 8 };
	uint64_t cache_metadata_entry_bytes{ default_cache_metadata_entry_bytes };

	double cacheline_dram_ratio;

	cxl_config() {
		dram_size = 0; 
		cache_portion_size = 0;
		current_size = 0;
		prefetch_portion_size = 0;
		cacheline_cache_size = 0;
		ssd_page_size = 4096;
		cpolicy = cachepolicy::random;
		pref_cpolicy = cachepolicy::random;
		lrfu_p = 2;
		lrfu_lambda = 1;
		set_associativity = 4;
		prefetch_policy = prefetchertype::tagged;
		total_number_of_requets = 0;
		has_mshr = 1;
		has_cache = 1;
		double cacheline_dram_ratio = 0;
	};
	void readConfigFile();
};
