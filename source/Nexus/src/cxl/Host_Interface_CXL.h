#ifndef HOST_INTERFACE_CXL_H
#define HOST_INTERFACE_CXL_H

#include <cstdint>

#include <vector>
#include <array>
#include <deque>
#include <fstream>
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <string>

#include "../sim/Sim_Event.h"
#include "../ssd/Host_Interface_Base.h"
#include "../ssd/User_Request.h"
#include "../ssd/Host_Interface_Defs.h"
#include "DRAM_Subsystem.h"
#include "CXL_Config.h"
#include "CXL_MSHR.h"
#include "DRAM_Model.h"
#include "OutputLog.h"
#include "Prefetching_Alg.h"

using namespace std;
class no_mshr_record_node {
public:
	uint64_t time{0};
	bool rw{ 1 };
};
namespace SSD_Components
{


	class CXL_Manager {
		struct AdmissionHistoryEntry {
			uint64_t tag{ UINT64_MAX };
			uint64_t bitmap{ 0 };
		};
		struct RecentWindowPageCoverage {
			std::array<uint32_t, 64> cacheline_count{};
			uint64_t unique_cachelines{ 0 };
		};
		struct GranularityFutureEvent {
			uint64_t page{ 0 };
			uint64_t ordinal{ 0 };
			uint8_t cacheline{ 0 };
			bool admission{ false };
			uint8_t dynamic_min{ 0 };
			uint8_t logical_selected{ 0 };
			uint8_t newly_admitted{ 0 };
			uint8_t backend_sectors{ 0 };
		};
		struct GranularityFuturePage {
			std::array<uint16_t, 64> cacheline_count{};
			uint64_t active_mask{ 0 };
		};
		struct GranularityFutureBin {
			uint64_t all_access_count{ 0 };
			uint64_t admission_count{ 0 };
			uint64_t future_active_all_sum{ 0 };
			uint64_t future_active_admission_sum{ 0 };
			uint64_t dynamic_min_sum{ 0 };
			uint64_t logical_selected_sum{ 0 };
			uint64_t newly_admitted_sum{ 0 };
			uint64_t backend_sector_sum{ 0 };
		};

	public:

		CXL_Manager(Host_Interface_Base* hosti);
		~CXL_Manager();
		
		bool process_requests(uint64_t address, void* payload, bool is_pref_req, uint64_t* evcit_lba);
		int count_sectors_from_cacheline_mask(uint64_t mask);
		uint64_t cacheline_mask_to_sector_mask(uint64_t cl_mask);
		uint64_t sector_mask_to_cacheline_mask(uint64_t sec_mask);
		void record_admission_eviction(uint64_t lba, uint64_t used_bitmap);
		void record_cacheline_eviction_history(const LRUSet::EvictedSegment& evicted);
		void drain_cacheline_eviction_history();
		uint64_t get_admission_history_mask(uint64_t lba);
		void record_recent_access_coverage(uint64_t lba, uint64_t cacheline_idx);
		double recent_window_average_page_coverage() const;
		uint64_t dynamic_min_load_granularity() const;
		void record_granularity_timeline_access(uint64_t trace_page,
			uint64_t trace_cacheline);
		void record_granularity_timeline_admission(uint64_t dynamic_min,
			uint64_t logical_selected_cachelines, uint64_t selected_cachelines,
			uint64_t requested_new_cachelines,
			uint64_t backend_sectors);
		void evaluate_granularity_future_front();
		void write_granularity_future_alignment();
		void flush_granularity_timeline_bin();
		void finalize_granularity_timeline();
		void record_metadata_overhead_request();
		void finalize_metadata_overhead_request_samples();
		double average_metadata_overhead_request_sample_bytes() const;

		void request_serviced(User_Request* request);



		cxl_mshr* mshr;

		dram_subsystem* dram;

			uint64_t falsehitcount{ 0 };
				std::map<uint64_t, uint64_t> pending_fill_bitmap;
				std::map<uint64_t, uint64_t> pending_fill_used_bitmap;
				std::set<uint64_t> pending_page_fill;
				bool cache_initialization_done{ false };

			//prefetchers

		void prefetch_decision_maker(uint64_t lba, bool isMiss, uint64_t prefetch_hit_count);
		int prefetch_feedback();
		double accuracy_high{ 0.75 }, accuracy_low{ 0.4 }, late_thresh{ 0.01 }, pollution_thresh{ 0.25 };

