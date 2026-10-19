#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

struct TraceEntry {
    uint64_t time;
    uint64_t addr;
    int rw_bit;
};

static bool parse_raw_line(const string& line, string& rw, uint64_t& addr, double& timestamp)
{
    if (line.size() < 3 || line[0] != '[' || (line[1] != 'R' && line[1] != 'W')) {
        return false;
    }

    string cleaned = line.substr(1, line.size() - 2);
    stringstream ss(cleaned);
    ss >> rw >> hex >> addr >> fixed >> setprecision(9) >> timestamp;
    return !rw.empty() && !ss.fail();
}

int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3) {
        cerr << "Usage: " << argv[0] << " input.vout|input.pout [output.trace]\n";
        return 1;
    }

    string input_name = argv[1];
    string output_name;
    if (argc == 3) {
        output_name = argv[2];
    } else {
        output_name = input_name;
        size_t dot = output_name.find_last_of('.');
        if (dot == string::npos) {
            output_name += ".trace";
        } else {
            output_name.replace(dot, string::npos, ".trace");
        }
    }

    ifstream input(input_name);
    if (!input.is_open()) {
        cerr << "File does not exist: " << input_name << "\n";
        return 1;
    }

    constexpr uint64_t scaler = 1000 / 90;
    bool have_start = false;
    uint64_t global_start_time = 0;
    uint64_t read_count = 0;
    set<uint64_t> footprint_pages;
    vector<TraceEntry> entries;

    string line;
    while (getline(input, line)) {
        string rw;
        uint64_t addr = 0;
        double timestamp = 0;
        if (!parse_raw_line(line, rw, addr, timestamp)) {
            continue;
        }

        uint64_t time = static_cast<uint64_t>(timestamp * 1000000);
        if (!have_start || time < global_start_time) {
            global_start_time = time;
            have_start = true;
        }

        int rw_bit = (rw == "R") ? 1 : 0;
        if (rw_bit == 1) {
            read_count++;
        }
        footprint_pages.insert(addr / 4096);
        entries.push_back({time, addr, rw_bit});

        if (entries.size() % 10000000 == 0) {
            cout << "Read " << entries.size() << " accesses\n";
        }
    }

    stable_sort(entries.begin(), entries.end(), [](const TraceEntry& a, const TraceEntry& b) {
        return a.time < b.time;
    });

    ofstream output(output_name);
    if (!output.is_open()) {
        cerr << "Cannot open output: " << output_name << "\n";
        return 1;
    }

    for (const auto& entry : entries) {
        output << (entry.time - global_start_time) * scaler
               << " 0 " << entry.addr << " 64 " << entry.rw_bit << '\n';
    }

    cout << "Translated accesses: " << entries.size() << '\n';
    cout << "Memory Footprint: " << footprint_pages.size() * 4096 / 1024 / 1024 << "MB\n";
    cout << "Read counts: " << read_count << " Write counts: " << entries.size() - read_count << '\n';
    cout << "Output: " << output_name << '\n';
    return 0;
}
