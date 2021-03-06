/*
 * dynamic_reconfiguration.c
 *
 * Created on: 20/05/2014  
 *
 * =========================================================================
 *  Copyright (C) 2012-2014, Daniele De Sensi (d.desensi.software@gmail.com)
 *
 *  This file is part of Peafowl.
 *
 *  Peafowl is free software: you can redistribute it and/or
 *  modify it under the terms of the Lesser GNU General Public
 *  License as published by the Free Software Foundation, either
 *  version 3 of the License, or (at your option) any later version.

 *  Peafowl is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  Lesser GNU General Public License for more details.
 *
 *  You should have received a copy of the Lesser GNU General Public
 *  License along with Peafowl.
 *  If not, see <http://www.gnu.org/licenses/>.
 *
 * =========================================================================
 */


#define _POSIX_C_SOURCE 1
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <typeinfo>
#include "timer.h"
#include "trie.h"
#include "signatures.h"
#include <cinttypes>

#include "mc_api.h"
#include <pcap.h>
#include <netinet/in.h>
#include <net/ethernet.h>
#include <stdint.h>
#include <ff/mapping_utils.hpp>
#include <ff/utils.hpp>
#include <ff/ubuffer.hpp>
 
using namespace antivirus;
#define CAPACITY_CHUNK 1000
#define SCANNER_POOL_SIZE 4096

#define CLOCK_FREQ 2000000000L

static ff::uSWSR_Ptr_Buffer* scanner_pool;

static int terminating;

static unsigned int intervals;
static double* rates;
static double* durations;
static unsigned int stats_collection_interval;
static u_int32_t current_interval=0;
static time_t last_sec=0;
static time_t start_time=0;
static double current_real_rate=0;
static u_int64_t processed_packets=0;

typedef struct pcap_packets_memory{
  unsigned char** packets;
  u_int32_t* sizes;
  u_int32_t num_packets;
  u_int32_t next_packet_to_sent;
}pcap_packets_memory_t;

inline ticks ticks_wait(ticks nticks) {
  ticks delta;
  ticks t0 = getticks();
  do { delta = (getticks()) - t0; } while (delta < nticks);
  return delta-nticks;
}

double getmstime(){
  struct timeval  tv;
  gettimeofday(&tv, NULL);

  return (tv.tv_sec) * 1000 + (tv.tv_usec) / 1000 ;
}
 
#define BURST_SIZE 10.0


#define CLOCK_RESYNC 10
void* clock_thread(void*){
  int i = 0;
  time_t tmp;
  ff_mapThreadToCpu(0, -20);
  last_sec = time(NULL);
  while(!terminating){
    sleep(1);
    if(i++ == CLOCK_RESYNC){
      i = 0;
      tmp = time(NULL);
      if(tmp>=last_sec){
	last_sec = tmp;
      }
    }else{
      ++last_sec;
    }
  }
  return NULL;
}


mc_dpi_packet_reading_result_t reading_cb(void* user_data){
  static time_t current_interval_start=0;
  static u_int32_t current_burst_size = 0;
  static ticks excess = 0;
  static ticks def = getticks();
  static double start_interval_ms;
 
  mc_dpi_packet_reading_result_t r;
 
  pcap_packets_memory_t* packets=(pcap_packets_memory_t*) user_data;

  if(packets->next_packet_to_sent==packets->num_packets){
    packets->next_packet_to_sent=0;
  }
 
  if(current_interval < intervals){
    r.pkt=packets->packets[packets->next_packet_to_sent];
  }else{
    r.pkt=NULL;
    terminating=1;
    printf("Sending EOS!\n");
    fflush(stdout);
  }

  if(current_burst_size == BURST_SIZE){
    /** Sleep to get the rate. **/
    double wait_interval_secs = 1.0 / rates[current_interval];
    ticks ticks_to_sleep = ((double)CLOCK_FREQ * wait_interval_secs * (double) BURST_SIZE);

    current_burst_size = 0;

    excess += (getticks()-def);

    if(excess >= ticks_to_sleep){
      excess = 0;
      //excess -= ticks_to_sleep;
    }else{
      excess = ticks_wait(ticks_to_sleep - excess);
    }
   
    def = getticks();
  }


  ++current_burst_size;

  if(current_interval_start == 0){
    current_interval_start = last_sec;
    start_interval_ms = getmstime();
  }

  ++processed_packets;

  /** Go to the next rate **/
  if(last_sec - current_interval_start >= durations[current_interval]){
    current_interval_start = last_sec;
    current_interval++;
    start_interval_ms = getmstime();
  }
 
  r.current_time=last_sec;
  r.length=packets->sizes[packets->next_packet_to_sent];
  ++packets->next_packet_to_sent;

  return r;
}
 