		set<uint64_t>* prefetched_lba;
		map<uint64_t, uint64_t>* in_progress_prefetch_lba;
		uint64_t prefetch_queue_size{ 1024 };

		//tagged prefetcher
		uint64_t previous_unused_lba{0};

		set<uint64_t> tagAssertedLBA;
		uint16_t prefetchK{ 4 };
		uint16_t prefetch_timing_offset{16};
		int prefetch_level{ 4 };
		vector<vector<uint64_t>> prefetch_aggre{ {1,4}, {1,8},{2,16},{4,32},{4,64} };

		//Best-offset prefetcher
		boClass boPrefetcher;

		//leap
		leapClass leapPrefetcher;

		//for dram avaialable scheduling
		set<uint64_t> serviced_before_lba;

		set<uint64_t> not_yet_serviced_lba;

		cxl_config cxl_config_para;
		uint64_t cache_miss_count{ 0 }, cache_hit_count{ 0 }, total_number_of_accesses{ 0 }, prefetch_hit_count{ 0 }, flush_count{ 0 }, flash_read_count{ 0 }, no_cache_flash_write_count{ 0 }, no_cache_flash_read_count{0};
		uint64_t admission_granularity_count[7]{ 0 };
		uint64_t admission_decision_count{ 0 };
		uint64_t admission_cacheline_total{ 0 };
		uint64_t admission_backend_sector_total{ 0 };
		uint64_t admission_inserted_cacheline_count{ 0 };
		std::list<AdmissionHistoryEntry> admission_history_lru;
		std::unordered_map<uint64_t, std::list<AdmissionHistoryEntry>::iterator> admission_history_map;
		uint64_t admission_history_lookup_count{ 0 };
		uint64_t admission_history_hit_count{ 0 };
		uint64_t admission_history_replace_count{ 0 };
		uint64_t community_read_miss_count{ 0 };
		uint64_t community_r_empty_count{ 0 };
		uint64_t community_r_nonempty_count{ 0 };
		uint64_t community_candidate_count{ 0 };
		uint64_t community_accept_count{ 0 };
		uint64_t community_fallback_count{ 0 };
		std::array<uint64_t, 65> community_logical_size_count{};
		long double community_resident_cl_sum{ 0 };
		long double community_resident_run_sum{ 0 };
		long double community_q_best_sum{ 0 };
		double community_q_best_max{ 0 };
		uint64_t merge_attempt_count{ 0 };
		uint64_t merge_success_count{ 0 };
		uint64_t merge_reject_count{ 0 };
		uint64_t merge_to_granularity_count[7]{ 0 };
		uint64_t merge_total_absorbed_segment_entries{ 0 };
		uint64_t merge_max_absorbed_segment_entries{ 0 };
		uint64_t merge_total_metadata_bytes_saved{ 0 };
		uint64_t merge_total_hole_cls{ 0 };
		uint64_t merge_total_hole_bytes{ 0 };
		int64_t merge_total_gain_bytes{ 0 };
		int64_t merge_max_gain_bytes{ 0 };
		uint64_t demand_read_bytes{ 0 };
		uint64_t community_admission_bytes{ 0 };
		uint64_t hole_fill_bytes{ 0 };
		uint64_t actual_backend_read_bytes{ 0 };
		uint64_t total_cache_lookups{ 0 };
		uint64_t hash_hit_count[7]{ 0 };
		uint64_t multi_hash_hit_error_count{ 0 };
		uint64_t total_segment_evictions{ 0 };
		uint64_t total_evicted_cls{ 0 };
		uint64_t victim_granularity_count[7]{ 0 };
		uint64_t dirty_segment_eviction_count{ 0 };
		uint64_t clean_segment_eviction_count{ 0 };
		uint64_t dirty_writeback_bytes{ 0 };
		uint64_t released_data_bytes{ 0 };
		uint64_t released_metadata_bytes{ 0 };
		uint64_t runtime_metadata_sample_count{ 0 };
		long double runtime_metadata_bytes_sum{ 0 };
		uint64_t runtime_metadata_bytes_min{ UINT64_MAX };
		uint64_t runtime_metadata_bytes_max{ 0 };
		long double runtime_metadata_ratio_sum{ 0 };
		double runtime_metadata_ratio_min{ 0 };
		double runtime_metadata_ratio_max{ 0 };
		long double runtime_segment_entry_count_sum{ 0 };
		uint64_t runtime_segment_entry_count_min{ UINT64_MAX };
		uint64_t runtime_segment_entry_count_max{ 0 };
		std::deque<std::pair<uint64_t, uint64_t>> recent_access_window;
		std::unordered_map<uint64_t, RecentWindowPageCoverage> recent_window_page_coverage;
		uint64_t recent_window_unique_cacheline_total{ 0 };
		uint64_t dynamic_min_granularity_count[7]{ 0 };
		uint64_t dynamic_min_decision_count{ 0 };
		long double dynamic_min_window_coverage_sum{ 0 };
		bool granularity_timeline_enabled{ false };
		bool granularity_timeline_finalized{ false };
		uint64_t granularity_timeline_window_requests{ 1000000 };
		uint64_t granularity_timeline_total_requests{ 0 };
		uint64_t granularity_timeline_bin_index{ 0 };
		uint64_t granularity_timeline_bin_request_begin{ 0 };
		uint64_t granularity_timeline_bin_requests{ 0 };
		uint64_t granularity_timeline_bin_admissions{ 0 };
		// Accumulate coverage as fixed-point values.  A one-million-request
		// timeline bin executes this update in a very hot path; fixed-point
		// accumulation is deterministic and avoids long-double state being
		// polluted by unrelated simulator state on long traces.
		uint64_t granularity_timeline_access_coverage_scaled_sum{ 0 };
		uint64_t granularity_timeline_admission_coverage_scaled_sum{ 0 };
		std::array<uint64_t, 65> granularity_timeline_selected_size_count{};
		std::array<uint64_t, 65> granularity_timeline_logical_selected_size_count{};
		std::array<uint64_t, 65> granularity_timeline_requested_new_size_count{};
		std::array<uint64_t, 65> granularity_timeline_dynamic_min_size_count{};
		uint64_t granularity_timeline_invalid_size_count{ 0 };
		uint64_t granularity_timeline_logical_below_min_count{ 0 };
		uint64_t granularity_timeline_backend_sector_total{ 0 };
		std::ofstream granularity_timeline_file;
		std::string granularity_future_alignment_path;
		std::deque<GranularityFutureEvent> granularity_future_window;
		std::unordered_map<uint64_t, GranularityFuturePage> granularity_future_pages;
		std::array<GranularityFutureBin, 100> granularity_future_bins{};
		static constexpr uint64_t metadata_overhead_request_sample_interval{ 1 };
		uint64_t metadata_overhead_requests_since_sample{ 0 };
		long double metadata_overhead_request_sample_sum_bytes{ 0 };
		uint64_t metadata_overhead_request_sample_count{ 0 };
		bool metadata_overhead_request_samples_finalized{ false };
		sim_time_type host_request_latency_sum{ 0 };
		uint64_t host_request_latency_count{ 0 };
		sim_time_type host_read_request_latency_sum{ 0 };
		sim_time_type host_write_request_latency_sum{ 0 };
		uint64_t host_read_request_latency_count{ 0 };
		uint64_t host_write_request_latency_count{ 0 };
		uint64_t prefetch_pollution_count{ 0 };

