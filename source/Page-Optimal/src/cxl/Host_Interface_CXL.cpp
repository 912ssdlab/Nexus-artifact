
#include <stdexcept>
#include "../sim/Engine.h"
#include "Host_Interface_CXL.h"
#include "../ssd/NVM_Transaction_Flash_RD.h"
#include "../ssd/NVM_Transaction_Flash_WR.h"
#include "Optimal_Cache_Oracle.h"
#include <fstream>
#include <Stats.h>
#include <bitset>
#include <FTL.h>
#include <Address_Mapping_Unit_Page_Level.h>

// ofstream ofFlush{ "Flush_initiation_time.txt" };
// ofstream ofrequest{ "Request_recieved.txt" };

ofstream oflatep{};
ofstream oflat_no_cache{};
// ofstream ofprefetch_chance{ "./Results/prefetch_potential.txt" };
ofstream ofrepeated_access;
ofstream of_delta{"Prediction_delta.txt"};

class prefetch_info_node
{
public:
	uint64_t hit_count{0};
	bool late{0};
};

map<uint64_t, prefetch_info_node> PREFETCH_INFO_MAP;
map<uint64_t, prefetch_info_node> PREFETCH_INFO_MAP_SAMPLE;
uint64_t LAST_CACHE_MISS{0}, LAST_POLLUTION{0}, LAST_PREFETCH{0};

set<uint64_t> PREFETCH_ACCURACY_INFO;
set<uint64_t> PREFETCH_LATE_INFO;
namespace
{
	constexpr uint16_t INTERNAL_FLUSH_CMDID = 0xFFFF;

	int admission_granularity_bucket(uint64_t length)
	{
		switch (length) {
		case 1: return 0;
		case 2: return 1;
		case 4: return 2;
		case 8: return 3;
		case 16: return 4;
		case 32: return 5;
		case 64: return 6;
		default: return -1;
		}
	}

	uint64_t count_bits64(uint64_t value)
	{
		return static_cast<uint64_t>(__builtin_popcountll(value));
	}
}
namespace SSD_Components
{

	CXL_Manager::CXL_Manager(Host_Interface_Base *hosti)
	{
		cxl_config_para.readConfigFile();

		dram = new dram_subsystem{cxl_config_para};
		dram->initDRAM();
		hi = hosti;
		mshr = new cxl_mshr;
		prefetched_lba = new set<uint64_t>;
		in_progress_prefetch_lba = new map<uint64_t, uint64_t>;

		((Host_Interface_CXL *)hi)->cxl_dram->total_number_of_requests = cxl_config_para.total_number_of_requets;
	}
	CXL_Manager::~CXL_Manager()
	{
		if (dram)
		{
			delete dram;
		}
		if (mshr)
		{
			delete mshr;
		}
		if (prefetched_lba)
		{
			prefetched_lba->clear();
			delete prefetched_lba;
		}
	}

	void CXL_Manager::record_admission_allocation(uint64_t cachelines, uint64_t backend_sectors)
	{
		int bucket = admission_granularity_bucket(cachelines);
		if (bucket >= 0) {
			admission_granularity_count[bucket]++;
		}
		admission_decision_count++;
		admission_cacheline_total += cachelines;
		admission_backend_sector_total += backend_sectors;
	}

	void CXL_Manager::record_pending_admission_valid(uint64_t lba, uint64_t cacheline_bitmap, uint64_t sector_bitmap)
	{
		auto it = pending_admission_lba.find(lba);
		if (it == pending_admission_lba.end()) {
			return;
		}
		pending_admission_lba.erase(it);

		uint64_t valid_cachelines = count_bits64(cacheline_bitmap);
		admission_valid_sample_count++;
		admission_inserted_cacheline_count += valid_cachelines;
		admission_valid_sector_total += count_bits64(sector_bitmap);
	}

	int CXL_Manager::prefetch_feedback()
	{
		double paccuracy{static_cast<double>(PREFETCH_ACCURACY_INFO.size()) / static_cast<double>(PREFETCH_INFO_MAP.size())};
		double plateness{static_cast<double>(PREFETCH_LATE_INFO.size()) / static_cast<double>(PREFETCH_INFO_MAP.size())};
		double pollution{static_cast<double>(prefetch_pollution_count) / static_cast<double>(cache_miss_count)};

		if (paccuracy >= accuracy_high && plateness >= late_thresh && pollution < pollution_thresh)
		{
			return 1;
		}
		else if (paccuracy >= accuracy_high && plateness >= late_thresh && pollution >= pollution_thresh)
		{
			return 1;
		}
		else if (paccuracy >= accuracy_high && plateness < late_thresh && pollution < pollution_thresh)
		{
			return 0;
		}
		else if (paccuracy >= accuracy_high && plateness < late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_high && paccuracy >= accuracy_low && plateness >= late_thresh && pollution < pollution_thresh)
		{
			return 1;
		}
		else if (paccuracy < accuracy_high && paccuracy >= accuracy_low && plateness >= late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_high && paccuracy >= accuracy_low && plateness < late_thresh && pollution < pollution_thresh)
		{
			return 0;
		}
		else if (paccuracy < accuracy_high && paccuracy >= accuracy_low && plateness < late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_low && plateness >= late_thresh && pollution < pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_low && plateness >= late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}
		else if (paccuracy < accuracy_low && plateness < late_thresh && pollution < pollution_thresh)
		{
			return 0;
		}
		else if (paccuracy < accuracy_low && plateness < late_thresh && pollution >= pollution_thresh)
		{
			return -1;
		}

		return 0;
	}
	void CXL_Manager::prefetch_decision_maker(uint64_t lba, bool isMiss, uint64_t prefetch_hit_count)
	{
		list<uint64_t> prefetchlba;

		/*if (mshr->getSize() < 5 || flash_back_end_queue_size - flash_back_end_access_count - 1 < 9) {
			return;
		}*/

		if (in_progress_prefetch_lba->size() == prefetch_queue_size || flash_back_end_queue_size - flash_back_end_access_count < 1 + 1 + 0)
		{
			previous_unused_lba = lba;
			return;
		}

		if (cxl_config_para.prefetch_policy == prefetchertype::no)
		{
			return;
		}
		else if (cxl_config_para.prefetch_policy == prefetchertype::tagged || cxl_config_para.prefetch_policy == prefetchertype::feedback_direct)
		{
			if (cxl_config_para.prefetch_policy == prefetchertype::feedback_direct)
			{

				int action{prefetch_feedback()};

				prefetch_level += action;
				if (prefetch_level > 4)
					prefetch_level = 4;
				if (prefetch_level < 0)
					prefetch_level = 0;
				vector<uint64_t> temp{prefetch_aggre[prefetch_level]};
				prefetchK = temp[0];
				prefetch_timing_offset = temp[1];
			}

			if (!isMiss && tagAssertedLBA.count(lba) > 0)
			{
				return;
			}
			else if (!isMiss && tagAssertedLBA.count(lba) == 0)
			{
				tagAssertedLBA.insert(lba);
			}

			for (uint64_t i = 1; i <= prefetchK; i++)
			{
				uint64_t plba{lba + i + prefetch_timing_offset};
				if (!dram->isCacheHit(plba) && !mshr->isInProgress(plba) && (in_progress_prefetch_lba->count(plba) == 0) &&
					plba * 8 <= ((Input_Stream_CXL *)(((Host_Interface_CXL *)hi)->input_stream_manager->input_streams[0]))->End_logical_sector_address)
				{
					prefetchlba.push_back(plba);
					// prefetched_lba->insert(plba);
					// tagAssertedLBA.insert(plba);
				}
			}
			// if (tagAssertedLBA.count(lba)) {
			//	tagAssertedLBA.erase(tagAssertedLBA.find(lba));
			// }
		}
		else if (cxl_config_para.prefetch_policy == prefetchertype::bo)
		{
			if (lba >= boPrefetcher.offsetundertest && boPrefetcher.inhistory(lba - boPrefetcher.offsetundertest))
			{
				boPrefetcher.incrementscore(boPrefetcher.offsetundertest);
			}

			boPrefetcher.addhistory(lba);
			boPrefetcher.offsetundertest++;

			if (boPrefetcher.endround())
			{
				boPrefetcher.offsetundertest = 1;
				boPrefetcher.round++;
				if (boPrefetcher.endlphase())
				{
					boPrefetcher.round = 0;
					boPrefetcher.findoffsets();
					if (boPrefetcher.olist.size() > 0)
					{
						boPrefetcher.prefetch_on = 1;
					}
					else
					{
						boPrefetcher.prefetch_on = 0;
					}
					boPrefetcher.resetscore();
				}
			}

			if (boPrefetcher.prefetch_on)
			{
				for (auto i : boPrefetcher.olist)
				{
					uint64_t plba{lba + (i)};

					if (!dram->isCacheHit(plba) && !mshr->isInProgress(plba) && (in_progress_prefetch_lba->count(plba) == 0) &&
						plba * 8 <= ((Input_Stream_CXL *)(((Host_Interface_CXL *)hi)->input_stream_manager->input_streams[0]))->End_logical_sector_address)
					{
						prefetchlba.push_back(plba);
						// of_delta << i << endl;
						// prefetched_lba->insert(plba);
					}
				}
			}
		}
		else if (cxl_config_para.prefetch_policy == prefetchertype::leap && isMiss)
		{
			leapPrefetcher.historyinsert(lba);
			int64_t offset{leapPrefetcher.findoffset()};

			uint64_t leapprefetchK{leapPrefetcher.getk(prefetch_hit_count)};

			if (offset == 0)
			{
				offset = leapPrefetcher.last_offset;
				for (uint64_t i = 1; i <= leapprefetchK; i++)
				{

					uint64_t plba{lba + i};

					plba = (offset >= 0) ? plba + static_cast<uint64_t>(offset) : plba - static_cast<uint64_t>(-1 * offset);

					// check if the prefetch block is in the cache or miss map
					// prefetcher_ol.print_offset(plba - event->lba);
					if (!dram->isCacheHit(plba) && !mshr->isInProgress(plba) && (in_progress_prefetch_lba->count(plba) == 0) &&
						plba * 8 <= ((Input_Stream_CXL *)(((Host_Interface_CXL *)hi)->input_stream_manager->input_streams[0]))->End_logical_sector_address)
					{

						prefetchlba.push_back(plba);
						// prefetched_lba->insert(plba);
					}
				}
			}
			else
			{
				for (uint64_t i = 1; i <= leapprefetchK; i++)
				{
					uint64_t plba{lba};

					plba = (offset >= 0) ? plba + i * static_cast<uint64_t>(offset) : plba - i * static_cast<uint64_t>(-1 * offset);

					// check if the prefetch block is in the cache or miss map
					// prefetcher_ol.print_offset(plba - event->lba);
					if (!dram->isCacheHit(plba) && !mshr->isInProgress(plba) && (in_progress_prefetch_lba->count(plba) == 0) &&
						plba * 8 <= ((Input_Stream_CXL *)(((Host_Interface_CXL *)hi)->input_stream_manager->input_streams[0]))->End_logical_sector_address)
					{

						prefetchlba.push_back(plba);
						// prefetched_lba->insert(plba);
					}
				}
			}
			leapPrefetcher.last_offset = offset;
		}

		// if (mshr->getSize()-4 < flash_back_end_queue_size - flash_back_end_access_count - 8) {
		//	if (prefetchlba.size() > mshr->getSize()-4) {
		//		prefetchlba.resize(mshr->getSize()-4);
		//	}
		// }
		// else {
		//	if (prefetchlba.size() > flash_back_end_queue_size - flash_back_end_access_count - 1 - 8) {
		//		prefetchlba.resize(flash_back_end_queue_size - flash_back_end_access_count - 1 - 8);
		//	}
		// }

		if (prefetch_queue_size - in_progress_prefetch_lba->size() < flash_back_end_queue_size - flash_back_end_access_count - 1 - 0)
		{
			if (prefetchlba.size() > prefetch_queue_size - in_progress_prefetch_lba->size())
			{
				prefetchlba.resize(prefetch_queue_size - in_progress_prefetch_lba->size());
			}
		}
		else
		{
			if (prefetchlba.size() > flash_back_end_queue_size - flash_back_end_access_count - 1 - 0)
			{
				prefetchlba.resize(flash_back_end_queue_size - flash_back_end_access_count - 1 - 0);
			}
		}

		for (auto i : prefetchlba)
		{
			prefetched_lba->insert(i);
		}
		((Host_Interface_CXL *)hi)->process_CXL_prefetch_requests(prefetchlba);
	}

