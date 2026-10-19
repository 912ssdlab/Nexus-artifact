#include"CXL_Config.h"






void cxl_config::readConfigFile() {

	ifstream configfile{ "config.txt" };


	while (!configfile.eof()) {
		string info;

		configfile >> info;

		if (info == "DRAM_size") {
			uint64_t value;
			configfile >> dec >> value;
			dram_size = value;
		}
		else if (info == "DRAM_mode") {
			uint64_t value;
			configfile >> dec >> value;
			dram_mode = (bool)value;
		}
		else if (info == "Has_cache") {
			uint64_t value;
			configfile >> dec >> value;
			has_cache = value;
		}
		else if (info == "Mix_mode") {
			uint64_t value;
			configfile >> dec >> value;
			mix_mode = static_cast<bool>(value);
		}
			else if (info == "Cache_portion_percentage") {
				uint64_t value;
				configfile >> dec >> value;
				cacheline_cache_size = dram_size * value / 100;
				cache_portion_size = only_cacheline_enable ? 0 : cacheline_cache_size - 8388608;
				current_size = cache_portion_size;
				//cacheline_cache_size = 40265318;
				prefetch_portion_size = 0;
			}
		else if (info == "Has_mshr") {
			uint64_t value;
			configfile >> dec >> value;
			has_mshr = static_cast<bool>(value);
		}
		else if (info == "SSD_page_size") {
			uint64_t value;
			configfile >> dec >> value;
			ssd_page_size = value;
		}
		else if (info == "Cache_policy") {
			string policy;
			configfile >> policy;
			if (policy == "Random") {
				cpolicy = cachepolicy::random;
			}
			else if (policy == "LRU2") {
				cpolicy = cachepolicy::lru2;
			}
			else if (policy == "FIFO") {
				cpolicy = cachepolicy::fifo;
			}
			else if (policy == "LFU") {
				cpolicy = cachepolicy::lfu;
			}
			else if (policy == "LRFU") {
				cpolicy = cachepolicy::lrfu;
			}
			else if (policy == "LRU") {
				cpolicy = cachepolicy::lru;
			}
			else if (policy == "CFLRU") {
				cpolicy = cachepolicy::cflru;
			}
			else if (policy == "CPU") {
				cpolicy = cachepolicy::cpu;
			}
		}
		else if (info == "Prefetch_cache_policy") {
			string policy;
			configfile >> policy;
			if (policy == "Random") {
				pref_cpolicy = cachepolicy::random;
			}
			else if (policy == "LRU2") {
				pref_cpolicy = cachepolicy::lru2;
			}
			else if (policy == "LRFU") {
				pref_cpolicy = cachepolicy::lrfu;
			}
			else if (policy == "CPU") {
				pref_cpolicy = cachepolicy::cpu;
			}
		}
		else if (info == "LRFU_p_lambda") {
			double p, lambda;
			configfile >> p >> lambda;
			lrfu_p = p;
			lrfu_lambda = lambda;
		}
		else if (info == "Cache_placement") {
				uint64_t sa;
				configfile >> sa;
				if (sa == 1)
				{
					set_associativity = cache_portion_size == 0 ? 1 : cache_portion_size / ssd_page_size;
				}
				else set_associativity = sa;
		}
		else if (info == "Prefetcher") {
			string ptype;
			configfile >> ptype;
			if (ptype == "No") {
				prefetch_policy = prefetchertype::no;
			}
			else if (ptype == "Tagged") {
				prefetch_policy = prefetchertype::tagged;
			}
			else if (ptype == "Best-offset") {
				prefetch_policy = prefetchertype::bo;
			}
			else if (ptype == "STMS") {
				prefetch_policy = prefetchertype::stms;
			}
			else if (ptype == "Leap") {
				prefetch_policy = prefetchertype::leap;
			}
			else if (ptype == "Readahead") {
				prefetch_policy = prefetchertype::readahead;

			}
			else if (ptype == "Feedback_direct") {
				prefetch_policy = prefetchertype::feedback_direct;

			}

		}
		else if (info == "Total_number_of_requests") {
			uint64_t value{ 0 };

			configfile >> value;

			total_number_of_requets = value;
		}
		else if (info == "Admission_history_entry_count" || info == "Ghost_entry_count") {
			uint64_t value{ 0 };

			configfile >> value;

			admission_history_entry_count = value;
		}
		else if (info == "Cache_metadata_entry_bytes"
			|| info == "Cacheline_metadata_entry_bytes"
			|| info == "Data_cache_metadata_entry_bytes") {
			uint64_t value{ 0 };

			configfile >> value;

			cache_metadata_entry_bytes = value;
		}
		else if (info == "Admission_history_size_bytes" || info == "Ghost_size_bytes") {
			uint64_t value{ 0 };

			configfile >> value;

			admission_history_entry_count = value == 0
				? 0
				: (value + admission_history_entry_bytes - 1) / admission_history_entry_bytes;
		}

	}
	configfile.close();
}