		uint64_t last_arrval_time{ 0 };
		uint64_t now_arrival_time{ 0 };
		uint64_t interval_count{0};

		//uint64_t interval_count{0};

		double average_interval{ 0 };
		set<uint64_t> prefetch_pollution_tracker;

		set<uint64_t> unique_lba;


		uint64_t flash_back_end_queue_size{ 1024 };
		uint64_t flash_back_end_access_count{ 0 };


		//no mshr
		map<uint64_t, list<no_mshr_record_node>> no_mshr_requests_record;
		list<uint64_t> no_mshr_not_yet_serviced_lba;
		uint64_t repeated_flash_access_count{ 0 };
		Host_Interface_Base* hi{NULL};

		
	private:

		float perc{ 1 };

		




	};


	class Input_Stream_CXL : public Input_Stream_Base
	{
	public:
		Input_Stream_CXL(IO_Flow_Priority_Class priority_class, LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
			uint64_t submission_queue_base_address, uint16_t submission_queue_size,
			uint64_t completion_queue_base_address, uint16_t completion_queue_size) : Input_Stream_Base(),
			Priority_class(priority_class),
			Start_logical_sector_address(start_logical_sector_address), End_logical_sector_address(end_logical_sector_address),
			Submission_queue_base_address(submission_queue_base_address), Submission_queue_size(submission_queue_size),
			Completion_queue_base_address(completion_queue_base_address), Completion_queue_size(completion_queue_size),
			Submission_head(0), Submission_head_informed_to_host(0), Submission_tail(0), Completion_head(0), Completion_tail(0), On_the_fly_requests(0) {}
		~Input_Stream_CXL();
		IO_Flow_Priority_Class Priority_class;
		LHA_type Start_logical_sector_address;
		LHA_type End_logical_sector_address;
		uint64_t Submission_queue_base_address;
		uint16_t Submission_queue_size;
		uint64_t Completion_queue_base_address;
		uint16_t Completion_queue_size;
		uint16_t Submission_head;
		uint16_t Submission_head_informed_to_host;//To avoide race condition, the submission head must not be informed to host until the request info of the head is successfully arrived
		uint16_t Submission_tail;
		uint16_t Completion_head;
		uint16_t Completion_tail;
		std::list<User_Request*> Waiting_user_requests;//The list of requests that have been fetch to the device queue and are getting serviced
		std::list<User_Request*> Completed_user_requests;//The list of requests that are completed but have not been informed to the host due to full CQ
		std::list<User_Request*> Waiting_write_data_transfers;//The list of write requests that are waiting for data
		uint16_t On_the_fly_requests;// the number of requests that are either being fetch from host or waiting in the device queue
	};