void processing_cb(mc_dpi_processing_result_t* processing_result,
                   void* user_data){
  ;
}
 
static void match_found(string::size_type position,
                        trie::value_type const &match){
  using namespace std;
  cout << "Matched '" << match.second << "' at " << position << endl;
}
 

static void load_rates(char* fileName){
  FILE* f = NULL;
  char line[512];
  f = fopen(fileName, "r");
  float rate = 0;
  float duration = 0;
  unsigned int size = 0;
  rates = (double*) malloc(sizeof(double)*10);
  durations = (double*) malloc(sizeof(double)*10);
  size = 10;
  intervals = 0;
  if(f){
    while(fgets(line, 512, f) != NULL){
      sscanf(line, "%f %f", &rate, &duration);
      rates[intervals] = rate;
      durations[intervals] = duration;
      ++intervals;

      if(intervals == size){
        size += 10;
        rates = (double*) realloc(rates, sizeof(double)*size);
        durations = (double*) realloc(durations, sizeof(double)*size);
      }
    }
    fclose(f);
  }
}

void body_cb(dpi_http_message_informations_t* http_informations,
             const u_char* app_data, u_int32_t data_length,
             dpi_pkt_infos_t* pkt,
             void** flow_specific_user_data, void* user_data,
             u_int8_t last){
  if(*flow_specific_user_data==NULL){
    if(scanner_pool->mc_pop(flow_specific_user_data)==false){
            *flow_specific_user_data=
	      new byte_scanner(*((trie*) user_data), match_found);
    }
  }
  byte_scanner* scanner=(byte_scanner*) (*flow_specific_user_data);
  for(u_int32_t i=0; i<data_length; i++){
    scanner->match(app_data[i]);
  }
}
 
void flow_cleaner(void* flow_specific_user_data){
  if(scanner_pool->mp_push(flow_specific_user_data)==false){
    delete (byte_scanner*) flow_specific_user_data;
  }
}
 

static double idle_watts_socket=0;
static double idle_watts_cores=0;
static double idle_watts_offcores=0;
static double idle_watts_dram=0;

FILE* outstats = NULL;

void print_stats_callback(u_int16_t num_workers, unsigned long workers_frequency, mc_dpi_joules_counters joules, double current_system_load){
  static u_int64_t last_pkts = 0;
  double interval = 0;

  u_int64_t proc_pkts = processed_packets;
  double watts_sockets, watts_cores, watts_offcores, watts_dram;

  static ticks lastticks = getticks();
  ticks currentticks;
  float pktloss=0;

  currentticks = getticks();
  interval = (double) (currentticks - lastticks) / (double) CLOCK_FREQ;

  current_real_rate = ((double)(proc_pkts - last_pkts))/interval;

  last_pkts = proc_pkts;

  watts_sockets = ((joules.joules_socket[0]+joules.joules_socket[1])/(double)interval)-idle_watts_socket;
  watts_cores = ((joules.joules_cores[0]+joules.joules_cores[1])/(double)interval)-idle_watts_cores;
  watts_offcores = ((joules.joules_offcores[0]+joules.joules_offcores[1])/(double)interval)-idle_watts_offcores;
  if(watts_offcores == 0){
  	watts_offcores = watts_sockets - watts_cores;
  }
  watts_dram = ((joules.joules_dram[0]+joules.joules_dram[1])/(double)stats_collection_interval)-idle_watts_dram;
 
  fprintf(outstats, "%d\t%d,%lu\t%f\t%f\t%f\t%f\t%f\t%f\t%f\t%f\n",
	  last_sec-start_time,
	  num_workers,
	  workers_frequency,
	  rates[current_interval],
	  current_real_rate,
	  watts_sockets,
	  watts_cores,
	  watts_offcores,
	  watts_dram,
          current_real_rate / watts_cores,
          current_system_load);
  lastticks = currentticks;

}
 