	bool CXL_Manager::process_requests(uint64_t address, void *payload, bool is_pref_req, uint64_t *evcit_lba)
	{
		bool cache_miss{1};

		Submission_Queue_Entry *sqe = (Submission_Queue_Entry *)payload;


		interval_count++;
		if (interval_count ==1)
		{
			now_arrival_time = Simulator->Time();
		}
		else if (interval_count > 1)
		{
			last_arrval_time = now_arrival_time;
			now_arrival_time = Simulator->Time();
			average_interval+=(now_arrival_time - last_arrval_time);
		}

		u_int64_t internal_addr = sqe->Command_specific[0] % ((((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->End_logical_sector_address - (((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->Start_logical_sector_address)) * 512);


		u_int64_t cacheline_idx = sqe->Command_specific[0] % 4096 / 64;


		sqe->Command_specific[2] = ((uint32_t)((uint16_t)1)) & (uint32_t)(0x0000ffff);

		LHA_type memory_addr{(((LHA_type)sqe->Command_specific[1]) << 32) | (LHA_type)sqe->Command_specific[0]};
		

		// No translate
		// LHA_type lba{ memory_addr };
		// translate
		//  4096 is the page size
		LHA_type lba{memory_addr / (cxl_config_para.num_sec * 512)}; // stream alignment will be done when dealing with transaction segmentation
		LHA_type page_base_addr = lba * cxl_config_para.num_sec * 512;
        LHA_type sector_offset = (memory_addr - page_base_addr) / 512;	
		//LHA_type lsa{lba * cxl_config_para.num_sec};				  // lsa to be used for request
		LHA_type lsa = lba * cxl_config_para.num_sec + sector_offset;


		if (lsa < ((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->Start_logical_sector_address || lsa > ((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->End_logical_sector_address)
		{
			lsa = ((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->Start_logical_sector_address + (lsa % (((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->End_logical_sector_address - (((Input_Stream_CXL *)hi->input_stream_manager->input_streams[0])->Start_logical_sector_address)));
			lba = lsa / cxl_config_para.num_sec;
		}

		sqe->Command_specific[0] = (uint32_t)lsa;
		sqe->Command_specific[1] = (uint32_t)(lsa >> 32);

		sector_offset = lsa % cxl_config_para.num_sec;
		uint64_t byte_offset_in_sector = memory_addr % 512;
		cacheline_idx = (sector_offset * 512 + byte_offset_in_sector) / 64;


		uint64_t lines_per_page = cxl_config_para.ssd_page_size / 64; // 4096/64 = 64
		uint64_t global_cacheline_id = lba * lines_per_page + cacheline_idx;
		if (!is_pref_req) {
			Optimal_Cache_Oracle::ConsumeAccess(lba);
		}

		// 反推
		//  uint64_t lba = global_cacheline_id / 64;
		//  uint64_t cacheline_idx = global_cacheline_id % 64;

		if (!cxl_config_para.has_cache)
		{
			// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " cm" << endl;
			return 1;
		}
		bool cacheline_dram_enable = false;
		
		if (cacheline_dram_enable)
		{
			if (dram->cacheline_dram->contains(global_cacheline_id)) // cacheline_cache hit
			{
				cache_miss = 0;
				bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_HIT, Simulator->Time()}};
				dram_request->is_cacheline = true;
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
				if (!rw)
				{
					dram->cacheline_dram->markDirty(global_cacheline_id);
				}
				return cache_miss;
			}
				else if (cxl_config_para.dram_mode || dram->isCacheHit(lba)) // page_cache hit
				{
					auto it = dram->write_map->find(lba);
					bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
					if(rw && it != dram->write_map->end() && ((it->second & (1ULL << cacheline_idx)) == 0)) //read miss  lba is in write_map, but the cacheline is not written yet, so it's a miss
				{
					if (!cxl_config_para.has_mshr)
					{
						no_mshr_record_node n;
						n.time = Simulator->Time();
						n.rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
						if (no_mshr_requests_record.count(lba))
						{
							no_mshr_requests_record[lba].push_back(n);
							repeated_flash_access_count++;
							ofrepeated_access << lba << " " << 1 << endl;
						}
						else
							{
								list<no_mshr_record_node> l;
								l.push_back(n);
								no_mshr_requests_record.emplace(lba, l);
								ofrepeated_access << lba << " " << 0 << endl;
							}
						cache_miss = 1;
						return cache_miss;
					}
					if (mshr->isInProgress(lba))
					{
						cache_miss = 0; // 控制不去发送请求的
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
					}
					else
						{
							// if (sqe->Opcode == NVME_WRITE_OPCODE) {
							//
							//	cout << "Check" << endl;
							// }
							if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
							{
							if (prefetch_pollution_tracker.count(lba))
							{
								prefetch_pollution_count++;
								prefetch_pollution_tracker.erase(prefetch_pollution_tracker.find(lba));
							}
						}
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						// 下面是预取控制路径
						if (in_progress_prefetch_lba->count(lba))
						{
							cache_miss = 0; // 这里也是控制不去发送请求的
						}
						else
						{
							if (!is_pref_req)
							{
								cache_miss_count++;
								prefetch_decision_maker(lba, 1, prefetch_hit_count);
							}
						}
						//cache_miss = 1;

					}		
					//cache_miss = 1;
					return cache_miss;
				}
				cache_miss = 0;
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_HIT, Simulator->Time()}};
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
				bool falsehit{0};
				uint64_t mask{~0ULL};
				// std::cout<<"mask: "<<std::bitset<64>(mask)<<std::endl;
				// exit(0);
				auto temp = dram->record_access_dram_dirty_bitmap->find(lba);
				if (temp != dram->record_access_dram_dirty_bitmap->end())
				{
					(*(dram->record_access_dram_dirty_bitmap))[lba] |= (1ULL << cacheline_idx);
				}
				if (it != dram->write_map->end())
				{
					(*(dram->write_map))[lba] |= (1ULL << cacheline_idx);
				}

				if (!cxl_config_para.dram_mode)
					((Host_Interface_CXL *)hi)->Update_CXL_DRAM_state(rw, lba, falsehit, mask);
				if (falsehit)
					falsehitcount++;
				if (!is_pref_req)
					cache_hit_count++;
				if (!is_pref_req && prefetched_lba->count(lba))
				{
					prefetch_hit_count++;
					PREFETCH_INFO_MAP[lba].hit_count++;
					PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
					PREFETCH_ACCURACY_INFO.emplace(lba);
					prefetch_decision_maker(lba, 0, prefetch_hit_count);

					if (!is_pref_req)
					{
						// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " ph" << endl;
					}
				}
				else
				{
					// prefetch_decision_maker(previous_unused_lba, 0, prefetch_hit_count);
					if (!is_pref_req)
					{
						// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " ch" << endl;
					}
				}
				return cache_miss;
			}
			else // miss in both cacheline_cache and page_cache, need to check MSHR and decide whether to send request to dram or not
			{
				bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
				if (!rw)
				{
					auto evicted = dram->cacheline_dram->put(global_cacheline_id, true); // 往cacheline_dram中写
					if (evicted)
					{
						if (!evicted->dirty)
						{
							cache_miss = 0;
							CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_MISS, Simulator->Time()}};
							dram_request->is_cacheline = true;
							dram_request->is_writeback = true;
							((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
							return cache_miss; //  算命中不产生向下写的请求,但是统计上还是算miss
						}
						else
						{
							uint64_t evicted_lba = evicted->key / lines_per_page; // lba eviceed from cacheline_cache
							// evict all cacheline in this lba; 
							uint64_t base_cacheline_id = evicted_lba * lines_per_page;
						    uint64_t evicted_cacheline_mask = 1ULL << (evicted->key % lines_per_page); // 先把这个被evict出去的cacheline对应的bit加到evicted_cacheline_mask里
							for (uint64_t i = 0; i < lines_per_page; i++)
							{
								uint64_t global_cacheline_id = base_cacheline_id + i;
								if (!dram->cacheline_dram->contains(global_cacheline_id, false))
									continue;
								evicted_cacheline_mask |= (1ULL << i);
								// 这里处理这个 page 里的第 i 个 cacheline
								dram->cacheline_dram->setDirty(global_cacheline_id, false); // 先把这个cacheline的dirty bit清掉, 不用弹出, 顺带写下去;
								
							}
							// Then check in page_dram;
							uint64_t evicted_sector_mask = cacheline_mask_to_sector_mask(evicted_cacheline_mask);
							if (cxl_config_para.dram_mode || dram->isCacheHit(evicted_lba)) // 如果在page_dram中命中，说明只是被cacheline_cache挤出去了，不算真正的miss
							{
								//cache_miss = 0;
								// bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
								// CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_HIT, Simulator->Time()}};
								// ((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
								bool falsehit{0};
								dram->evict(rw, evicted_lba, falsehit, 0);

								auto temp = dram->write_map->find(evicted_lba);
								if (temp != dram->write_map->end())
								{
									evicted_sector_mask |= temp->second; // 如果这个evicted_lba在write_map中，说明这个lba有些cacheline是被写过的，这些cacheline对应的bit在write_map里是1，需要把这些bit也加入到evicted_sector_mask中
									// 不用弹出, 数据还在dram中, 只不过变干净了
								}
									
								
							}
							uint64_t lsa = evicted_lba  * cxl_config_para.num_sec;
							sqe->Command_specific[0] = (uint32_t)lsa;
							sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
							sqe->Command_specific[2] = ((uint32_t)((uint16_t)count_sector_no_from_status_bitmap(evicted_sector_mask))) & (uint32_t)(0x0000ffff);
							sqe->Command_specific[3] = 1;
							sqe->Command_specific[4] = (uint32_t)(evicted_sector_mask & 0xFFFFFFFFULL);
							sqe->Command_specific[5] = (uint32_t)((evicted_sector_mask >> 32) & 0xFFFFFFFFULL);
							sqe->Reserved = Simulator->Time();
							cache_miss = true;
						    flush_count++;
							return cache_miss;
						}
					}
					else
					{
						cache_miss = 0;
						CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_MISS, Simulator->Time()}};
						dram_request->is_cacheline = true;
						dram_request->is_writeback = true;
						((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
						return cache_miss;
					}
				}
				else // It is a read request, and it misses in both cacheline_cache and page_cache, need to check MSHR and decide whether to send request to dram or not
				{

					if (!cxl_config_para.has_mshr)
					{
						no_mshr_record_node n;
						n.time = Simulator->Time();
						n.rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
						if (no_mshr_requests_record.count(lba))
						{
							no_mshr_requests_record[lba].push_back(n);
							repeated_flash_access_count++;
							ofrepeated_access << lba << " " << 1 << endl;
						}
						else
						{
							list<no_mshr_record_node> l;
							l.push_back(n);
							no_mshr_requests_record.emplace(lba, l);
							ofrepeated_access << lba << " " << 0 << endl;
						}
						cache_miss = 1;
						return cache_miss;
					}
					if (mshr->isInProgress(lba))
					{
						cache_miss = 0; // 控制不去发送请求的
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						return cache_miss;
					}
					else
					{
						if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
						{
							if (prefetch_pollution_tracker.count(lba))
							{
								prefetch_pollution_count++;
								prefetch_pollution_tracker.erase(prefetch_pollution_tracker.find(lba));
							}
						}
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						// 下面是预取控制路径
						if (in_progress_prefetch_lba->count(lba))
						{
							cache_miss = 0; // 这里也是控制不去发送请求的
						}
						else
						{
							if (!is_pref_req)
							{
								cache_miss_count++;
								prefetch_decision_maker(lba, 1, prefetch_hit_count);
							}
						}
					}
				}
			//	cache_miss = 0; // 说明cacheline_dram的容量够 可以直接往里面写
				return cache_miss;
			}
		}
		else
		{
			if (cxl_config_para.dram_mode || dram->isCacheHit(lba)) 
			{
				
				auto it = dram->write_map->find(lba);
				bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
				uint64_t cacheline_mask = 1ULL << cacheline_idx;
				bool cacheline_present = cxl_config_para.dram_mode ||
					(it != dram->write_map->end() && ((it->second & cacheline_mask) != 0));
				if(rw && !cacheline_present) //read miss  lba is in write_map, but the cacheline is not written yet, so it's a miss
				{
					if (!cxl_config_para.has_mshr)
					{
						no_mshr_record_node n;
						n.time = Simulator->Time();
						n.rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
						if (no_mshr_requests_record.count(lba))
						{
							no_mshr_requests_record[lba].push_back(n);
							repeated_flash_access_count++;
							ofrepeated_access << lba << " " << 1 << endl;
						}
						else
						{
							list<no_mshr_record_node> l;
							l.push_back(n);
							no_mshr_requests_record.emplace(lba, l);
							ofrepeated_access << lba << " " << 0 << endl;
						}
						cache_miss = 1;
						return cache_miss;
					}
					if (mshr->isInProgress(lba))
					{
						cache_miss = 0; // 控制不去发送请求的
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
					}
					else
					{
						// if (sqe->Opcode == NVME_WRITE_OPCODE) {
						//
						//	cout << "Check" << endl;
						// }
						if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
						{
							if (prefetch_pollution_tracker.count(lba))
							{
								prefetch_pollution_count++;
								prefetch_pollution_tracker.erase(prefetch_pollution_tracker.find(lba));
							}
						}
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						// 下面是预取控制路径
						if (in_progress_prefetch_lba->count(lba))
						{
							cache_miss = 0; // 这里也是控制不去发送请求的
						}
						else
						{
							if (!is_pref_req)
							{
								cache_miss_count++;
								prefetch_decision_maker(lba, 1, prefetch_hit_count);
							}
						}
						//cache_miss = 1;

					}		
					//cache_miss = 1;
					return cache_miss;
				}
				cache_miss = 0;
				CXL_DRAM_EVENTS dram_event = (!rw && !cacheline_present) ?
					CXL_DRAM_EVENTS::CACHE_MISS : CXL_DRAM_EVENTS::CACHE_HIT;
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, dram_event, Simulator->Time()}};
				if (dram_event == CXL_DRAM_EVENTS::CACHE_MISS)
				{
					dram_request->is_writeback = true;
				}
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
				bool falsehit{0};
				uint64_t mask{~0ULL};
				// std::cout<<"mask: "<<std::bitset<64>(mask)<<std::endl;
				// exit(0);
				auto temp = dram->record_access_dram_dirty_bitmap->find(lba);
				if (temp != dram->record_access_dram_dirty_bitmap->end())
				{
					(*(dram->record_access_dram_dirty_bitmap))[lba] |= cacheline_mask;
				}
				if (it != dram->write_map->end())
				{
					(*(dram->write_map))[lba] |= cacheline_mask;
				}
				else
				{
					dram->write_map->emplace(lba, cacheline_mask);
				}

				if (!cxl_config_para.dram_mode)
					((Host_Interface_CXL *)hi)->Update_CXL_DRAM_state(rw, lba, falsehit, mask);
				if (falsehit)
					falsehitcount++;
				if (!is_pref_req && dram_event == CXL_DRAM_EVENTS::CACHE_HIT)
					cache_hit_count++;
				if (!is_pref_req && prefetched_lba->count(lba))
				{
					prefetch_hit_count++;
					PREFETCH_INFO_MAP[lba].hit_count++;
					PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
					PREFETCH_ACCURACY_INFO.emplace(lba);
					prefetch_decision_maker(lba, 0, prefetch_hit_count);

					if (!is_pref_req)
					{
						// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " ph" << endl;
					}
				}
				else
				{
					// prefetch_decision_maker(previous_unused_lba, 0, prefetch_hit_count);
					if (!is_pref_req)
					{
						// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " ch" << endl;
					}
				}
				return cache_miss;
			}
			else
			{
				if (!is_pref_req)
				{
					// ofprefetch_chance << flash_back_end_queue_size - flash_back_end_access_count - 1 << " cm" << endl;
				}
				bool rw{(sqe->Opcode == NVME_READ_OPCODE) ? true : false};
				if (sqe->Opcode == NVME_WRITE_OPCODE)
				{

					list<uint64_t> *flush_lba{new list<uint64_t>};
					uint64_t cacheline_bitmap = 1ULL << cacheline_idx;
					dram->process_miss_data_ready_new(rw, lba, flush_lba, Simulator->Time(), prefetched_lba, tagAssertedLBA, prefetch_pollution_tracker, serviced_before_lba, cacheline_bitmap);
					// mask = 1 new write only one cacheline
					if(flush_lba->size() > 0) 
					{
						if (flush_lba->size() > 1)
						{
							std::cout<<"error: flush more than 1 cacheline!"<<endl;
							while(1){}
						}
						//flush_count++;
						uint64_t evcit_lba = flush_lba->front();
						flush_lba->pop_front();
						uint64_t lsa = evcit_lba * cxl_config_para.num_sec;
						sqe->Command_specific[0] = (uint32_t)lsa;
						sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
						uint64_t mask = ((uint32_t)((uint16_t)cxl_config_para.num_sec)) & (uint32_t)(0x0000ffff);
						auto it = dram->write_map->find(evcit_lba);
						if (it != dram->write_map->end())
						{
							mask = it->second;
							dram->write_map->erase(it);
						}
						sqe->Command_specific[2] = ((uint32_t)((uint16_t)count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff); 
						sqe->Command_specific[3] = 1;
						uint64_t sector_bitmap = cacheline_mask_to_sector_mask(mask);
						sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
						sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
						sqe->Reserved = Simulator->Time();
						cache_miss = true;
						flush_count++;
					}
					else
					{
						CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_MISS, Simulator->Time()}};
						dram_request->is_writeback = true;
						((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
						cache_miss = false;
					}
				}
				else
				{
					if (!cxl_config_para.has_mshr)
					{
						no_mshr_record_node n;
						n.time = Simulator->Time();
						n.rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
						if (no_mshr_requests_record.count(lba))
						{
							no_mshr_requests_record[lba].push_back(n);
							repeated_flash_access_count++;
							ofrepeated_access << lba << " " << 1 << endl;
						}
						else
						{
							list<no_mshr_record_node> l;
							l.push_back(n);
							no_mshr_requests_record.emplace(lba, l);
							if (!is_pref_req)
							{
								record_admission_allocation(lines_per_page, cxl_config_para.num_sec);
								pending_admission_lba.emplace(lba);
							}
							ofrepeated_access << lba << " " << 0 << endl;
						}
						cache_miss = 1;
						return cache_miss;
					}
					if (mshr->isInProgress(lba))
					{
						cache_miss = 0; // 控制不去发送请求的
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
					}
					else
					{
						// if (sqe->Opcode == NVME_WRITE_OPCODE) {
						//
						//	cout << "Check" << endl;
						// }
						if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
						{
							record_admission_allocation(lines_per_page, cxl_config_para.num_sec);
							pending_admission_lba.emplace(lba);
						}
						if (!is_pref_req && !in_progress_prefetch_lba->count(lba))
						{
							if (prefetch_pollution_tracker.count(lba))
							{
								prefetch_pollution_count++;
								prefetch_pollution_tracker.erase(prefetch_pollution_tracker.find(lba));
							}
						}
						Submission_Queue_Entry *nsqe{new Submission_Queue_Entry{*sqe}};
						mshr->insertRequest(lba, Simulator->Time(), nsqe);
						if (mshr->isFull())
						{
							// notify cxl pcie device is full
							hi->Notify_CXL_Host_mshr_full();
						}
						// 下面是预取控制路径
						if (in_progress_prefetch_lba->count(lba))
						{
							cache_miss = 0; // 这里也是控制不去发送请求的
						}
						else
						{
							if (!is_pref_req)
							{
								cache_miss_count++;
								prefetch_decision_maker(lba, 1, prefetch_hit_count);
							}
						}
					}

				}
				}
			}
			return cache_miss;
	}
		int CXL_Manager::count_sectors_from_cacheline_mask(uint64_t cl_mask) {
    int sector_count = 0;
    for (int s = 0; s < 8; s++) {
        uint64_t group = (cl_mask >> (s * 8)) & 0xFFULL;
        if (group != 0) {
            sector_count++;
        }
    }
    return sector_count;
	}

	uint64_t CXL_Manager::cacheline_mask_to_sector_mask(uint64_t cl_mask) {
		uint64_t sec_mask = 0;
		for (int s = 0; s < 8; s++) {
			uint64_t group = (cl_mask >> (s * 8)) & 0xFFULL;
			if (group != 0) {
				sec_mask |= (1ULL << s);
			}
		}
		return sec_mask;
	}

	uint64_t CXL_Manager::sector_mask_to_cacheline_mask(uint64_t sec_mask) {
    uint64_t cl_mask = 0;
    for (int sec = 0; sec < 8; sec++) {
        if (sec_mask & (1ULL << sec)) {
            cl_mask |= (0xFFULL << (sec * 8));
        }
    }
    return cl_mask;
}

	void CXL_Manager::request_serviced(User_Request *request)
	{
		Submission_Queue_Entry *sqe = (Submission_Queue_Entry *)request->IO_command_info;
		LHA_type lba{((((LHA_type)sqe->Command_specific[1]) << 32) | (LHA_type)sqe->Command_specific[0]) / (cxl_config_para.num_sec)};
		if (!cxl_config_para.has_cache)
		{
			unique_lba.insert(lba);

			if (flash_back_end_access_count >= flash_back_end_queue_size)
			{
				if (flash_back_end_access_count > flash_back_end_queue_size)
				{
					std::cout << "Check" << endl;
				}
				hi->Notify_CXL_Host_flash_not_full();
			}
			flash_back_end_access_count--;

			oflat_no_cache << Simulator->Time() - request->STAT_InitiationTime << endl;

			total_number_of_accesses++;

			if (sqe->Opcode == NVME_WRITE_OPCODE)
				no_cache_flash_write_count++;
			else
				no_cache_flash_read_count++;

			float current_progress{static_cast<float>(total_number_of_accesses) / static_cast<float>(cxl_config_para.total_number_of_requets)};
			if (current_progress * 100 - perc > 1)
			{
				perc += 1;
				uint8_t number_of_bars{static_cast<uint8_t>(perc / 4)};
				std::cout << "Simulation progress: [";
				for (auto i = 0; i < number_of_bars; i++)
				{
					std::cout << "=";
				}
				for (auto i = 0; i < 25 - number_of_bars - 1; i++)
				{
					std::cout << " ";
				}
				std::cout << "] " << perc << "%" << "\r";
			}

			if (total_number_of_accesses == cxl_config_para.total_number_of_requets)
			{
				std::cout << "Simulation progress: [";
				for (auto i = 0; i < 25; i++)
				{
					std::cout << "=";
				}
				std::cout << "] " << 100 << "%" << std::endl;
				std::cout << "Flash read count: " << no_cache_flash_read_count << endl;
				std::cout << "Flash write count: " << no_cache_flash_write_count << endl;
				std::cout << "Unique LBA ratio: " << static_cast<float>(unique_lba.size()) / static_cast<float>(cxl_config_para.total_number_of_requets) << endl;
				std::cout << "Request ends at timestamp: " << static_cast<float>(Simulator->Time()) / 1000000000 << " s" << endl;
			}
			return;
		}
		if (sqe->Opcode == NVME_WRITE_OPCODE)
		{ // flush done
			if (flash_back_end_access_count >= flash_back_end_queue_size)
			{
				if (flash_back_end_access_count > flash_back_end_queue_size)
				{
					std::cout << "Check" << endl;
				}
				hi->Notify_CXL_Host_flash_not_full();
			}

			// if write miss write bcck
			if (sqe->Command_specific[3] == 1) // this is write miss write back
			{
				bool rw{false};
				rw = (sqe->Opcode == NVME_READ_OPCODE) ? true : false;
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, rw, CXL_DRAM_EVENTS::CACHE_MISS, ((Submission_Queue_Entry*)request->IO_command_info)->Reserved}};
				//dram_request->is_writeback = true;
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);
			
			}
			flash_back_end_access_count--;

			// flush_count++;
			// ofFlush << request->STAT_InitiationTime << " " << Simulator->Time() << endl;
			// outputf.of << "Finshed_time " << Simulator->Time()  << " Starting_time " << request->STAT_InitiationTime << " Flush_done_at " << lba <<  std::endl;
			return;
		}

		if (!cxl_config_para.has_mshr)
		{
			if (((Host_Interface_CXL *)hi)->cxl_dram->getDRAMAvailability() && no_mshr_not_yet_serviced_lba.empty())
			{
				no_mshr_record_node n{no_mshr_requests_record[lba].front()};
				no_mshr_requests_record[lba].pop_front();

				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{static_cast<unsigned int>(64), lba, n.rw, CXL_DRAM_EVENTS::CACHE_MISS, n.time}};
				((Host_Interface_CXL *)hi)->Send_request_to_CXL_DRAM(dram_request);

				list<uint64_t> *flush_lba{new list<uint64_t>};

				page_status_type sector_bitmap = ((Address_Mapping_Unit_Page_Level*) ((FTL*)(hi->cache->nvm_firmware))->Address_Mapping_Unit)->domains[0]->Get_page_status(true, 0 ,lba);

				//std::cout<<std::bitset<64>(sector_bitmap)<<std::endl;
				auto* amu = static_cast<FTL*>(hi->cache->nvm_firmware)->Address_Mapping_Unit;
				PPA_type ppa;
			//	page_status_type sector_bitmap = 0;
				amu->Get_data_mapping_info_for_gc(0, lba, ppa, sector_bitmap);
				// std::cout << "lba=" << lba
				// 		<< " ppa=" << ppa
				// 		<< " sector_bitmap=" << std::bitset<64>(sector_bitmap)
				// 		<< std::endl;
				//std::cout<<std::bitset<8>(sector_bitmap)<<std::endl;

				uint64_t cacheline_bitmap = sector_mask_to_cacheline_mask(sector_bitmap);
				//	std::cout<<std::bitset<64>(cacheline_bitmap)<<std::endl;
				record_pending_admission_valid(lba, cacheline_bitmap, sector_bitmap);
				dram->process_miss_data_ready_new(n.rw, lba, flush_lba, Simulator->Time(), prefetched_lba, tagAssertedLBA, prefetch_pollution_tracker, serviced_before_lba,cacheline_bitmap);

				if (flash_back_end_access_count >= flash_back_end_queue_size)
				{
					if (flash_back_end_access_count > flash_back_end_queue_size)
					{
						std::cout << "Check" << endl;
					}
					hi->Notify_CXL_Host_flash_not_full();
				}
				flash_back_end_access_count--;

				if (no_mshr_requests_record[lba].empty())
				{
					no_mshr_requests_record.erase(no_mshr_requests_record.find(lba));
				}

				while (!flush_lba->empty())
				{
					flush_count++;
					uint64_t lba = flush_lba->front();
					uint64_t lsa{lba * 8};

					flush_lba->pop_front();
					Submission_Queue_Entry *sqe{new Submission_Queue_Entry};
					sqe->Command_Identifier = 0;
					sqe->Opcode = NVME_WRITE_OPCODE;
					sqe->Command_specific[0] = (uint32_t)lsa;
					sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
					uint64_t mask = ((uint32_t)((uint16_t) cxl_config_para.num_sec)) & (uint32_t)(0x0000ffff);
					//uint64_t evicted_lba = lba / 64; // lba eviceed from cacheline_cache
							// evict all cacheline in this lba; 
					uint64_t base_cacheline_id = lba * 64;
					uint64_t evicted_cacheline_mask = 0ULL;
					for (int i = 0 ; i < 64 ; i++)
					{
						//uint64_t global_cacheline_id = base_cacheline_id + i;
						uint64_t global_cacheline_id = base_cacheline_id + i;
						if (!dram->cacheline_dram->contains(global_cacheline_id, false))
							continue;
						evicted_cacheline_mask |= (1ULL << i);
						// 这里处理这个 page 里的第 i 个 cacheline
						dram->cacheline_dram->setDirty(global_cacheline_id, false); // 先把这个
						
					}
					
				
			

					auto it = dram->write_map->find(lba);
					if (it != dram->write_map->end())
					{
						mask = it->second;
						dram->write_map->erase(it);
					}
					sqe->Command_specific[2] = ((uint32_t)((uint16_t)count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff);
					sqe->Command_specific[3] = 0; // flush  caued by read miss;
					uint64_t sector_bitmap = cacheline_mask_to_sector_mask(mask);
					sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
					sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
					sqe->Reserved = 0;
					sqe->PRP_entry_1 = (DATA_MEMORY_REGION);		  // Dummy addresses, just to emulate data read/write access
					sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000); // Dummy addresses

					((Request_Fetch_Unit_CXL *)(((Host_Interface_CXL *)hi)->request_fetch_unit))->Fetch_next_request(0);

					((Request_Fetch_Unit_CXL *)(((Host_Interface_CXL *)hi)->request_fetch_unit))->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
				}

				delete flush_lba;
			}
			else
			{
				no_mshr_not_yet_serviced_lba.push_back(lba);
			}

			return;
		}

		if (((Host_Interface_CXL *)hi)->cxl_dram->getDRAMAvailability() && serviced_before_lba.size() == 0)
		{

			if (serviced_before_lba.size() > 0)
			{
				std::cout << Simulator->Time() << endl;
			}

			bool rw{true};
			bool completely_removed{0};
			((Host_Interface_CXL *)hi)->Update_CXL_DRAM_state_when_miss_data_ready(rw, lba, 0, completely_removed);

			if (!completely_removed)
			{
				serviced_before_lba.emplace(lba);
			}
		}
		else
		{
			((Host_Interface_CXL *)hi)->cxl_man->not_yet_serviced_lba.emplace(lba);
		}
	}

	Input_Stream_CXL::~Input_Stream_CXL()
	{
		for (auto &user_request : Waiting_user_requests)
			delete user_request;
		for (auto &user_request : Completed_user_requests)
			delete user_request;
	}

	Input_Stream_Manager_CXL::Input_Stream_Manager_CXL(Host_Interface_Base *host_interface, uint16_t queue_fetch_szie) : Input_Stream_Manager_Base(host_interface), Queue_fetch_size(queue_fetch_szie)
	{
	}

	stream_id_type Input_Stream_Manager_CXL::Create_new_stream(IO_Flow_Priority_Class priority_class, LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
															   uint64_t submission_queue_base_address, uint16_t submission_queue_size, uint64_t completion_queue_base_address, uint16_t completion_queue_size)
	{
		if (end_logical_sector_address < start_logical_sector_address)
		{
			PRINT_ERROR("Error in allocating address range to a stream in host interface: the start address should be smaller than the end address.")
		}
		Input_Stream_CXL *input_stream = new Input_Stream_CXL(priority_class, start_logical_sector_address, end_logical_sector_address,
															  submission_queue_base_address, submission_queue_size, completion_queue_base_address, completion_queue_size);
		this->input_streams.push_back(input_stream);

		return (stream_id_type)(this->input_streams.size() - 1);
	}

	inline void Input_Stream_Manager_CXL::Submission_queue_tail_pointer_update(stream_id_type stream_id, uint16_t tail_pointer_value)
	{
		//((Input_Stream_CXL*)input_streams[stream_id])->Submission_tail = tail_pointer_value;

		// if (((Input_Stream_CXL*)input_streams[stream_id])->On_the_fly_requests < Queue_fetch_size) {
		//	((Host_Interface_CXL*)host_interface)->request_fetch_unit->Fetch_next_request(stream_id);
		//	((Input_Stream_CXL*)input_streams[stream_id])->On_the_fly_requests++;
		//	((Input_Stream_CXL*)input_streams[stream_id])->Submission_head++;//Update submission queue head after starting fetch request
		//	if (((Input_Stream_CXL*)input_streams[stream_id])->Submission_head == ((Input_Stream_CXL*)input_streams[stream_id])->Submission_queue_size) {//Circular queue implementation
		//		((Input_Stream_CXL*)input_streams[stream_id])->Submission_head = 0;
		//	}
		// }
	}

	inline void Input_Stream_Manager_CXL::Completion_queue_head_pointer_update(stream_id_type stream_id, uint16_t head_pointer_value)
	{
		//((Input_Stream_CXL*)input_streams[stream_id])->Completion_head = head_pointer_value;

		////If this check is true, then the host interface couldn't send the completion queue entry, since the completion queue was full
		// if (((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.size() > 0) {
		//	User_Request* request = ((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.front();
		//	((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.pop_front();
		//	inform_host_request_completed(stream_id, request);
		// }
	}

	inline void Input_Stream_Manager_CXL::Handle_new_arrived_request(User_Request *request)
	{
		((Input_Stream_CXL *)input_streams[request->Stream_id])->Submission_head_informed_to_host++;
		if (((Input_Stream_CXL *)input_streams[request->Stream_id])->Submission_head_informed_to_host == ((Input_Stream_CXL *)input_streams[request->Stream_id])->Submission_queue_size)
		{ // Circular queue implementation
			((Input_Stream_CXL *)input_streams[request->Stream_id])->Submission_head_informed_to_host = 0;
		}
		if (request->Type == UserRequestType::READ)
		{
			((Input_Stream_CXL *)input_streams[request->Stream_id])->Waiting_user_requests.push_back(request);
			((Input_Stream_CXL *)input_streams[request->Stream_id])->STAT_number_of_read_requests++;
			segment_user_request(request);

			((Host_Interface_CXL *)host_interface)->broadcast_user_request_arrival_signal(request);
		}
		else
		{ // This is a write request
			((Input_Stream_CXL *)input_streams[request->Stream_id])->Waiting_user_requests.push_back(request);
			((Input_Stream_CXL *)input_streams[request->Stream_id])->STAT_number_of_write_requests++;
			((Host_Interface_CXL *)host_interface)->request_fetch_unit->Fetch_write_data(request);
		}
	}

	inline void Input_Stream_Manager_CXL::Handle_arrived_write_data(User_Request *request)
	{
		segment_user_request(request);
		((Host_Interface_CXL *)host_interface)->broadcast_user_request_arrival_signal(request);
	}

	inline void Input_Stream_Manager_CXL::Handle_serviced_request(User_Request *request)
	{
		stream_id_type stream_id = request->Stream_id;
		((Input_Stream_CXL *)input_streams[request->Stream_id])->Waiting_user_requests.remove(request);
		((Input_Stream_CXL *)input_streams[stream_id])->On_the_fly_requests--;

		// list<uint64_t>* flush_lba{ new list<uint64_t> };
		((Host_Interface_CXL *)host_interface)->cxl_man->request_serviced(request);

		DEBUG("** Host Interface: Request #" << request->ID << " from stream #" << request->Stream_id << " is finished")
		////If this is a read request, then the read data should be written to host memory
		// if (request->Type == UserRequestType::READ) {
		//	((Host_Interface_CXL*)host_interface)->request_fetch_unit->Send_read_data(request);
		// }

		////there are waiting requests in the submission queue but have not been fetched, due to Queue_fetch_size limit
		// if (((Input_Stream_CXL*)input_streams[stream_id])->Submission_head != ((Input_Stream_CXL*)input_streams[stream_id])->Submission_tail) {
		//	((Host_Interface_CXL*)host_interface)->request_fetch_unit->Fetch_next_request(stream_id);
		//	((Input_Stream_CXL*)input_streams[stream_id])->On_the_fly_requests++;
		//	((Input_Stream_CXL*)input_streams[stream_id])->Submission_head++;//Update submission queue head after starting fetch request
		//	if (((Input_Stream_CXL*)input_streams[stream_id])->Submission_head == ((Input_Stream_CXL*)input_streams[stream_id])->Submission_queue_size) {//Circular queue implementation
		//		((Input_Stream_CXL*)input_streams[stream_id])->Submission_head = 0;
		//	}
		// }

		////Check if completion queue is full
		// if (((Input_Stream_CXL*)input_streams[stream_id])->Completion_head > ((Input_Stream_CXL*)input_streams[stream_id])->Completion_tail) {
		//	//completion queue is full
		//	if (((Input_Stream_CXL*)input_streams[stream_id])->Completion_tail + 1 == ((Input_Stream_CXL*)input_streams[stream_id])->Completion_head) {
		//		((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.push_back(request);//Wait while the completion queue is full
		//		return;
		//	}
		// }
		// else if (((Input_Stream_CXL*)input_streams[stream_id])->Completion_tail - ((Input_Stream_CXL*)input_streams[stream_id])->Completion_head
		//	== ((Input_Stream_CXL*)input_streams[stream_id])->Completion_queue_size - 1) {
		//	((Input_Stream_CXL*)input_streams[stream_id])->Completed_user_requests.push_back(request);//Wait while the completion queue is full
		//	return;
		// }

		// inform_host_request_completed(stream_id, request);//Completion queue is not full, so the device can DMA the completion queue entry to the host
		DELETE_REQUEST_NVME(request);
	}

	uint16_t Input_Stream_Manager_CXL::Get_submission_queue_depth(stream_id_type stream_id)
	{
		return ((Input_Stream_CXL *)this->input_streams[stream_id])->Submission_queue_size;
	}

	uint16_t Input_Stream_Manager_CXL::Get_completion_queue_depth(stream_id_type stream_id)
	{
		return ((Input_Stream_CXL *)this->input_streams[stream_id])->Completion_queue_size;
	}

	IO_Flow_Priority_Class Input_Stream_Manager_CXL::Get_priority_class(stream_id_type stream_id)
	{
		return ((Input_Stream_CXL *)this->input_streams[stream_id])->Priority_class;
	}

	inline void Input_Stream_Manager_CXL::inform_host_request_completed(stream_id_type stream_id, User_Request *request)
	{
	}

	void Input_Stream_Manager_CXL::segment_user_request(User_Request *user_request)
	{
		LHA_type lsa = user_request->Start_LBA;
		LHA_type lsa2 = user_request->Start_LBA;
		unsigned int req_size = user_request->SizeInSectors;
		page_status_type access_status_bitmap = 0;
		unsigned int hanled_sectors_count = 0;
		unsigned int transaction_size = 0;
		if (user_request->Type == UserRequestType::WRITE && user_request->Sector_bitmap != 0)
		{
			unsigned int sector_cnt = count_sector_no_from_status_bitmap(user_request->Sector_bitmap);
			NVM_Transaction_Flash_WR *transaction =
				new NVM_Transaction_Flash_WR(
					Transaction_Source_Type::USERIO,
					user_request->Stream_id,
					sector_cnt * SECTOR_SIZE_IN_BYTE,
					user_request->Start_LBA/ host_interface->sectors_per_subpage,
					user_request,
					0,
					user_request->Sector_bitmap,
					CurrentTimeStamp);
			user_request->Transaction_list.push_back(transaction);
			return;
		}

		while (hanled_sectors_count < req_size)
		{
			// Check if LSA is in the correct range allocted to the stream
			if (lsa < ((Input_Stream_CXL *)input_streams[user_request->Stream_id])->Start_logical_sector_address || lsa > ((Input_Stream_CXL *)input_streams[user_request->Stream_id])->End_logical_sector_address)
			{
				lsa = ((Input_Stream_CXL *)input_streams[user_request->Stream_id])->Start_logical_sector_address + (lsa % (((Input_Stream_CXL *)input_streams[user_request->Stream_id])->End_logical_sector_address - (((Input_Stream_CXL *)input_streams[user_request->Stream_id])->Start_logical_sector_address)));
			}
			LHA_type internal_lsa = lsa - ((Input_Stream_CXL *)input_streams[user_request->Stream_id])->Start_logical_sector_address; // For each flow, all lsa's should be translated into a range starting from zero

#if PATCH_SEGMENT_REQ
			LPA_type lpa;
			if (user_request->Type == UserRequestType::READ)
			{
				transaction_size = host_interface->sectors_per_subpage - (unsigned int)(lsa % host_interface->sectors_per_subpage);
				if (hanled_sectors_count + transaction_size >= req_size)
				{
					transaction_size = req_size - hanled_sectors_count;
				}
				lpa = internal_lsa / host_interface->sectors_per_subpage;

				page_status_type temp = ~(0xffffffffffffffff << (int)transaction_size); //
				access_status_bitmap = temp << (int)(internal_lsa % host_interface->sectors_per_subpage);
				//std::cout<<transaction_size<<" "<< count_sector_no_from_status_bitmap(access_status_bitmap) << std::endl;
				// printf("access_status_bitmap: %lx", access_status_bitmap);
			}
			else if (user_request->Type == UserRequestType::WRITE)
			{
				transaction_size = host_interface->sectors_per_subpage - (unsigned int)(lsa % host_interface->sectors_per_subpage);
				if (hanled_sectors_count + transaction_size >= req_size)
				{
					transaction_size = req_size - hanled_sectors_count;
				}
				lpa = internal_lsa / host_interface->sectors_per_subpage;

				page_status_type temp = ~(0xffffffffffffffff << (int)transaction_size);					  //
				access_status_bitmap = temp << (int)(internal_lsa % host_interface->sectors_per_subpage); // write_sectors_bitmap
																										  // printf("access_status_bitmap: %lx", access_status_bitmap);
			}
		// 	std::cout << "[READ_SEG] lsa=" << lsa
        //   << " internal_lsa=" << internal_lsa
        //   << " tx_size=" << transaction_size
        //   << " bitmap=" << std::bitset<64>(access_status_bitmap)
        //   << std::endl;
			// std::cout << "tr size: " << transaction_size << std::endl;
#else
			transaction_size = host_interface->sectors_per_page - (unsigned int)(lsa % host_interface->sectors_per_page);
			if (hanled_sectors_count + transaction_size >= req_size)
			{
				transaction_size = req_size - hanled_sectors_count;
			}
			LPA_type lpa = internal_lsa / host_interface->sectors_per_page;

			page_status_type temp = ~(0xffffffffffffffff << (int)transaction_size);				   //
			access_status_bitmap = temp << (int)(internal_lsa % host_interface->sectors_per_page); // write_sectors_bitmap
#endif
			if (user_request->Type == UserRequestType::READ)
			{
				NVM_Transaction_Flash_RD *transaction = new NVM_Transaction_Flash_RD(Transaction_Source_Type::USERIO, user_request->Stream_id,
				transaction_size * SECTOR_SIZE_IN_BYTE, lpa, NO_PPA, user_request, 0, access_status_bitmap, CurrentTimeStamp);
				user_request->Transaction_list.push_back(transaction);
				input_streams[user_request->Stream_id]->STAT_number_of_read_transactions++;
			}
			else
			{ // user_request->Type == UserRequestType::WRITE
				// cout << user_request->Start_LBA << endl;
				NVM_Transaction_Flash_WR *transaction = new NVM_Transaction_Flash_WR(Transaction_Source_Type::USERIO, user_request->Stream_id,
																					 transaction_size * SECTOR_SIZE_IN_BYTE, lpa, user_request, 0, access_status_bitmap, CurrentTimeStamp);
				user_request->Transaction_list.push_back(transaction);
				input_streams[user_request->Stream_id]->STAT_number_of_write_transactions++;
			}

			lsa = lsa + transaction_size;
			hanled_sectors_count += transaction_size;
		}
	}

	Request_Fetch_Unit_CXL::Request_Fetch_Unit_CXL(Host_Interface_Base *host_interface) : Request_Fetch_Unit_Base(host_interface), current_phase(0xffff), number_of_sent_cqe(0) {}

	void Request_Fetch_Unit_CXL::Process_pcie_write_message(uint64_t address, void *payload, unsigned int payload_size)
	{
		/*Host_Interface_CXL* hi = (Host_Interface_CXL*)host_interface;
		uint64_t val = (uint64_t)payload;
		switch (address)
		{
		case SUBMISSION_QUEUE_REGISTER_1:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(0, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_1:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(0, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_2:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(1, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_2:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(1, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_3:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(2, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_3:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(2, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_4:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(3, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_4:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(3, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_5:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(4, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_5:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(4, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_6:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(5, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_6:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(5, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_7:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(6, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_7:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(6, (uint16_t)val);
			break;
		case SUBMISSION_QUEUE_REGISTER_8:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Submission_queue_tail_pointer_update(7, (uint16_t)val);
			break;
		case COMPLETION_QUEUE_REGISTER_8:
			((Input_Stream_Manager_CXL*)(hi->input_stream_manager))->Completion_queue_head_pointer_update(7, (uint16_t)val);
			break;
		default:
			throw std::invalid_argument("Unknown register is written!");
		}*/
	}

	void Request_Fetch_Unit_CXL::Process_pcie_read_message(uint64_t address, void *payload, unsigned int payload_size)
	{
		Host_Interface_CXL *hi = (Host_Interface_CXL *)host_interface;

		DMA_Req_Item *dma_req_item = dma_list.front();
		dma_list.pop_front();

		switch (dma_req_item->Type)
		{
		case DMA_Req_Type::REQUEST_INFO:
		{

			User_Request *new_reqeust = new User_Request;
			new_reqeust->IO_command_info = payload;
			new_reqeust->Stream_id = (stream_id_type)((uint64_t)(dma_req_item->object));
			new_reqeust->Priority_class = ((Input_Stream_Manager_CXL *)host_interface->input_stream_manager)->Get_priority_class(new_reqeust->Stream_id);
			new_reqeust->STAT_InitiationTime = Simulator->Time();
			Submission_Queue_Entry *sqe = (Submission_Queue_Entry *)payload;
			new_reqeust->ToBeIgnored = (sqe->Command_Identifier == INTERNAL_FLUSH_CMDID);
			new_reqeust->Sector_bitmap = ((uint64_t)sqe->Command_specific[5] << 32) | (uint64_t)sqe->Command_specific[4];
			
			switch (sqe->Opcode)
			{
			case NVME_READ_OPCODE:
				new_reqeust->Type = UserRequestType::READ;
				new_reqeust->Start_LBA = (((LHA_type)sqe->Command_specific[1]) << 32) | (LHA_type)sqe->Command_specific[0]; // Command Dword 10 and Command Dword 11
				new_reqeust->SizeInSectors = sqe->Command_specific[2] & (LHA_type)(0x0000ffff);
				//new_reqeust->SizeInSectors = ((Host_Interface_CXL*)host_interface)->cxl_man->cxl_config_para.num_sec & (LHA_type)(0x0000ffff);
				new_reqeust->Size_in_byte = new_reqeust->SizeInSectors * SECTOR_SIZE_IN_BYTE;
				hi->cxl_man->flash_read_count++;
				hi->cxl_man->flash_back_end_access_count++;
				break;
			case NVME_WRITE_OPCODE:
				new_reqeust->Type = UserRequestType::WRITE;
				new_reqeust->Start_LBA = (((LHA_type)sqe->Command_specific[1]) << 32) | (LHA_type)sqe->Command_specific[0]; // Command Dword 10 and Command Dword 11
				// cout << new_reqeust->Start_LBA << endl;
				new_reqeust->SizeInSectors = sqe->Command_specific[2] & (LHA_type)(0x0000ffff);
				new_reqeust->Size_in_byte = new_reqeust->SizeInSectors * SECTOR_SIZE_IN_BYTE;
				hi->cxl_man->flash_back_end_access_count++;
			
				break;
			default:
				throw std::invalid_argument("NVMe command is not supported!");
			}
			if (hi->cxl_man->flash_back_end_access_count >= hi->cxl_man->flash_back_end_queue_size)
			{
				hi->Notify_CXL_Host_flash_full();
			}

			((Input_Stream_Manager_CXL *)(hi->input_stream_manager))->Handle_new_arrived_request(new_reqeust);
			break;
		}
		case DMA_Req_Type::WRITE_DATA:
			COPYDATA(((User_Request *)dma_req_item->object)->Data, payload, payload_size);
			((Input_Stream_Manager_CXL *)(hi->input_stream_manager))->Handle_arrived_write_data((User_Request *)dma_req_item->object);
			break;
		default:
			break;
		}
		delete dma_req_item;
	}

	void Request_Fetch_Unit_CXL::Fetch_next_request(stream_id_type stream_id)
	{
		DMA_Req_Item *dma_req_item = new DMA_Req_Item;
		dma_req_item->Type = DMA_Req_Type::REQUEST_INFO;
		dma_req_item->object = (void *)(intptr_t)stream_id;
		dma_list.push_back(dma_req_item);

		Host_Interface_CXL *hi = (Host_Interface_CXL *)host_interface;
		Input_Stream_CXL *im = ((Input_Stream_CXL *)hi->input_stream_manager->input_streams[stream_id]);
		// host_interface->Send_read_message_to_host(im->Submission_queue_base_address + im->Submission_head * sizeof(Submission_Queue_Entry), sizeof(Submission_Queue_Entry));
	}

	void Request_Fetch_Unit_CXL::Fetch_write_data(User_Request *request)
	{
		DMA_Req_Item *dma_req_item = new DMA_Req_Item;
		dma_req_item->Type = DMA_Req_Type::WRITE_DATA;
		dma_req_item->object = (void *)request;
		dma_list.push_back(dma_req_item);

		Submission_Queue_Entry *sqe = (Submission_Queue_Entry *)request->IO_command_info;

		Process_pcie_read_message(0, NULL, 4096); // immediate send data

		// host_interface->Send_read_message_to_host((sqe->PRP_entry_2 << 31) | sqe->PRP_entry_1, request->Size_in_byte);
	}

	void Request_Fetch_Unit_CXL::Send_completion_queue_element(User_Request *request, uint16_t sq_head_value)
	{
		// Host_Interface_CXL* hi = (Host_Interface_CXL*)host_interface;
		// Completion_Queue_Entry* cqe = new Completion_Queue_Entry;
		// cqe->SQ_Head = sq_head_value;
		// cqe->SQ_ID = FLOW_ID_TO_Q_ID(request->Stream_id);
		// cqe->SF_P = 0x0001 & current_phase;
		// cqe->Command_Identifier = ((Submission_Queue_Entry*)request->IO_command_info)->Command_Identifier;
		// Input_Stream_CXL* im = ((Input_Stream_CXL*)hi->input_stream_manager->input_streams[request->Stream_id]);
		// host_interface->Send_write_message_to_host(im->Completion_queue_base_address + im->Completion_tail * sizeof(Completion_Queue_Entry), cqe, sizeof(Completion_Queue_Entry));
		// number_of_sent_cqe++;
		// if (number_of_sent_cqe % im->Completion_queue_size == 0)
		//{
		//	//According to protocol specification, the value of the Phase Tag is inverted each pass through the Completion Queue
		//	if (current_phase == 0xffff) {
		//		current_phase = 0xfffe;
		//	}
		//	else {
		//		current_phase = 0xffff;
		//	}
		// }
	}

	void Request_Fetch_Unit_CXL::Send_read_data(User_Request *request)
	{
		// Submission_Queue_Entry* sqe = (Submission_Queue_Entry*)request->IO_command_info;
		// host_interface->Send_write_message_to_host(sqe->PRP_entry_1, request->Data, request->Size_in_byte);
	}

	Host_Interface_CXL::Host_Interface_CXL(const sim_object_id_type &id,
										   LHA_type max_logical_sector_address, uint16_t submission_queue_depth, uint16_t completion_queue_depth,
										   unsigned int no_of_input_streams, uint16_t queue_fetch_size, unsigned int sectors_per_page, Data_Cache_Manager_Base *cache, CXL_DRAM_Model *cxl_dram) : Host_Interface_Base(id, HostInterface_Types::NVME, max_logical_sector_address, sectors_per_page, cache),
																																																   submission_queue_depth(submission_queue_depth), completion_queue_depth(completion_queue_depth), no_of_input_streams(no_of_input_streams), cxl_dram(cxl_dram)
	{
		this->input_stream_manager = new Input_Stream_Manager_CXL(this, queue_fetch_size);
		this->request_fetch_unit = new Request_Fetch_Unit_CXL(this);

		this->cxl_man = new CXL_Manager((Host_Interface_CXL *)this);
	}

	Host_Interface_CXL::~Host_Interface_CXL()
	{
		delete cxl_man;
		delete cxl_dram;
	}

	stream_id_type Host_Interface_CXL::Create_new_stream(IO_Flow_Priority_Class priority_class, LHA_type start_logical_sector_address, LHA_type end_logical_sector_address,
	uint64_t submission_queue_base_address, uint64_t completion_queue_base_address)
{
		return ((Input_Stream_Manager_CXL *)input_stream_manager)->Create_new_stream(priority_class, start_logical_sector_address, end_logical_sector_address, submission_queue_base_address, submission_queue_depth, completion_queue_base_address, completion_queue_depth);
	}

	void Host_Interface_CXL::Validate_simulation_config()
	{
		Host_Interface_Base::Validate_simulation_config();
		if (this->input_stream_manager == NULL)
		{
			throw std::logic_error("Input stream manager is not set for Host Interface");
		}
		if (this->request_fetch_unit == NULL)
		{
			throw std::logic_error("Request fetch unit is not set for Host Interface");
		}
	}

	void Host_Interface_CXL::Start_simulation()
	{
	}

	void Host_Interface_CXL::Execute_simulator_event(MQSimEngine::Sim_Event *event)
	{
		// this->request_fetch_unit->Process_pcie_read_message(0, NULL, 4096); //initiate when there is a write request

		// if (Simulator->Time() == 210411353) {
		//	cout << " Check" << endl;
		// }

		if (!cxl_man->cxl_config_para.has_mshr)
		{
			while (cxl_dram->getDRAMAvailability() > 0)
			{
				if (cxl_man->no_mshr_not_yet_serviced_lba.empty())
					break;
				uint64_t lba{cxl_man->no_mshr_not_yet_serviced_lba.front()};
				cxl_man->no_mshr_not_yet_serviced_lba.pop_front();
				no_mshr_record_node n{cxl_man->no_mshr_requests_record[lba].front()};
				cxl_man->no_mshr_requests_record[lba].pop_front();
				CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{static_cast<unsigned int>(64), lba, n.rw, CXL_DRAM_EVENTS::CACHE_MISS, n.time}};
				Send_request_to_CXL_DRAM(dram_request);

				list<uint64_t> *flush_lba{new list<uint64_t>};

				page_status_type sector_bitmap = ((Address_Mapping_Unit_Page_Level*) ((FTL*)(this->cxl_man->hi->cache->nvm_firmware))->Address_Mapping_Unit)->domains[0]->Get_page_status(true, 0 ,lba);
			
				uint64_t cacheline_bitmap = this->cxl_man->sector_mask_to_cacheline_mask(sector_bitmap);

				this->cxl_man->record_pending_admission_valid(lba, cacheline_bitmap, sector_bitmap);
				this->cxl_man->dram->process_miss_data_ready_new(n.rw, lba, flush_lba, Simulator->Time(), this->cxl_man->prefetched_lba, this->cxl_man->tagAssertedLBA, this->cxl_man->prefetch_pollution_tracker, cxl_man->serviced_before_lba,cacheline_bitmap);

				if (cxl_man->flash_back_end_access_count >= cxl_man->flash_back_end_queue_size)
				{
					Notify_CXL_Host_flash_not_full();
				}
				cxl_man->flash_back_end_access_count--;

				if (cxl_man->no_mshr_requests_record[lba].empty())
				{
					cxl_man->no_mshr_requests_record.erase(cxl_man->no_mshr_requests_record.find(lba));
				}

				while (!flush_lba->empty())
				{

					// ofFlush << static_cast<float>(cxl_man->total_number_of_accesses) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) * 100 << endl;

					cxl_man->flush_count++;
					uint64_t lba{flush_lba->front()};

					
					uint64_t lsa{lba * 8};

					flush_lba->pop_front();
					Submission_Queue_Entry *sqe{new Submission_Queue_Entry};
					sqe->Command_Identifier = INTERNAL_FLUSH_CMDID;
					sqe->Opcode = NVME_WRITE_OPCODE;

					sqe->Command_specific[0] = (uint32_t)lsa;
					sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
					uint64_t mask = ((uint32_t)((uint16_t) cxl_man->cxl_config_para.num_sec)) & (uint32_t)(0x0000ffff);
					auto it = cxl_man->dram->write_map->find(lba);
					if (it != cxl_man->dram->write_map->end())
					{
						mask = it->second;
						cxl_man->dram->write_map->erase(it);
					}
					sqe->Command_specific[2] = ((uint32_t)((uint16_t)cxl_man->count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff);

					sqe->Command_specific[3] = 0; // flush  caued by read miss;
					uint64_t sector_bitmap = cxl_man->cacheline_mask_to_sector_mask(mask);
					sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
					sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
					sqe->Reserved = 0;
					sqe->PRP_entry_1 = (DATA_MEMORY_REGION);		  // Dummy addresses, just to emulate data read/write access
					sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000); // Dummy addresses
					((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Fetch_next_request(0);

					((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
				}

				delete flush_lba;
			}

			if (cxl_dram->getDRAMAvailability() > 0)
			{
				Notify_host_DRAM_is_free();
			}

			return;
		}

		while (cxl_dram->getDRAMAvailability() > 0)
		{
			if (cxl_man->serviced_before_lba.size() > 0)
			{
				uint64_t lba{*(cxl_man->serviced_before_lba.begin())};
				bool completely_removed{0};
				Update_CXL_DRAM_state_when_miss_data_ready(1, lba, 1, completely_removed);
				if (completely_removed)
				{
					cxl_man->serviced_before_lba.erase(cxl_man->serviced_before_lba.find(lba));
				}
			}
			else if (cxl_man->not_yet_serviced_lba.size() > 0)
			{
				uint64_t lba{*(cxl_man->not_yet_serviced_lba.begin())};
				bool completely_removed{0};
				Update_CXL_DRAM_state_when_miss_data_ready(1, lba, 0, completely_removed);
				cxl_man->not_yet_serviced_lba.erase(cxl_man->not_yet_serviced_lba.find(lba));
				if (!completely_removed)
				{
					cxl_man->serviced_before_lba.emplace(lba);
				}
			}

			if (cxl_man->serviced_before_lba.size() == 0 && cxl_man->not_yet_serviced_lba.size() == 0)
			{
				break;
			}
		}

		if (cxl_dram->getDRAMAvailability() > 0)
		{
			Notify_host_DRAM_is_free();
		}
	}

	uint16_t Host_Interface_CXL::Get_submission_queue_depth()
	{
		return submission_queue_depth;
	}

	uint16_t Host_Interface_CXL::Get_completion_queue_depth()
	{
		return completion_queue_depth;
	}

	void Host_Interface_CXL::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter &xmlwriter)
	{
		std::string tmp = name_prefix + ".HostInterface";
		xmlwriter.Write_open_tag(tmp);

		std::string attr = "Name";
		std::string val = ID();
		xmlwriter.Write_attribute_string(attr, val);

		for (unsigned int stream_id = 0; stream_id < no_of_input_streams; stream_id++)
		{
			std::string tmp = name_prefix + ".IO_Stream";
			xmlwriter.Write_open_tag(tmp);

			attr = "Stream_ID";
			val = std::to_string(stream_id);
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Read_Transaction_Turnaround_Time";
			val = std::to_string(input_stream_manager->Get_average_read_transaction_turnaround_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Read_Transaction_Execution_Time";
			val = std::to_string(input_stream_manager->Get_average_read_transaction_execution_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Read_Transaction_Transfer_Time";
			val = std::to_string(input_stream_manager->Get_average_read_transaction_transfer_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Read_Transaction_Waiting_Time";
			val = std::to_string(input_stream_manager->Get_average_read_transaction_waiting_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Write_Transaction_Turnaround_Time";
			val = std::to_string(input_stream_manager->Get_average_write_transaction_turnaround_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Write_Transaction_Execution_Time";
			val = std::to_string(input_stream_manager->Get_average_write_transaction_execution_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Write_Transaction_Transfer_Time";
			val = std::to_string(input_stream_manager->Get_average_write_transaction_transfer_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			attr = "Average_Write_Transaction_Waiting_Time";
			val = std::to_string(input_stream_manager->Get_average_write_transaction_waiting_time(stream_id));
			xmlwriter.Write_attribute_string(attr, val);

			xmlwriter.Write_close_tag();
		}

		xmlwriter.Write_close_tag();
	}
	void Host_Interface_CXL::print_prefetch_info()
	{
		std::cout << "Flush count: " << cxl_man->flush_count << endl;
		of_overall << "Flush count: " << cxl_man->flush_count << endl;
		std::cout << "Admission granularity count (1/2/4/8/16/32/64 cachelines): "
			<< cxl_man->admission_granularity_count[0] << ", "
			<< cxl_man->admission_granularity_count[1] << ", "
			<< cxl_man->admission_granularity_count[2] << ", "
			<< cxl_man->admission_granularity_count[3] << ", "
			<< cxl_man->admission_granularity_count[4] << ", "
			<< cxl_man->admission_granularity_count[5] << ", "
			<< cxl_man->admission_granularity_count[6] << endl;
		of_overall << "Admission granularity count (1/2/4/8/16/32/64 cachelines): "
			<< cxl_man->admission_granularity_count[0] << ", "
			<< cxl_man->admission_granularity_count[1] << ", "
			<< cxl_man->admission_granularity_count[2] << ", "
			<< cxl_man->admission_granularity_count[3] << ", "
			<< cxl_man->admission_granularity_count[4] << ", "
			<< cxl_man->admission_granularity_count[5] << ", "
			<< cxl_man->admission_granularity_count[6] << endl;
		double avg_admission_cachelines = cxl_man->admission_decision_count == 0 ? 0.0 :
			(double)cxl_man->admission_cacheline_total / (double)cxl_man->admission_decision_count;
		double avg_admission_backend_sectors = cxl_man->admission_decision_count == 0 ? 0.0 :
			(double)cxl_man->admission_backend_sector_total / (double)cxl_man->admission_decision_count;
		double avg_admission_valid_cachelines = cxl_man->admission_valid_sample_count == 0 ? 0.0 :
			(double)cxl_man->admission_inserted_cacheline_count / (double)cxl_man->admission_valid_sample_count;
		double avg_admission_valid_sectors = cxl_man->admission_valid_sample_count == 0 ? 0.0 :
			(double)cxl_man->admission_valid_sector_total / (double)cxl_man->admission_valid_sample_count;
		std::cout << "Admission decision count: " << cxl_man->admission_decision_count << endl;
		of_overall << "Admission decision count: " << cxl_man->admission_decision_count << endl;
		std::cout << "Admission average cachelines: " << avg_admission_cachelines << endl;
		of_overall << "Admission average cachelines: " << avg_admission_cachelines << endl;
		std::cout << "Admission backend sector total: " << cxl_man->admission_backend_sector_total << endl;
		of_overall << "Admission backend sector total: " << cxl_man->admission_backend_sector_total << endl;
		std::cout << "Admission average backend sectors: " << avg_admission_backend_sectors << endl;
		of_overall << "Admission average backend sectors: " << avg_admission_backend_sectors << endl;
		std::cout << "Admission inserted cachelines: " << cxl_man->admission_inserted_cacheline_count << endl;
		of_overall << "Admission inserted cachelines: " << cxl_man->admission_inserted_cacheline_count << endl;
		std::cout << "Admission valid sample count: " << cxl_man->admission_valid_sample_count << endl;
		of_overall << "Admission valid sample count: " << cxl_man->admission_valid_sample_count << endl;
		std::cout << "Admission valid sector total: " << cxl_man->admission_valid_sector_total << endl;
		of_overall << "Admission valid sector total: " << cxl_man->admission_valid_sector_total << endl;
		std::cout << "Admission average valid cachelines: " << avg_admission_valid_cachelines << endl;
		of_overall << "Admission average valid cachelines: " << avg_admission_valid_cachelines << endl;
		std::cout << "Admission average valid sectors: " << avg_admission_valid_sectors << endl;
		of_overall << "Admission average valid sectors: " << avg_admission_valid_sectors << endl;
		double avg_host_response_us = 0.0;
		double avg_host_read_response_us = 0.0;
		double avg_host_write_response_us = 0.0;
		// if (cxl_man->host_request_latency_count > 0)
		// {
		// 	avg_host_response_us = (double)cxl_man->host_request_latency_sum / (double)cxl_man->host_request_latency_count / (double)SIM_TIME_TO_MICROSECONDS_COEFF;
		// }
		if (cxl_man->host_read_request_latency_count > 0)
		{
			avg_host_read_response_us = (double)cxl_man->host_read_request_latency_sum / (double)cxl_man->host_read_request_latency_count / (double)SIM_TIME_TO_MICROSECONDS_COEFF;
		}
		if (cxl_man->host_write_request_latency_count > 0)
		{
			avg_host_write_response_us = (double)cxl_man->host_write_request_latency_sum / (double)cxl_man->host_write_request_latency_count / (double)SIM_TIME_TO_MICROSECONDS_COEFF;
		}

		if (cxl_man->host_write_request_latency_count +cxl_man->host_read_request_latency_count > 0)
		{
			avg_host_response_us = (double)(cxl_man->host_write_request_latency_sum+cxl_man->host_read_request_latency_sum ) / (cxl_man->host_write_request_latency_count +cxl_man->host_read_request_latency_count)/(double)1000;
		}
		std::cout << "Average host request response time (us): " << avg_host_response_us << endl;
		of_overall << "Average host request response time (us): " << avg_host_response_us << endl;
		std::cout << "Average host read response time (us): " << avg_host_read_response_us << endl;
		of_overall << "Average host read response time (us): " << avg_host_read_response_us << endl;
		std::cout << "Average host write response time (us): " << avg_host_write_response_us << endl;
		of_overall << "Average host write response time (us): " << avg_host_write_response_us << endl;
		std::cout << "Eviction count: " << cxl_man->dram->eviction_count << endl;
		of_overall << "Eviction count: " << cxl_man->dram->eviction_count << endl;

		std::cout<<"average_access_interval: "<<(double)cxl_man->average_interval/1000/1000 / ((double)cxl_man->interval_count-1)<< " us" << endl;

		of_overall<<"average_access_interval: "<<(double)cxl_man->average_interval/1000/1000 / ((double)cxl_man->interval_count-1)<< " us" << endl;

		std::cout<<"read ratio: "<<(double)cxl_man->host_read_request_latency_count/(double)(cxl_man->host_read_request_latency_count+cxl_man->host_write_request_latency_count)<<endl;

		of_overall<<"read ratio: "<<(double)cxl_man->host_read_request_latency_count/(double)(cxl_man->host_read_request_latency_count+cxl_man->host_write_request_latency_count)<<endl;

		std::cout<<"total requests: "<< cxl_man->host_read_request_latency_count+cxl_man->host_write_request_latency_count<<std::endl;

		of_overall<<"total requests: "<< cxl_man->host_read_request_latency_count+cxl_man->host_write_request_latency_count<<std::endl;

		double avg_gc_page_movements = Stats::Total_gc_executions == 0 ? 0.0 :
			static_cast<double>(Stats::Total_page_movements_for_gc) / static_cast<double>(Stats::Total_gc_executions);
		std::cout << "GC count: " << Stats::GC_count << endl;
		of_overall << "GC count: " << Stats::GC_count << endl;
		std::cout << "GC executions: " << Stats::Total_gc_executions << endl;
		of_overall << "GC executions: " << Stats::Total_gc_executions << endl;
		std::cout << "GC page movements: " << Stats::Total_page_movements_for_gc << endl;
		of_overall << "GC page movements: " << Stats::Total_page_movements_for_gc << endl;
		std::cout << "Average GC page movements: " << avg_gc_page_movements << endl;
		of_overall << "Average GC page movements: " << avg_gc_page_movements << endl;


		std::cout << "Request ends at timestamp: " << static_cast<float>(Simulator->Time()) / 1000000000 << " s" << endl;
		of_overall << "Request ends at timestamp: " << static_cast<float>(Simulator->Time()) / 1000000000 << " s" << endl;
		std::cout << "Repeated flash access count: " << cxl_man->repeated_flash_access_count << endl;
		of_overall << "Repeated flash access count: " << cxl_man->repeated_flash_access_count << endl;
		if (PREFETCH_INFO_MAP.size() == 0)
			return;

		uint64_t accurate_prefetch{0};
		uint64_t late_prefetch{0};

		for (auto &i : PREFETCH_INFO_MAP)
		{
			if (i.second.hit_count > 0)
			{
				accurate_prefetch++;
				if (i.second.late)
				{
					late_prefetch++;
				}
			}
		}
		// uint64_t accurate_prefetch_sample{ 0 };
		// uint64_t late_prefetch_sample{ 0 };
		// for (auto& i : PREFETCH_INFO_MAP_SAMPLE) {
		//	if (i.second.hit_count > 0) {
		//		accurate_prefetch_sample++;
		//		if (i.second.late) {
		//			late_prefetch_sample++;
		//		}
		//	}
		// }
		/*of_overall << "Prefetch Coverage (sampled): " << static_cast<float>(cxl_man->prefetch_hit_count - LAST_PREFETCH) / static_cast<float>(43000000)<<endl;
		of_overall << "Prefetch Accuracy (sampled): " << static_cast<float>(accurate_prefetch_sample) / static_cast<float>(PREFETCH_INFO_MAP_SAMPLE.size()+1) << endl;
		of_overall << "Prefetch Lateness (sampled): " << static_cast<float>(late_prefetch_sample) / static_cast<float>(accurate_prefetch_sample+1) << endl;
		of_overall << "Prefetch Pollution (sampled): " << static_cast<float>(cxl_man->prefetch_pollution_count - LAST_POLLUTION) / static_cast<float>(cxl_man->cache_miss_count - LAST_CACHE_MISS +1) << endl;
		LAST_POLLUTION = cxl_man->prefetch_pollution_count;
		LAST_CACHE_MISS = cxl_man->cache_miss_count;
		LAST_PREFETCH = cxl_man->prefetch_hit_count;
		PREFETCH_INFO_MAP_SAMPLE.clear();*/

		std::cout << "Prefetch Coverage: " << static_cast<float>(cxl_man->prefetch_hit_count) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) << endl;
		of_overall << "Prefetch Coverage: " << static_cast<float>(cxl_man->prefetch_hit_count) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) << endl;
		std::cout << "Prefetch Accuracy: " << static_cast<float>(accurate_prefetch) / static_cast<float>(PREFETCH_INFO_MAP.size()) << endl;
		of_overall << "Prefetch Accuracy: " << static_cast<float>(accurate_prefetch) / static_cast<float>(PREFETCH_INFO_MAP.size()) << endl;
		std::cout << "Prefetch Lateness: " << static_cast<float>(late_prefetch) / static_cast<float>(accurate_prefetch) << endl;
		of_overall << "Prefetch Lateness: " << static_cast<float>(late_prefetch) / static_cast<float>(accurate_prefetch) << endl;
		std::cout << "Prefetch Pollution: " << static_cast<float>(cxl_man->prefetch_pollution_count) / static_cast<float>(cxl_man->cache_miss_count) << endl;
		of_overall << "Prefetch Pollution: " << static_cast<float>(cxl_man->prefetch_pollution_count) / static_cast<float>(cxl_man->cache_miss_count) << endl;
	}

	void Host_Interface_CXL::Update_CXL_DRAM_state_when_miss_data_ready(bool rw, uint64_t lba, bool serviced_before, bool &completed_removed_from_mshr)
	{
		list<uint64_t> readcount, writecount;
		mshr_request *first_entry{NULL};
		bool wasfull{0};
		uint64_t dram_avail{this->cxl_dram->getDRAMAvailability()};
		bool completely_removed{0};

		if (this->cxl_man->mshr->isInProgress(lba))
		{
			first_entry = this->cxl_man->mshr->removeRequestNew(lba, readcount, writecount, wasfull, dram_avail, serviced_before, completely_removed);
			if (wasfull)
			{
				// notify cxl pcie  no longer full
				this->Notify_CXL_Host_mshr_not_full();
			}
		}
		else
		{
			completely_removed = 1; // for prefetched data
		}

		if (!serviced_before)
		{
			// CXL_DRAM_EVENTS evt{ CXL_DRAM_EVENTS::CACHE_MISS };
			// if (this->cxl_man->in_progress_prefetch_lba->count(lba)) {
			//	evt = CXL_DRAM_EVENTS::PREFETCH_READY;
			// }

			CXL_DRAM_EVENTS evt{CXL_DRAM_EVENTS::CACHE_MISS};
			if (first_entry == NULL)
			{ // disregard slow prefetch
				evt = CXL_DRAM_EVENTS::PREFETCH_READY;

				if (PREFETCH_INFO_MAP.count(lba) == 0)
				{
					prefetch_info_node n;
					PREFETCH_INFO_MAP.emplace(lba, n);
				}
				if (PREFETCH_INFO_MAP_SAMPLE.count(lba) == 0)
				{
					prefetch_info_node n;
					PREFETCH_INFO_MAP_SAMPLE.emplace(lba, n);
				}
			}
			else
			{
				if (this->cxl_man->in_progress_prefetch_lba->count(lba))
				{
					evt = CXL_DRAM_EVENTS::SLOW_PREFETCH;
					oflatep << first_entry->time << " " << Simulator->Time() << endl;
					if (PREFETCH_INFO_MAP.count(lba) == 0)
					{
						prefetch_info_node n;
						n.late = 1;
						n.hit_count++;
						PREFETCH_INFO_MAP.emplace(lba, n);
						PREFETCH_ACCURACY_INFO.emplace(lba);
						PREFETCH_LATE_INFO.emplace(lba);
					}
					else
					{
						PREFETCH_INFO_MAP[lba].late = 1;
						PREFETCH_INFO_MAP[lba].hit_count++;
						PREFETCH_ACCURACY_INFO.emplace(lba);
						PREFETCH_LATE_INFO.emplace(lba);
					}
					if (PREFETCH_INFO_MAP_SAMPLE.count(lba) == 0)
					{
						prefetch_info_node n;
						n.late = 1;
						n.hit_count++;
						PREFETCH_INFO_MAP_SAMPLE.emplace(lba, n);
					}
					else
					{
						PREFETCH_INFO_MAP_SAMPLE[lba].late = 1;
						PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
					}
				}
			}

			sim_time_type itime{0};
			if (first_entry == NULL)
			{
				itime = (*(this->cxl_man->in_progress_prefetch_lba))[lba];
			}
			else
			{

				itime = first_entry->time;
			}

			CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{static_cast<unsigned int>(64), lba, rw, evt, itime}};
			Send_request_to_CXL_DRAM(dram_request);

			if (first_entry)
				rw = (first_entry->sqe->Opcode == NVME_READ_OPCODE) ? 1 : 0;
			else
				rw = 1;

			list<uint64_t> *flush_lba{new list<uint64_t>};

			page_status_type sector_bitmap = ((Address_Mapping_Unit_Page_Level*) ((FTL*)(this->cxl_man->hi->cache->nvm_firmware))->Address_Mapping_Unit)->domains[0]->Get_page_status(true, 0 ,lba);

			uint64_t cacheline_bitmap = this->cxl_man->sector_mask_to_cacheline_mask(sector_bitmap);

			this->cxl_man->record_pending_admission_valid(lba, cacheline_bitmap, sector_bitmap);
			this->cxl_man->dram->process_miss_data_ready_new(rw, lba, flush_lba, Simulator->Time(), this->cxl_man->prefetched_lba, this->cxl_man->tagAssertedLBA, this->cxl_man->prefetch_pollution_tracker, cxl_man->serviced_before_lba,cacheline_bitmap);


			if (cxl_man->flash_back_end_access_count >= cxl_man->flash_back_end_queue_size)
			{
				Notify_CXL_Host_flash_not_full();
			}
			cxl_man->flash_back_end_access_count--;

			if (this->cxl_man->in_progress_prefetch_lba->count(lba))
			{
				this->cxl_man->in_progress_prefetch_lba->erase(this->cxl_man->in_progress_prefetch_lba->find(lba));
			}

			while (!flush_lba->empty())
			{

				// ofFlush << static_cast<float>(cxl_man->total_number_of_accesses) / static_cast<float>(cxl_man->cxl_config_para.total_number_of_requets) * 100 << endl;
				cxl_man->flush_count++;
				uint64_t lba{flush_lba->front()};
				uint64_t lsa{lba * 8};

				flush_lba->pop_front();
				Submission_Queue_Entry *sqe{new Submission_Queue_Entry};
				sqe->Command_Identifier = 0;
				sqe->Opcode = NVME_WRITE_OPCODE;
				sqe->Command_specific[0] = (uint32_t)lsa;
				sqe->Command_specific[1] = (uint32_t)(lsa >> 32);
				uint64_t mask = ((uint32_t)((uint16_t) cxl_man->cxl_config_para.num_sec)) & (uint32_t)(0x0000ffff);
				auto it = cxl_man->dram->write_map->find(lba);
				if (it != cxl_man->dram->write_map->end())
				{
					mask = it->second;
					cxl_man->dram->write_map->erase(it);
				}
				sqe->Command_specific[2] = ((uint32_t)((uint16_t)cxl_man->count_sectors_from_cacheline_mask(mask))) & (uint32_t)(0x0000ffff);
				sqe->Command_specific[3] = 0; // flush  caued by read miss;
				uint64_t sector_bitmap = cxl_man->cacheline_mask_to_sector_mask(mask);
				sqe->Command_specific[4] = (uint32_t)(sector_bitmap & 0xFFFFFFFFULL);
				sqe->Command_specific[5] = (uint32_t)((sector_bitmap >> 32) & 0xFFFFFFFFULL);
				sqe->Reserved = 0;
				sqe->PRP_entry_1 = (DATA_MEMORY_REGION);		  // Dummy addresses, just to emulate data read/write access
				sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000); // Dummy addresses

				((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Fetch_next_request(0);

				((Request_Fetch_Unit_CXL *)(this->request_fetch_unit))->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
			}

			delete flush_lba;
			delete first_entry;
		}

		for (auto i : readcount)
		{
			CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, 1, CXL_DRAM_EVENTS::CACHE_HIT_UNDER_MISS, i}};
			this->Send_request_to_CXL_DRAM(dram_request);

			bool falsehit{0};
			Update_CXL_DRAM_state(1, lba, falsehit, (~0ULL));
			if (falsehit)
				cxl_man->falsehitcount++;

			if (cxl_man->prefetched_lba->count(lba))
			{
				PREFETCH_INFO_MAP[lba].hit_count++;
				PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
				PREFETCH_ACCURACY_INFO.emplace(lba);
			}
		}

		for (auto i : writecount)
		{
			CXL_DRAM_ACCESS *dram_request{new CXL_DRAM_ACCESS{64, lba, 0, CXL_DRAM_EVENTS::CACHE_HIT_UNDER_MISS, i}};
			this->Send_request_to_CXL_DRAM(dram_request);

			bool falsehit{0};
			Update_CXL_DRAM_state(0, lba, falsehit, (~0ULL));
			if (falsehit)
				cxl_man->falsehitcount++;

			if (cxl_man->prefetched_lba->count(lba))
			{
				PREFETCH_INFO_MAP[lba].hit_count++;
				PREFETCH_INFO_MAP_SAMPLE[lba].hit_count++;
				PREFETCH_ACCURACY_INFO.emplace(lba);
			}
		}
		completed_removed_from_mshr = completely_removed;
	}

	void Host_Interface_CXL::process_CXL_prefetch_requests(list<uint64_t> prefetchlba)
	{

		while (!prefetchlba.empty())
		{
			uint64_t lba{prefetchlba.front()};

			prefetchlba.pop_front();

			Submission_Queue_Entry *sqe = new Submission_Queue_Entry;
			sqe->Command_Identifier = 0;
			sqe->Opcode = NVME_READ_OPCODE;
			sqe->Command_specific[0] = (uint32_t)(lba * cxl_man->cxl_config_para.num_sec); // cxl_man->process_requests will do a translation
			sqe->Command_specific[1] = (uint32_t)((lba * cxl_man->cxl_config_para.num_sec) >> 32);
			sqe->Command_specific[2] = ((uint32_t)((uint16_t)cxl_man->cxl_config_para.num_sec)) & (uint32_t)(0x0000ffff); // magic number
			sqe->PRP_entry_1 = (DATA_MEMORY_REGION);																	  // Dummy addresses, just to emulate data read/write access
			sqe->PRP_entry_2 = (DATA_MEMORY_REGION + 0x1000);															  // Dummy addresses

			cxl_man->in_progress_prefetch_lba->emplace(lba, Simulator->Time());
			request_fetch_unit->Fetch_next_request(0);

			request_fetch_unit->Process_pcie_read_message(0, sqe, sizeof(Submission_Queue_Entry));
		}
	}

}
