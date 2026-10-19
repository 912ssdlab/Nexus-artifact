#include "OutputLog.h"
#include <cstdlib>
#include <string>

//outlog outputf{ "output.txt" };
//outlog evictf{ "evictionlog.txt" };
ofstream of_overall{};
extern ofstream oflate;
extern ofstream oflatep;
extern ofstream oflat_no_cache;
extern ofstream ofsus_mshr;
extern ofstream ofsus_flash;

namespace
{
	bool detailed_latency_log_enabled()
	{
		const char* value = std::getenv("CXL_ENABLE_LATENCY_LOG");
		if (value == nullptr) {
			return false;
		}

		std::string flag{value};
		return flag == "1" || flag == "true" || flag == "TRUE" ||
			flag == "yes" || flag == "YES" || flag == "on" || flag == "ON";
	}
}

void reset_overall_log(const std::string& trace_name)
{
	if (of_overall.is_open()) {
		of_overall.close();
	}

	std::string filename = "./Results/overall.txt";
	if (!trace_name.empty()) {
		filename = "./Results/" + trace_name + "_overall.txt";
	}

	of_overall.open(filename);
}

void reset_trace_result_logs(const std::string& trace_name)
{
	auto close_stream = [](ofstream& stream) {
		if (stream.is_open()) {
			stream.close();
		}
	};

	close_stream(oflate);
	close_stream(oflatep);
	close_stream(oflat_no_cache);

	if (!detailed_latency_log_enabled()) {
		return;
	}

	auto open_stream = [&](ofstream& stream, const std::string& suffix) {

		std::string filename = "./Results/" + suffix;
		if (!trace_name.empty()) {
			filename = "./Results/" + trace_name + "_" + suffix;
		}

		stream.open(filename);
	};

	open_stream(oflate, "latency_result.txt");
	open_stream(oflatep, "late_prefetch_lateness.txt");
	open_stream(oflat_no_cache, "latency_results_no_cache.txt");
	// Stall-time traces are very large and disabled by default.
	// Leave ofsus_mshr/ofsus_flash unopened so writes become no-ops.
}


outlog::outlog(string filename) {
	if (!of.is_open()) {
		of.open(filename);
	}
}

outlog::~outlog() {
	if (of.is_open()) {
		of.close();
	}
}