int main(int argc, char **argv){
  pthread_t clock;
  unsigned int polling_interval;
  using namespace std;
  ff_mapThreadToCpu(0, -20);
  terminating=0;
  mc_dpi_reconfiguration_freq_strategy strategy = MC_DPI_RECONF_STRAT_CORES_CONSERVATIVE;

  try {
    if (argc<4){
            cerr << "Usage: " << argv[0] <<
	            " virus-signatures-file input-file polling-interval [strategy]\n";
            exit(EXIT_FAILURE);
    }


    string::size_type trie_depth=DEFAULT_TRIE_MAXIMUM_DEPTH;
    
    char const *virus_signatures_file_name=argv[1];
    char const *input_file_name=argv[2];
    polling_interval=atoi(argv[3]);
    if(argc>=5){
       strategy=(mc_dpi_reconfiguration_freq_strategy)atoi(argv[4]);
    }

    outstats = fopen("stats.txt", "w");
    fprintf(outstats, "Secs\tNumW,Freq\tExpectedRate\tCurrRate\tWattsSocket\tWattsCores\tWattsOffCores\tWattsDRAM\tEfficiency\tUtilization\n");
     
    ifstream signatures;
    signatures.open(virus_signatures_file_name);
    if(!signatures){
            cerr << argv[0] << ": failed to open '" <<
	      virus_signatures_file_name << "'\n";
            exit(EXIT_FAILURE);
    }
 
    signature_reader reader(signatures);
     
    cout << "reading '" << virus_signatures_file_name << "'... ";
     
    timer read_signatures_timer;
    read_signatures_timer.start();
    trie t(trie_depth);
    while (reader.next()) {
      t.insert(make_pair(reader.current_pattern(),
			 reader.current_name()));
    }
    read_signatures_timer.stop();
    cout << setiosflags(ios_base::fixed) << setprecision(3) <<
      read_signatures_timer.real_time() << " seconds.\n";
    cout << "preparing '" << virus_signatures_file_name << "'... ";
    timer prepare_signatures_timer;
    prepare_signatures_timer.start();
    t.prepare();
    prepare_signatures_timer.stop();
    cout << setiosflags(ios_base::fixed)
	 << setprecision(3)
	 << prepare_signatures_timer.real_time() << " seconds.\n";
 
    cout << "# of allocated trie nodes: " << t.node_count() << endl;
 
    timer full_timer;

    /******************************************************/
    /*             Start scanning the files.              */
    /******************************************************/
    pcap_t *handle;
    char errbuf[PCAP_ERRBUF_SIZE];
 
    mc_dpi_parallelism_details_t details;
    bzero(&details, sizeof(mc_dpi_parallelism_details_t));
    load_rates("rates.txt"); 
    if(argc>=6){
      details.available_processors = atoi(argv[5]);
    }else{
      details.available_processors = 16;
    }

    mc_dpi_library_state_t* state=mc_dpi_init_stateful(
						       32767, 32767, 1000000, 1000000, details);

    mc_dpi_joules_counters joules_before, joules_after, joules_diff;
    double interval;
    unsigned int i=0;

#if 1
    joules_before = mc_dpi_joules_counters_read(state);
    interval = 10;
    printf("Computing watts before running farm (over a %f secs interval)\n", interval);
    sleep(interval);
    joules_after = mc_dpi_joules_counters_read(state);
    joules_diff = mc_dpi_joules_counters_diff(state, joules_after, joules_before);

    for(i=0; i<joules_before.num_sockets; i++){
      idle_watts_socket+=joules_diff.joules_socket[i]/interval;
      idle_watts_cores+=joules_diff.joules_cores[i]/interval;
      idle_watts_offcores+=joules_diff.joules_offcores[i]/interval;
      idle_watts_dram+=joules_diff.joules_dram[i]/interval;
      printf("Idle watts %d: Socket: %f Cores: %f Offcores: %f DRAM: %f\n", i, joules_diff.joules_socket[i]/interval, joules_diff.joules_cores[i]/interval,joules_diff.joules_offcores[i]/interval, joules_diff.joules_dram[i]/interval);
    }

    printf("Wrapping interval: %d seconds\n", mc_dpi_joules_counters_wrapping_interval(state));
#endif
 
    printf("Open offline.\n");
    handle=pcap_open_offline(input_file_name, errbuf);
 
    if(handle==NULL){
      fprintf(stderr, "Couldn't open device %s: %s\n",
	      input_file_name, errbuf);
      exit(EXIT_FAILURE);
    }
    const u_char* packet;
    struct pcap_pkthdr header;
    unsigned char** packets;
    u_int32_t* sizes;
    u_int32_t num_packets=0;
    u_int32_t current_capacity=0;
    packets=(unsigned char**)
      malloc(sizeof(unsigned char*)*CAPACITY_CHUNK);
    sizes=(u_int32_t*)
      malloc((sizeof(u_int32_t))*CAPACITY_CHUNK);
    assert(packets);
    assert(sizes);
    current_capacity+=CAPACITY_CHUNK;
    while((packet=pcap_next(handle, &header))!=NULL){
      if((((struct ether_header*) packet)->ether_type)!=
	 htons(ETHERTYPE_IP) &&
	 (((struct ether_header*) packet)->ether_type!=
	  htons(ETHERTYPE_IPV6))){
	continue;
      }
 
      if(num_packets==current_capacity){
	packets=(unsigned char**)
	  realloc(packets, sizeof(unsigned char*)*
		  (current_capacity+CAPACITY_CHUNK));
	sizes=(u_int32_t*)
	  realloc(sizes, sizeof(u_int32_t)*
		  (current_capacity+CAPACITY_CHUNK));
	current_capacity+=CAPACITY_CHUNK;
	assert(packets);
	assert(sizes);
      }
 
      assert(header.len>sizeof(struct ether_header));
 
      posix_memalign((void**) &(packets[num_packets]),
		     DPI_CACHE_LINE_SIZE,
		     sizeof(unsigned char)*
		     (header.len-sizeof(struct ether_header)));
      assert(packets[num_packets]);
      memcpy(packets[num_packets],
	     packet+sizeof(struct ether_header),
	     (header.len-sizeof(struct ether_header)));
            sizes[num_packets]=
	      (header.len-sizeof(struct ether_header));
            ++num_packets;
    }
    std::cout << "Read " << num_packets << " packets." <<
      std::endl;
    pcap_close(handle);
    pcap_packets_memory_t x;
    x.packets=packets;
    x.sizes=sizes;
    x.next_packet_to_sent=0;
    x.num_packets=num_packets;
 
 
    scanner_pool=new ff::uSWSR_Ptr_Buffer(SCANNER_POOL_SIZE);
    scanner_pool->init();
    for(uint i=0; i<SCANNER_POOL_SIZE; i++){
      scanner_pool->push(new byte_scanner(t, match_found));
    }
    mc_dpi_set_read_and_process_callbacks(
					  state, &reading_cb, &processing_cb, (void*) &x);
    mc_dpi_set_flow_cleaner_callback(state, &flow_cleaner);
    dpi_http_callbacks_t callback={0, 0, 0, 0, 0, &body_cb};
    mc_dpi_http_activate_callbacks(state, &callback, (void*)(&t));
    
    mc_dpi_set_num_workers(state, details.available_processors);

    mc_dpi_reconfiguration_parameters reconf_params;
    reconf_params.sampling_interval = polling_interval;
    reconf_params.num_samples = 6; //4;
    reconf_params.system_load_up_threshold = 90;
    //reconf_params.worker_load_up_threshold = 90;
    reconf_params.system_load_down_threshold = 80;
    //reconf_params.worker_load_down_threshold = 80;
    reconf_params.freq_type = MC_DPI_RECONF_FREQ_GLOBAL;
    reconf_params.freq_strategy = strategy;
    reconf_params.stabilization_period = 4;
    //    reconf_params.migrate_collector = 1;
  
    mc_dpi_reconfiguration_set_parameters(state, reconf_params);

    full_timer.start();

    start_time = time(NULL);
    pthread_create(&clock, NULL, clock_thread, NULL);

    //    mc_dpi_run(state);
    stats_collection_interval = polling_interval;
    mc_dpi_set_stats_collection_callback(state,
					 stats_collection_interval,
					 print_stats_callback);

    mc_dpi_run(state); 
    mc_dpi_wait_end(state);
    mc_dpi_print_stats(state);
    full_timer.stop();
 
    byte_scanner* bs;
    while(!scanner_pool->empty()){
      scanner_pool->pop((void**) &bs);
      delete bs;
    }
    /* And close the session */
    mc_dpi_terminate(state);
    delete scanner_pool;
 
    for(i=0; i<num_packets; i++){
      free(packets[i]);
    }
    free(packets);
    free(sizes);
    free(rates);
    free(durations);
    fclose(outstats);
    exit(EXIT_SUCCESS);
  }catch(exception const &ex){
    cout.flush();
    cerr << typeid(ex).name() << " exception: " << ex.what() << "\n";
    throw;
  }
}
