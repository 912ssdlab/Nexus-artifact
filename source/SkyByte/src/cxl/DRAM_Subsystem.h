#ifndef DRAM_SUBSYSTEM_H
#define DRAM_SUBSYSTEM_H

#include <iostream>
#include <cstdint>

#include <set>
#include <map>
#include <vector>
#include <list>
#include "CXL_Config.h"
#include "FlashTypes.h"
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

#include <iostream>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <optional>

class LRUSet {
public:
    struct Entry {
        uint64_t key;
        bool dirty;
    };

    struct EvictedEntry {
        uint64_t key;
        bool dirty;
    };

    struct HashPageState {
    uint64_t line_count;
    uint64_t bucket_count; // initial 4, then 8/16/32/64
    };

    std::unordered_map<uint64_t, HashPageState> hash_pages_;
    uint64_t hash_index_bytes_{0};
    uint64_t lines_per_page_{64};
    long double hash_index_sample_sum_bytes_{0};
    uint64_t hash_index_sample_count_{0};
    uint64_t max_hash_index_bytes_{0};

public:
    explicit LRUSet(std::size_t capacity)
        : capacity_(capacity) {
        map_.reserve(capacity_);
    }

    bool setDirty(uint64_t addr, bool dirty) 
    {
        auto it = map_.find(addr);
        if (it == map_.end()) {
            return false;
        }

        it->second->dirty = dirty;
        return true;
    }

    void remove(uint64_t addr) 
    {
       
        auto it = map_.find(addr);
        if (it == map_.end()) {
            return;
        }

        removeHashIndex(addr);

        map_.erase(it);
    }

    uint64_t index_bytes() const {
    return hash_index_bytes_;
}

    double average_index_bytes() const {
        if (hash_index_sample_count_ == 0) {
            return static_cast<double>(hash_index_bytes_);
        }
        return static_cast<double>(hash_index_sample_sum_bytes_ / hash_index_sample_count_);
    }

    uint64_t max_index_bytes() const {
        return max_hash_index_bytes_;
    }

    uint64_t index_sample_count() const {
        return hash_index_sample_count_;
    }

    bool Is_Dirty(uint64_t addr) 
    {
        auto it = map_.find(addr);
        if (it == map_.end()) {
            return false;
        }
        return it->second->dirty;
    }


    // SkyByte-style write log: every write appends a new physical log entry.
    // map_ always points to the newest live version of each cacheline.
    std::optional<EvictedEntry> put(uint64_t key, bool dirty = false) {
        auto it = map_.find(key);
        bool first_live_version = (it == map_.end());

        items_.push_back(Entry{key, dirty});
        auto newest = std::prev(items_.end());
        map_[key] = newest;

        if (first_live_version) {
            addHashIndex(key);
        }

        auto evicted = evictUntilWithinCapacity();
        recordHashIndexSample();
        return evicted;
    }

    // touch is ignored intentionally: reads probe the latest hash entry and do
    // not reorder the circular write log.
    bool contains(uint64_t key, bool touch = true) {
        (void)touch;
        return map_.find(key) != map_.end();
    }

    // 标记为脏
    bool markDirty(uint64_t key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }

        it->second->dirty = true;
        return true;
    }

    // 标记为干净
    bool markClean(uint64_t key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }

        it->second->dirty = false;
        return true;
    }

    // 查询某个 key 是否是脏的
    std::optional<bool> isDirty(uint64_t key) const {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }

        return it->second->dirty;
    }

    // Keep the old name for callers, but this now reclaims the write-log head.
    std::optional<EvictedEntry> popLast() {
        while (!items_.empty()) {
            auto head = items_.begin();
            Entry entry = *head;
            auto live = map_.find(entry.key);

            if (live == map_.end() || live->second != head) {
                items_.pop_front();
                continue;
            }

            removeHashIndex(entry.key);
            map_.erase(live);
            items_.pop_front();
            return EvictedEntry{entry.key, entry.dirty};
        }

        return std::nullopt;
    }

    std::size_t size() const {
        return map_.size();
    }

    bool empty() const {
        return map_.empty();
    }

