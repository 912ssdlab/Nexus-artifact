#pragma once
#include <cstdint>
#include <limits>
#include <list>
#include <optional>
#include <set>
#include <unordered_map>
// Oracle-only host bookkeeping, not simulated deployable metadata.
class CachelineOptimalIndex {
 static constexpr uint64_t NEVER=std::numeric_limits<uint64_t>::max();
 struct Node {uint64_t next;std::list<uint64_t>::iterator far;};
 std::unordered_map<uint64_t,Node> nodes;
 std::list<uint64_t> far_lru;
 std::set<std::pair<uint64_t,uint64_t>> near;
public:
 void erase(uint64_t key) {
  auto it=nodes.find(key);if(it==nodes.end()) return;
  if(it->second.next==NEVER) far_lru.erase(it->second.far);else near.erase({it->second.next,key});
  nodes.erase(it);
 }
 void refresh(uint64_t key,uint64_t next) {
  erase(key);
  if(next==NEVER) {far_lru.push_front(key);nodes.emplace(key,Node{next,far_lru.begin()});}
  else {near.emplace(next,key);nodes.emplace(key,Node{next,{}});}
 }
 template<class Lookup> std::optional<uint64_t> victim(Lookup lookup) {
  while(!far_lru.empty()) {uint64_t key=far_lru.back(),next=lookup(key);if(next==NEVER) return key;refresh(key,next);}
  if(!near.empty()) return near.rbegin()->second;
  return std::nullopt;
 }
};
