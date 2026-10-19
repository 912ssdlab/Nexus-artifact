#pragma once
#include <iostream>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <set>

using namespace std;

class LRUNode {
public:
	uint64_t page_num{ 0 };
	bool dirty{ 0 };
	LRUNode* left{ NULL };
	LRUNode* right{ NULL };
	uint64_t oracle_next_use{ 0 };
	bool oracle_tracked{ false };
	bool oracle_far{ false };
	std::list<uint64_t>::iterator oracle_far_position;
};

class CFLRU {
public:
	CFLRU() {};
	CFLRU(uint64_t cache_size);
	~CFLRU();
	void add(uint64_t pn);
	void add_optimal(uint64_t pn, const std::function<uint64_t(uint64_t)>& next_use);
	uint64_t pop();
	uint64_t pop_optimal(const std::function<uint64_t(uint64_t)>& next_use);
	void modify(uint64_t pn, bool dirty);
	void modify_optimal(uint64_t pn, bool dirty, const std::function<uint64_t(uint64_t)>& next_use);
	private:
	void track_optimal(LRUNode* node, uint64_t next_use);
	void untrack_optimal(LRUNode* node);
	uint64_t remove_node(LRUNode* node);
	uint64_t window{ 0 };
	LRUNode* head{ NULL };
	LRUNode* tail{ NULL };
	map<uint64_t, LRUNode*> m;
	std::list<uint64_t> optimal_far_lru;
	std::set<std::pair<uint64_t, uint64_t>> optimal_near_order;



};