private:
    void addHashIndex(uint64_t key) {
        LPA_type lpa = key / lines_per_page_;
        auto page_it = hash_pages_.find(lpa);

        if (page_it == hash_pages_.end())
        {
            hash_pages_.emplace(lpa, HashPageState{0, 4});
            hash_index_bytes_ += 16; // 1st-level hash entry
            hash_index_bytes_ += 16; // 2nd-level hash table: 4 * 4B
            page_it = hash_pages_.find(lpa);
        }

        page_it->second.line_count++;
        if (page_it->second.line_count > page_it->second.bucket_count * 3 / 4) {
            hash_index_bytes_ += page_it->second.bucket_count * 4;
            page_it->second.bucket_count *= 2;
        }
    }

    void removeHashIndex(uint64_t key) {
        uint64_t page_id = key / lines_per_page_;
        auto page_it = hash_pages_.find(page_id);
        if (page_it == hash_pages_.end()) {
            return;
        }

        if (page_it->second.line_count > 0) {
            page_it->second.line_count--;
        }

        if (page_it->second.line_count == 0) {
            hash_index_bytes_ -= 16;
            hash_index_bytes_ -= page_it->second.bucket_count * 4;
            hash_pages_.erase(page_it);
        }
        recordHashIndexSample();
    }

    std::optional<EvictedEntry> evictUntilWithinCapacity() {
        std::optional<EvictedEntry> evicted;
        while (items_.size() > capacity_) {
            auto candidate = popLast();
            if (candidate && !evicted) {
                evicted = candidate;
            }
        }
        return evicted;
    }

    void recordHashIndexSample() {
        hash_index_sample_sum_bytes_ += hash_index_bytes_;
        hash_index_sample_count_++;
        if (hash_index_bytes_ > max_hash_index_bytes_) {
            max_hash_index_bytes_ = hash_index_bytes_;
        }
    }

    std::size_t capacity_;

    std::list<Entry> items_;

    std::unordered_map<
        uint64_t,
        std::list<Entry>::iterator
    > map_;
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
		uint64_t dynamic_page_cache_capacity() const;
		bool ensure_page_cache_capacity(list<uint64_t>* flush_lba, uint64_t simtime, set<uint64_t>* prefetched_lba,
			set<uint64_t>& taggedAddr, set<uint64_t>& prefetch_pollution_tracker, set<uint64_t> not_finished,
			uint64_t trigger_lba, uint64_t pages_to_insert);
		map<uint64_t, uint64_t>* record_access_dram_dirty_bitmap{ nullptr };
        map<uint64_t, uint64_t>* write_map{ nullptr };
        
		LRUSet* cacheline_dram;
        LRUSet* page_dram;

	private:
		cxl_config cpara;
		map<uint64_t, uint64_t>* dram_mapping{ NULL }; // LBA, cache line index
		map<uint64_t, uint64_t>* dram_valid_bitmap{ nullptr }; // lba -> valid CL mask
        map<uint64_t, uint64_t>* dram_dirty_bitmap{ nullptr }; // lba -> dirty CL mask
		void evict_one_page_full_assoc(list<uint64_t>* flush_lba, uint64_t simtime, set<uint64_t>* prefetched_lba,
			set<uint64_t>& taggedAddr, set<uint64_t>& prefetch_pollution_tracker, set<uint64_t> not_finished,
			uint64_t trigger_lba);
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

        uint64_t hash_reserved_pages() const {
        uint64_t bytes = cacheline_dram->index_bytes();
        return (bytes + cpara.ssd_page_size - 1) / cpara.ssd_page_size;
    }

    uint64_t total_page_slots() const {
    return cpara.cache_portion_size / cpara.ssd_page_size;
}
uint64_t usable_page_slots() const {
    uint64_t reserved = hash_reserved_pages();
    uint64_t total = total_page_slots();
    return reserved >= total ? 0 : total - reserved;
}

	};


}


#endif
