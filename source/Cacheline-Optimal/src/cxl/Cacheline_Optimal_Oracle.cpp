#include "Cacheline_Optimal_Oracle.h"
#include "../host/ASCII_Trace_Definition.h"
#include "../utils/StringTools.h"
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
namespace SSD_Components {
namespace {
std::ifstream input;
std::string path;
std::unordered_map<uint64_t,std::deque<uint64_t>> positions;
std::deque<uint64_t> order;
uint64_t expected=0,consumed=0,loaded=0,window=0,sectors=0,first_lsa=0,last_lsa=0;
bool ready=false;
void reopen() {
 input.close();input.clear();input.open(path);
 if(!input) throw std::runtime_error("Cannot open oracle trace: "+path);
#if defined(OLD_TRACE)
 std::string header;for(int i=0;i<3;++i) std::getline(input,header);
#endif
}
uint64_t read_key() {
 std::string line;
 if(!std::getline(input,line)) {reopen();if(!std::getline(input,line)) throw std::runtime_error("Empty trace");}
 if(line.empty()) throw std::runtime_error("Empty oracle record");
 Utils::Helper_Functions::Remove_cr(line);
 std::vector<std::string> f;Utils::Helper_Functions::Tokenize(line,ASCIILineDelimiter,f);
 if(f.size()!=ASCIIItemsPerLine || (f[ASCIITraceTypeColumn]!=ASCIITraceWriteCode && f[ASCIITraceTypeColumn]!=ASCIITraceReadCode))
  throw std::runtime_error("Invalid oracle trace record "+std::to_string(loaded));
 char* end=nullptr;uint64_t address=std::strtoull(f[ASCIITraceAddressColumn].c_str(),&end,0);
 if(end==f[ASCIITraceAddressColumn].c_str()) throw std::runtime_error("Invalid oracle address");
 while(end && *end && std::isspace(static_cast<unsigned char>(*end))) ++end;
 if(!end || *end) throw std::runtime_error("Invalid oracle address");
 uint64_t bytes=sectors*512,page=address/bytes,lsa=page*sectors+(address%bytes)/512;
 if(lsa<first_lsa || lsa>last_lsa) {
  if(last_lsa<=first_lsa) throw std::runtime_error("Invalid LSA range");
  lsa=first_lsa+lsa%(last_lsa-first_lsa);page=lsa/sectors;
 }
 return page*(bytes/64)+((lsa%sectors)*512+address%512)/64;
}
void refill() {
 const uint64_t target=consumed+std::min(expected-consumed,window+1);
 while(loaded<target) {uint64_t key=read_key();positions[key].push_back(loaded);order.push_back(key);++loaded;}
}
}
bool Cacheline_Optimal_Oracle::LoadTrace(const std::string& p,uint64_t count,uint64_t sec,uint64_t start,uint64_t end) {
 ready=false;positions.clear();order.clear();consumed=loaded=0;
 expected=count;path=p;sectors=sec;first_lsa=start;last_lsa=end;
 // Artifact policy: always enabled; fixed for every workload and cache size.
 window=512ULL*1024;
 if(!sectors || !expected)
  throw std::runtime_error("Invalid bounded cacheline oracle parameters");
 positions.reserve(static_cast<size_t>(std::min(expected,window+1)));
 reopen();refill();ready=true;
 std::cout<<"Cacheline optimal oracle ready: expected "<<expected<<" accesses; lookahead window "<<window
          <<" requests; key=global_cacheline_id; bounded streaming"<<std::endl;
 return true;
}
bool Cacheline_Optimal_Oracle::IsReady(){return ready;}
void Cacheline_Optimal_Oracle::ConsumeAccess(uint64_t key) {
 if(!ready) return;
 if(consumed>=expected || order.empty() || order.front()!=key)
  throw std::runtime_error("Cacheline oracle/host order mismatch at "+std::to_string(consumed));
 auto it=positions.find(key);
 if(it==positions.end() || it->second.empty() || it->second.front()!=consumed) throw std::runtime_error("Oracle cursor mismatch");
 it->second.pop_front();if(it->second.empty()) positions.erase(it);
 order.pop_front();++consumed;refill();
}
uint64_t Cacheline_Optimal_Oracle::NextUse(uint64_t key) {
 auto it=positions.find(key);
 if(!ready || it==positions.end() || it->second.empty()) return NEVER;
 uint64_t next=it->second.front();return next-consumed<window?next:NEVER;
}
uint64_t Cacheline_Optimal_Oracle::Window(){return window;}
uint64_t Cacheline_Optimal_Oracle::Consumed(){return consumed;}
uint64_t Cacheline_Optimal_Oracle::Expected(){return expected;}
}
