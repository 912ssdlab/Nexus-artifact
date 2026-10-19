#pragma once
#include <iostream>
#include <cstdint>

#include <fstream>

using namespace std;
class outlog {
public:
	ofstream of;
	outlog(string filename);
	~outlog();
};

//extern outlog outputf;
extern outlog evictf;
extern ofstream of_overall;
extern ofstream oflate;
extern ofstream ofsus_mshr;
extern ofstream ofsus_flash;
void reset_overall_log(const std::string& trace_name);
void reset_trace_result_logs(const std::string& trace_name);