	class Input_Stream_Manager_CXL : public Input_Stream_Manager_Base
	{
	public:
		Input_Stream_Manager_CXL(Host_Interface_Base* host_interface, uint16_t queue_fetch_szie);
		unsigned int Queue_fetch_size;
		stream_id_type Create_new_stream(IO_Flow_Priority_Class priority_class, LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
			uint64_t submission_queue_base_address, uint16_t submission_queue_size,
			uint64_t completion_queue_base_address, uint16_t completion_queue_size);
		void Submission_queue_tail_pointer_update(stream_id_type stream_id, uint16_t tail_pointer_value);
		void Completion_queue_head_pointer_update(stream_id_type stream_id, uint16_t head_pointer_value);
		void Handle_new_arrived_request(User_Request* request);
		void Handle_arrived_write_data(User_Request* request);
		void Handle_serviced_request(User_Request* request);
		uint16_t Get_submission_queue_depth(stream_id_type stream_id);
		uint16_t Get_completion_queue_depth(stream_id_type stream_id);
		IO_Flow_Priority_Class Get_priority_class(stream_id_type stream_id);
	private:
		void segment_user_request(User_Request* user_request);
		void inform_host_request_completed(stream_id_type stream_id, User_Request* request);
	};

	class Request_Fetch_Unit_CXL : public Request_Fetch_Unit_Base
	{
	public:
		Request_Fetch_Unit_CXL(Host_Interface_Base* host_interface);
		void Fetch_next_request(stream_id_type stream_id);
		void Fetch_write_data(User_Request* request);
		void Send_read_data(User_Request* request);
		void Send_completion_queue_element(User_Request* request, uint16_t sq_head_value);
		void Process_pcie_write_message(uint64_t, void*, unsigned int);
		void Process_pcie_read_message(uint64_t, void*, unsigned int);
	private:
		uint16_t current_phase;
		uint32_t number_of_sent_cqe;
	};

	class Host_Interface_CXL : public Host_Interface_Base
	{
		friend class Input_Stream_Manager_CXL;
		friend class Request_Fetch_Unit_CXL;
		friend class CXL_Manager;
	public:
		Host_Interface_CXL(const sim_object_id_type& id, LHA_type max_logical_sector_address,
			uint16_t submission_queue_depth, uint16_t completion_queue_depth,
			unsigned int no_of_input_streams, uint16_t queue_fetch_size, unsigned int sectors_per_page, Data_Cache_Manager_Base* cache, CXL_DRAM_Model* cxl_dram);
		stream_id_type Create_new_stream(IO_Flow_Priority_Class priority_class, LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
			uint64_t submission_queue_base_address, uint64_t completion_queue_base_address);
		~Host_Interface_CXL();
		void Start_simulation();
		void Validate_simulation_config();
		void Execute_simulator_event(MQSimEngine::Sim_Event*);
		uint16_t Get_submission_queue_depth();
		uint16_t Get_completion_queue_depth();
		void Report_results_in_XML(std::string name_prefix, Utils::XmlWriter& xmlwriter);
		CXL_Manager* cxl_man;
		CXL_DRAM_Model* cxl_dram;
		uint64_t* evict_lba;

