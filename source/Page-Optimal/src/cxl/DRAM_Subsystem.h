#ifndef DRAM_SUBSYSTEM_H
#define DRAM_SUBSYSTEM_H

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

public:
    explicit LRUSet(std::size_t capacity)
        : capacity_(capacity) {
        map_.reserve(capacity_);
    }

    // 插入或访问
    bool setDirty(uint64_t addr, bool dirty) 
    {
        auto it = map_.find(addr);
        if (it == map_.end()) {
            return false;
        }

        it->second->dirty = dirty;
        return true;
    }

    // dirty 表示这次插入/访问是否会把 cacheline 标记为脏
    std::optional<EvictedEntry> put(uint64_t key, bool dirty = false) {
        auto it = map_.find(key);

        // 已存在：刷新到链表头部
        if (it != map_.end()) {
            auto list_it = it->second;

            // 如果这次是写操作，把 dirty 置为 true
            if (dirty) {
                list_it->dirty = true;
            }

            items_.splice(items_.begin(), items_, list_it);
            return std::nullopt;
        }

        // 新插入
        items_.push_front(Entry{key, dirty});
        map_[key] = items_.begin();

        // 超出容量，弹出最后一个
        if (items_.size() > capacity_) {
            return popLast();
        }

        return std::nullopt;
    }

    // 查找
    // touch=true 表示命中后刷新为最近使用
    bool contains(uint64_t key, bool touch = true) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }

        if (touch) {
            items_.splice(items_.begin(), items_, it->second);
        }

        return true;
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

    // 弹出最后一个，也就是最久未使用的元素
    std::optional<EvictedEntry> popLast() {
        if (items_.empty()) {
            return std::nullopt;
        }

        Entry last = items_.back();

        map_.erase(last.key);
        items_.pop_back();

        return EvictedEntry{last.key, last.dirty};
    }

    std::size_t size() const {
        return items_.size();
    }

    bool empty() const {
        return items_.empty();
    }

private:
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
		map<uint64_t, uint64_t>* record_access_dram_dirty_bitmap{ nullptr };
        map<uint64_t, uint64_t>* write_map{ nullptr };
        
		LRUSet* cacheline_dram;
        LRUSet* page_dram;

	private:
		cxl_config cpara;
		map<uint64_t, uint64_t>* dram_mapping{ NULL }; // LBA, cache line index
		map<uint64_t, uint64_t>* dram_valid_bitmap{ nullptr }; // lba -> valid CL mask
        map<uint64_t, uint64_t>* dram_dirty_bitmap{ nullptr }; // lba -> dirty CL mask
		
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

	};


}


#endif
