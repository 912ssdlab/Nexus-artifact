#include "CFLRU.h"

#include <limits>

CFLRU::CFLRU(uint64_t cache_size) {
	if (cache_size >= 4096) {
		window = 4096;
	}
	else {
		window = cache_size / 2;
	}
}
CFLRU::~CFLRU() {
	optimal_far_lru.clear();
	optimal_near_order.clear();
	while (head != NULL && head != tail) {
		LRUNode* n = head->right;
		delete head;
		head = n;
	}
	if (head != NULL) delete head;
	m.clear();
}

void CFLRU::add(uint64_t pn) {
	LRUNode* nnode{ new LRUNode };
	nnode->page_num = pn;

	if (head == NULL) {
		head = nnode;
		tail = nnode;
		m.emplace(pn, nnode);
		return;
	}

	nnode->right = head;
	head->left = nnode;
	head = nnode;

	m.emplace(pn, nnode);
}

void CFLRU::track_optimal(LRUNode* node, uint64_t next_use) {
	node->oracle_next_use = next_use;
	node->oracle_tracked = true;
	node->oracle_far = next_use == std::numeric_limits<uint64_t>::max();
	if (node->oracle_far) {
		optimal_far_lru.push_front(node->page_num);
		node->oracle_far_position = optimal_far_lru.begin();
	}
	else {
		optimal_near_order.emplace(next_use, node->page_num);
	}
}

void CFLRU::untrack_optimal(LRUNode* node) {
	if (!node->oracle_tracked) {
		return;
	}
	if (node->oracle_far) {
		optimal_far_lru.erase(node->oracle_far_position);
	}
	else {
		optimal_near_order.erase({node->oracle_next_use, node->page_num});
	}
	node->oracle_tracked = false;
	node->oracle_far = false;
}

void CFLRU::add_optimal(uint64_t pn, const std::function<uint64_t(uint64_t)>& next_use) {
	add(pn);
	auto it = m.find(pn);
	if (it != m.end()) {
		track_optimal(it->second, next_use(pn));
	}
}

uint64_t CFLRU::remove_node(LRUNode* target) {
	if (target == NULL) {
		return 0;
	}

	untrack_optimal(target);
	uint64_t evict_addr = target->page_num;
	if (target == head) {
		head = target->right;
		if (head != NULL) {
			head->left = NULL;
		}
		else {
			tail = NULL;
		}
	}
	else if (target == tail) {
		tail = target->left;
		if (tail != NULL) {
			tail->right = NULL;
		}
		else {
			head = NULL;
		}
	}
	else {
		target->left->right = target->right;
		target->right->left = target->left;
	}
	m.erase(evict_addr);
	delete target;
	return evict_addr;
}

uint64_t CFLRU::pop() {
	LRUNode* p{ tail }, * target{ NULL };
	uint64_t evict_addr{ 0 };

	for (auto i = 0; i < window; i++) {
		if (!p->dirty) {
			target = p;
			break;
		}
		p = p->left;
	}

	if (target == NULL) {
		target = tail;
	}
	return remove_node(target);
}

uint64_t CFLRU::pop_optimal(const std::function<uint64_t(uint64_t)>& next_use) {
	LRUNode* target{ NULL };

	// Entries leave the far queue lazily when their next use moves into the
	// bounded lookahead window. Each transition is paid for only once.
	while (!optimal_far_lru.empty()) {
		uint64_t page_num = optimal_far_lru.back();
		auto node_it = m.find(page_num);
		if (node_it == m.end()) {
			optimal_far_lru.pop_back();
			continue;
		}

		LRUNode* candidate = node_it->second;
		uint64_t candidate_next_use = next_use(page_num);
		if (candidate_next_use == std::numeric_limits<uint64_t>::max()) {
			target = candidate;
			break;
		}

		untrack_optimal(candidate);
		track_optimal(candidate, candidate_next_use);
	}

	if (target == NULL && !optimal_near_order.empty()) {
		auto victim = optimal_near_order.rbegin();
		auto node_it = m.find(victim->second);
		if (node_it != m.end()) {
			target = node_it->second;
		}
	}
	if (target == NULL) {
		target = tail;
	}
	return remove_node(target);
}

void CFLRU::modify(uint64_t pn, bool dirty) {
	LRUNode* target{ m.find(pn)->second };

	if (target != head) {
		if (target == tail) {
			if (!target->dirty) target->dirty = dirty;
			tail = target->left;
			tail->right = NULL;

			head->left = target;
			target->right = head;
			target->left = NULL;
			head = target;
			return;
		}

		head->left = target;
		target->left->right = target->right;
		target->right->left = target->left;
		if (!target->dirty) target->dirty = dirty;
		target->right = head;
		target->left = NULL;
		head = target;
		return;
	}

	if (!target->dirty) target->dirty = dirty;
	return;
}

void CFLRU::modify_optimal(uint64_t pn, bool dirty, const std::function<uint64_t(uint64_t)>& next_use) {
	auto it = m.find(pn);
	if (it == m.end()) {
		return;
	}
	LRUNode* target = it->second;
	untrack_optimal(target);
	modify(pn, dirty);
	track_optimal(target, next_use(pn));
}