		void Consume_pcie_message(Host_Components::PCIe_Message* message)
		{
			if (!(cxl_man->process_requests(message->Address, message->Payload, 0, evict_lba))) {
				delete message;
				return;
			}
			else {

				// if(cxl_man->cxl_config_para.has_cache)((Submission_Queue_Entry*)message->Payload)->Opcode = NVME_READ_OPCODE;
				
				request_fetch_unit->Fetch_next_request(0);
			}

			if (message->Type == Host_Components::PCIe_Message_Type::READ_COMP) {
				request_fetch_unit->Process_pcie_read_message(message->Address, message->Payload, message->Payload_size);
			}
			else {
				request_fetch_unit->Process_pcie_write_message(message->Address, message->Payload, message->Payload_size);
			}
			delete message;
		}

		void Update_CXL_DRAM_state(bool rw, uint64_t lba, bool& falsehit, uint64_t mask) {
			this->cxl_man->dram->process_cache_hit(rw, lba, falsehit, mask);
			//this->cxl_man->dram->set_valid_bits(lba, mask);
			// if (!rw) {
			// 	this->cxl_man->dram->set_dirty_bits(lba, mask);
			// }
			if (falsehit) this->cxl_man->falsehitcount++;
		}
		void Update_CXL_DRAM_state_when_miss_data_ready(bool rw, uint64_t lba, bool serviced_before, bool& completed_removed_from_mshr);
		void process_CXL_prefetch_requests(list<uint64_t> prefetchlba);

		void Send_request_to_CXL_DRAM(CXL_DRAM_ACCESS* dram_request) {
			cxl_dram->service_cxl_dram_access(dram_request);
		}

		void Set_total_number_of_requests(uint64_t total_requests) override {
			if (cxl_dram) {
				cxl_dram->total_number_of_requests = total_requests;
			}
			if (cxl_man) {
				cxl_man->cxl_config_para.total_number_of_requets = total_requests;
			}
		} 
		void Fill_cachelines_from_bitmap(uint64_t lba, list<uint64_t>* flush_lba);
		

		void Notify_DRAM_is_free() {
			Simulator->Register_sim_event(Simulator->Time(), this, 0, 0);
		}
		uint64_t Get_flush_count() {
			return cxl_man->flush_count;
		}
		void print_prefetch_info();


		//void Handle_CXL_false_hit(bool rw, uint64_t lba) {

		//	Submission_Queue_Entry* sqe = new Submission_Queue_Entry;
		//	sqe->Command_Identifier = 0;
		//	sqe->Opcode = (rw) ? NVME_READ_OPCODE : NVME_WRITE_OPCODE;
		//	sqe->Command_specific[0] = (uint32_t)lba*4096; //cxl_man->process_requests will do a translation
		//	sqe->Command_specific[1] = (uint32_t)(lba*4096>>32); 
		//	sqe->Command_specific[2] = ((uint32_t)((uint16_t)8)) & (uint32_t)(0x0000ffff); // magic number
		//	sqe->PRP_entry_1 = (DATA_MEMORY_REGION);//Dummy addresses, just to emulate data read/write access
		//	sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000);//Dummy addresses

		//	if (!(cxl_man->process_requests(0, sqe, 0))) {
		//		delete sqe;
		//		return;
		//	}
		//	else {

		//		sqe->Opcode = NVME_READ_OPCODE;
		//		request_fetch_unit->Fetch_next_request(0);
		//	}

		//	request_fetch_unit->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));

		//	//delete sqe; // this is disrubting the mshr
		//}

	private:
		uint16_t submission_queue_depth, completion_queue_depth;
		unsigned int no_of_input_streams;
		
	};
}

#endif // !HOSTINTERFACE_NVME_H
