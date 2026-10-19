#pragma once
#include <cstdint>
#include <limits>
#include <string>
namespace SSD_Components {
class Cacheline_Optimal_Oracle {
public:
 static constexpr uint64_t NEVER=std::numeric_limits<uint64_t>::max();
 static bool LoadTrace(const std::string&,uint64_t,uint64_t,uint64_t,uint64_t);
 static bool IsReady();
 static void ConsumeAccess(uint64_t);
 static uint64_t NextUse(uint64_t);
 static uint64_t Window();
 static uint64_t Consumed();
 static uint64_t Expected();
};
}
