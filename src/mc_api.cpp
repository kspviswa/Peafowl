/*
 * mc_api.cpp
 *
 * Created on: 12/11/2012
 * =========================================================================
 *  Copyright (C) 2012-2013, Daniele De Sensi (d.desensi.software@gmail.com)
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
 * ====================================================================
 */


#include "mc_api.h"
#include "flow_table.h"
#include "energy_utils.h"
#include "worker.hpp"
#include <float.h>
#include <stddef.h>
#include <vector>
#include <cmath>

#include <ff/farm.hpp>
#include <ff/pipeline.hpp>
#include <ff/buffer.hpp>

#define DPI_DEBUG_MC_API 0
#define debug_print(fmt, ...)          \
            do { if (DPI_DEBUG_MC_API) \
            fprintf(stdout, fmt, __VA_ARGS__); } while (0)



#define DPI_MULTICORE_STATUS_UPDATER_TID 1


typedef struct mc_dpi_library_state{
	dpi_library_state_t* sequential_state;
	ff::SWSR_Ptr_Buffer* tasks_pool;

	u_int8_t parallel_module_type;
	/******************************************************/
	/*                     Callbacks                      */
	/******************************************************/
	mc_dpi_packet_reading_callback* reading_callback;
	mc_dpi_processing_result_callback* processing_callback;
	void* read_process_callbacks_user_data;

	u_int8_t freeze_flag;
	u_int8_t terminating;
	u_int8_t is_running;

	/**
	 * This lock keeps the state locked between a freeze and
	 * the successive run. **/
	ff::CLHSpinLock state_update_lock;
	u_int16_t available_processors;
	unsigned int* mapping;
	/******************************************************/
	/*                 Nodes for single farm.             */
	/******************************************************/
	ff::ff_farm<dpi::dpi_L7_scheduler>* single_farm;
	std::vector<ff::ff_node*>* single_farm_workers;
	dpi::dpi_collapsed_emitter* single_farm_emitter;
	dpi::dpi_L7_collector* single_farm_collector;
  	u_int16_t collector_proc_id;

	u_int16_t single_farm_active_workers;

	/******************************************************/
	/*                 Nodes for double farm.             */
	/******************************************************/
	dpi::dpi_L3_L4_emitter* L3_L4_emitter;
#if DPI_MULTICORE_L3_L4_FARM_TYPE == \
	DPI_MULTICORE_L3_L4_ORDERED_FARM
	ff::ff_ofarm* L3_L4_farm;
#else
	ff::ff_farm<>* L3_L4_farm;
#endif
	std::vector<ff::ff_node*>* L3_L4_workers;
	dpi::dpi_L3_L4_collector* L3_L4_collector;

	dpi::dpi_L7_emitter* L7_emitter;
	ff::ff_farm<dpi::dpi_L7_scheduler>* L7_farm;
	std::vector<ff::ff_node*>* L7_workers;
	dpi::dpi_L7_collector* L7_collector;
	ff::ff_pipeline* pipeline;
	u_int16_t double_farm_L3_L4_active_workers;
	u_int16_t double_farm_L7_active_workers;
	/******************************************************/
	/*                 Statistics.                        */
	/******************************************************/
	struct timeval start_time;
	struct timeval stop_time;
	energy_counters_state* energy_counters;
	mc_dpi_reconfiguration_parameters reconf_params;
	double** load_samples;
	unsigned int current_load_sample;
	unsigned int current_num_samples;
	u_int32_t collection_interval;
	mc_dpi_stats_collection_callback* stats_callback;
	unsigned long* available_frequencies;
	unsigned int num_available_frequencies;
	unsigned int current_frequency_id;
	unsigned int* one_core_per_socket;
	unsigned int num_sockets;
	double current_system_load;
	double current_instantaneous_system_load;
	double last_prediction;
}mc_dpi_library_state_t;


#ifndef DPI_DEBUG
static inline
#endif
void mc_dpi_create_double_farm(mc_dpi_library_state_t* state,
		                       u_int32_t size_v4,
		                       u_int32_t size_v6){
	u_int16_t last_mapped=0;
	/******************************************/
	/*         Create the first farm.         */
	/******************************************/
	void* tmp;
#if DPI_MULTICORE_L3_L4_FARM_TYPE == \
	DPI_MULTICORE_L3_L4_ORDERED_FARM
	tmp=malloc(sizeof(ff::ff_ofarm));
	assert(tmp);
	state->L3_L4_farm=new (tmp) ff::ff_ofarm(
			false,
			DPI_MULTICORE_L3_L4_FARM_INPUT_BUFFER_SIZE,
			DPI_MULTICORE_L3_L4_FARM_OUTPUT_BUFFER_SIZE,
			false, state->available_processors, true);
	tmp=malloc(sizeof(dpi::dpi_L3_L4_emitter));
	assert(tmp);
	state->L3_L4_emitter=new (tmp) dpi::dpi_L3_L4_emitter(
			&(state->reading_callback),
			&(state->read_process_callbacks_user_data),
			&(state->freeze_flag), &(state->terminating),
			state->mapping[last_mapped], state->tasks_pool);
	last_mapped=(last_mapped+1)%state->available_processors;
	state->L3_L4_farm->setEmitterF(state->L3_L4_emitter);
#else
	tmp=malloc(sizeof(ff::ff_farm<>));
	assert(tmp);
	state->L3_L4_farm=new (tmp) ff::ff_farm<>(
			false,
			DPI_MULTICORE_L3_L4_FARM_INPUT_BUFFER_SIZE,
			DPI_MULTICORE_L3_L4_FARM_OUTPUT_BUFFER_SIZE,
			false, state->available_processors, true);
	tmp=malloc(sizeof(dpi::dpi_L3_L4_emitter));
	assert(tmp);
	state->L3_L4_emitter=new (tmp) dpi::dpi_L3_L4_emitter(
			&(state->reading_callback),
			&(state->read_process_callbacks_user_data),
			&(state->freeze_flag), &(state->terminating),
			state->mapping[last_mapped], state->tasks_pool);
	last_mapped=(last_mapped+1)%state->available_processors;
	state->L3_L4_farm->add_emitter(state->L3_L4_emitter);
#if DPI_MULTICORE_L3_L4_FARM_TYPE == \
	DPI_MULTICORE_L3_L4_ON_DEMAND
	state->L3_L4_farm->set_scheduling_ondemand(1024);
#endif
#endif

	state->L3_L4_workers=new std::vector<ff::ff_node*>;
	dpi::dpi_L3_L4_worker* w1;
	for(uint i=0; i<state->double_farm_L3_L4_active_workers; i++){
		tmp=malloc(sizeof(dpi::dpi_L3_L4_worker));
		assert(tmp);
		w1=new (tmp) dpi::dpi_L3_L4_worker(state->sequential_state, i,
		   &(state->single_farm_active_workers),
		   state->mapping[last_mapped],
		   size_v4,
		   size_v6);
		state->L3_L4_workers->push_back(w1);
		last_mapped=(last_mapped+1)%state->available_processors;
	}
	assert(state->L3_L4_farm->add_workers(*(state->L3_L4_workers))==0);

	tmp=malloc(sizeof(dpi::dpi_L3_L4_collector));
	assert(tmp);
	state->L3_L4_collector=new (tmp)
			               dpi::dpi_L3_L4_collector(state->mapping[last_mapped]);
	assert(state->L3_L4_collector);
	last_mapped=(last_mapped+1)%state->available_processors;
#if DPI_MULTICORE_L3_L4_FARM_TYPE == \
	DPI_MULTICORE_L3_L4_ORDERED_FARM
	state->L3_L4_farm->setCollectorF(state->L3_L4_collector);
#else
	assert(state->L3_L4_farm->add_collector(state->L3_L4_collector)>=0);
#endif

	/**************************************/
	/*      Create the second farm.       */
	/**************************************/
	tmp=malloc(sizeof(ff::ff_farm<dpi::dpi_L7_scheduler>));
	assert(tmp);
	state->L7_farm=new (tmp) ff::ff_farm<dpi::dpi_L7_scheduler>(
			false, DPI_MULTICORE_L7_FARM_INPUT_BUFFER_SIZE,
			DPI_MULTICORE_L7_FARM_OUTPUT_BUFFER_SIZE, false,
			state->available_processors, true);

	tmp=malloc(sizeof(dpi::dpi_L7_emitter));
	assert(tmp);
	state->L7_emitter=new (tmp) dpi::dpi_L7_emitter(
			state->L7_farm->getlb(),
			state->double_farm_L7_active_workers,
			state->mapping[last_mapped]);
	last_mapped=(last_mapped+1)%state->available_processors;
	state->L7_farm->add_emitter(state->L7_emitter);

	state->L7_workers=new std::vector<ff::ff_node*>;
	dpi::dpi_L7_worker* w2;
	for(uint i=0; i<state->double_farm_L7_active_workers; i++){
		tmp=malloc(sizeof(dpi::dpi_L7_worker));
		assert(tmp);
		w2=new (tmp) dpi::dpi_L7_worker(state->sequential_state, i,
				                        state->mapping[last_mapped]);
		state->L7_workers->push_back(w2);
		last_mapped=(last_mapped+1)%state->available_processors;
	}
	assert(state->L7_farm->add_workers(*(state->L7_workers))==0);

	tmp=malloc(sizeof(dpi::dpi_L7_collector));
	assert(tmp);
        state->collector_proc_id=state->mapping[last_mapped];

	state->L7_collector=new (tmp) dpi::dpi_L7_collector(
			&(state->processing_callback),
			&(state->read_process_callbacks_user_data),
			&(state->collector_proc_id), state->tasks_pool);
	last_mapped=(last_mapped+1)%state->available_processors;
	assert(state->L7_farm->add_collector(state->L7_collector)>=0);

	/********************************/
	/*     Create the pipeline.     */
	/********************************/
	tmp=malloc(sizeof(ff::ff_pipeline));
	assert(tmp);
	state->pipeline=new (tmp) ff::ff_pipeline(
			false,
			DPI_MULTICORE_PIPELINE_INPUT_BUFFER_SIZE,
			DPI_MULTICORE_PIPELINE_OUTPUT_BUFFER_SIZE,
			true);

	state->pipeline->add_stage(state->L3_L4_farm);
	state->pipeline->add_stage(state->L7_farm);
	state->parallel_module_type=MC_DPI_PARALLELISM_FORM_DOUBLE_FARM;
}

#ifndef DPI_DEBUG
static inline
#endif
void mc_dpi_create_single_farm(mc_dpi_library_state_t* state,
		                       u_int32_t size_v4, u_int32_t size_v6){
	u_int16_t last_mapped=0;
	state->single_farm=new ff::ff_farm<dpi::dpi_L7_scheduler>(
			false,
			DPI_MULTICORE_L7_FARM_INPUT_BUFFER_SIZE,
			DPI_MULTICORE_L7_FARM_OUTPUT_BUFFER_SIZE,
			false, state->available_processors, true);
	assert(state->single_farm);

	state->single_farm_emitter=new dpi::dpi_collapsed_emitter(
			&(state->reading_callback),
			&(state->read_process_callbacks_user_data),
			&(state->freeze_flag), &(state->terminating),
			state->tasks_pool, state->sequential_state,
			&(state->single_farm_active_workers),
			size_v4,
			size_v6,
			state->single_farm->getlb(),
			state->mapping[last_mapped]);
	assert(state->single_farm_emitter);
	last_mapped=(last_mapped+1)%state->available_processors;
	state->single_farm->add_emitter(state->single_farm_emitter);

	state->single_farm_workers=new std::vector<ff::ff_node*>;
	dpi::dpi_L7_worker* w;
	for(u_int16_t i=0; i<state->single_farm_active_workers; i++){
		w=new dpi::dpi_L7_worker(state->sequential_state, i,
				                 state->mapping[last_mapped]);
		assert(w);
		state->single_farm_workers->push_back(w);
		last_mapped=(last_mapped+1)%state->available_processors;
	}

	assert(state->single_farm->add_workers(
				*(state->single_farm_workers))==0);
	state->collector_proc_id=state->mapping[last_mapped];
	state->single_farm_collector=new dpi::dpi_L7_collector(
			&(state->processing_callback),
			&(state->read_process_callbacks_user_data),
			&(state->collector_proc_id), state->tasks_pool);
	assert(state->single_farm_collector);
	last_mapped=(last_mapped+1)%state->available_processors;
	assert(state->single_farm->add_collector(
			state->single_farm_collector)>=0);
	state->parallel_module_type=MC_DPI_PARALLELISM_FORM_ONE_FARM;
}



/**
 * Initializes the library and sets the parallelism degree according to
 * the cost model obtained from the parameters that the user specifies.
 * If not specified otherwise after the initialization, the library will
 * consider all the protocols active.
 *
 * @param size_v4 Size of the array of pointers used to build the database
 *                for v4 flows.
 * @param size_v6 Size of the array of pointers used to build the database
 *                for v6 flows.
 * @param max_active_v4_flows The maximum number of IPv4 flows which can
 *                            be active at any time. After reaching this
 *                            threshold, new flows will not be created.
 * @param max_active_v6_flows The maximum number of IPv6 flows which can
 *                            be active at any time. After reaching this
 *                            threshold, new flows will not be created.
 * @param parallelism_details Details about the parallelism form. Must be
 *                            zeroed and then filled by the user.
 * @return A pointer to the state of the library.
 */
mc_dpi_library_state_t* mc_dpi_init_stateful(
		u_int32_t size_v4, u_int32_t size_v6,
		u_int32_t max_active_v4_flows,
		u_int32_t max_active_v6_flows,
		mc_dpi_parallelism_details_t parallelism_details){
	mc_dpi_library_state_t* state;
	assert(posix_memalign((void**) &state, DPI_CACHE_LINE_SIZE,
		   sizeof(mc_dpi_library_state_t)+DPI_CACHE_LINE_SIZE)==0);
	bzero(state, sizeof(mc_dpi_library_state_t));

	u_int8_t parallelism_form=parallelism_details.parallelism_form;
	if(parallelism_details.available_processors){
		state->available_processors=parallelism_details.available_processors;
	}else{
		energy_counters_get_num_real_cores((unsigned int*) &(state->available_processors));
	}

	if(parallelism_form==MC_DPI_PARALLELISM_FORM_DOUBLE_FARM){
		assert(state->available_processors>=
			4+2);

	}else{
		assert(state->available_processors>=
			2+1);
	}
	
	state->mapping = new unsigned int[state->available_processors];

	if(parallelism_details.mapping==NULL){
		energy_counters_get_real_cores_identifiers(state->mapping, state->available_processors);
	}else{
		uint k;
		for(k=0; k<state->available_processors; k++){
	  		state->mapping[k]=parallelism_details.mapping[k];
		}
	}

	state->terminating=0;

	u_int16_t hash_table_partitions;

	state->double_farm_L3_L4_active_workers=
			parallelism_details.double_farm_num_L3_workers;
	state->double_farm_L7_active_workers=
			parallelism_details.double_farm_num_L7_workers;
	state->single_farm_active_workers=
			state->available_processors-2;
	if(parallelism_form==MC_DPI_PARALLELISM_FORM_DOUBLE_FARM){
		assert(state->double_farm_L3_L4_active_workers>0 &&
			   state->double_farm_L7_active_workers>0);
		debug_print("%s\n","[mc_dpi_api.cpp]: A pipeline of two "
				"farms will be activated.");
		hash_table_partitions=state->double_farm_L7_active_workers;
	}else{
		assert(state->single_farm_active_workers>0);
		debug_print("%s\n","[mc_dpi_api.cpp]: Only one farm will "
				"be activated.");
		hash_table_partitions=state->single_farm_active_workers;
	}

	state->sequential_state=dpi_init_stateful_num_partitions(
			size_v4,
			size_v6,
			max_active_v4_flows,
			max_active_v6_flows,
			hash_table_partitions);

	/******************************/
	/*   Create the tasks pool.   */
	/******************************/
#if DPI_MULTICORE_USE_TASKS_POOL
	void* tmp;
	assert(posix_memalign((void**) &tmp, DPI_CACHE_LINE_SIZE,
		   sizeof(ff::SWSR_Ptr_Buffer)+DPI_CACHE_LINE_SIZE)==0);
	state->tasks_pool=new (tmp) ff::SWSR_Ptr_Buffer(
			DPI_MULTICORE_TASKS_POOL_SIZE);
	state->tasks_pool->init();
#endif

	if(parallelism_form==MC_DPI_PARALLELISM_FORM_DOUBLE_FARM){
		mc_dpi_create_double_farm(
				state,
				size_v4, size_v6);
	}else{
		mc_dpi_create_single_farm(
				state,
				size_v4, size_v6);
	}


	state->freeze_flag=1;

	debug_print("%s\n","[mc_dpi_api.cpp]: Preparing...");
	if(state->parallel_module_type==
			MC_DPI_PARALLELISM_FORM_DOUBLE_FARM){
		// Warm-up
		assert(state->pipeline->run_then_freeze()>=0);
		state->pipeline->wait_freezing();
	}else{
		// Warm-up
		assert(state->single_farm->run_then_freeze()>=0);
		state->single_farm->wait_freezing();
	}	
	debug_print("%s\n","[mc_dpi_api.cpp]: Freezed...");

	ff::init_unlocked(&(state->state_update_lock));
	ff::spin_lock(&(state->state_update_lock),
				DPI_MULTICORE_STATUS_UPDATER_TID);
	state->is_running=0;
	state->stop_time.tv_sec=0;
	state->stop_time.tv_usec=0;
	state->energy_counters=(energy_counters_state*)malloc(sizeof(energy_counters_state));
	if(energy_counters_init(state->energy_counters)!=0){
		free(state->energy_counters);
		state->energy_counters=NULL;
	}

	state->load_samples=NULL;
	state->current_load_sample=0;
	state->current_num_samples=0;
	state->stats_callback=0;
	state->collection_interval=0;
	energy_counters_get_available_frequencies(&(state->available_frequencies), &(state->num_available_frequencies));
	state->current_frequency_id=0;
	state->one_core_per_socket=NULL;

	debug_print("%s\n","[mc_dpi_api.cpp]: Preparation finished.");
	return state;
}

/**
 * Prints execution's statistics.
 * @param state A pointer to the state of the library.
 */
void mc_dpi_print_stats(mc_dpi_library_state_t* state){
	if(state){
		if(state->parallel_module_type==
				MC_DPI_PARALLELISM_FORM_DOUBLE_FARM){
			state->pipeline->ffStats(std::cout);
		}else{
			state->single_farm->ffStats(std::cout);
		}
		if(state->stop_time.tv_sec != 0){
			std::cout << "Completion time: " << ff::diffmsec(state->stop_time, state->start_time) << std::endl;
		}
	}
}


/**
 * Terminates the library.
 * @param state A pointer to the state of the library.
 */
void mc_dpi_terminate(mc_dpi_library_state_t *state){
	if(likely(state)){
		if(state->parallel_module_type==
				MC_DPI_PARALLELISM_FORM_DOUBLE_FARM){
			state->L3_L4_emitter->~dpi_L3_L4_emitter();
			free(state->L3_L4_emitter);
#if DPI_MULTICORE_L3_L4_FARM_TYPE ==\
	DPI_MULTICORE_L3_L4_ORDERED_FARM
			state->L3_L4_farm->~ff_ofarm();
#else
			state->L3_L4_farm->~ff_farm();
#endif
			free(state->L3_L4_farm);
			free(state->L3_L4_collector);

			while(!state->L3_L4_workers->empty()){
				((dpi::dpi_L3_L4_worker*) state->
						L3_L4_workers->back())->~dpi_L3_L4_worker();
				free((dpi::dpi_L3_L4_worker*) state->
						L3_L4_workers->back());
				state->L3_L4_workers->pop_back();
			}
			delete state->L3_L4_workers;

			state->L7_emitter->~dpi_L7_emitter();
			free(state->L7_emitter);
			state->L7_farm->~ff_farm();
			free(state->L7_farm);
			free(state->L7_collector);

			while(!state->L7_workers->empty()){
				((dpi::dpi_L7_worker*) state->
						L7_workers->back())->~dpi_L7_worker();
				free((dpi::dpi_L7_worker*) state->L7_workers->back());
				state->L7_workers->pop_back();
			}
			delete state->L7_workers;

			state->pipeline->~ff_pipeline();
			free(state->pipeline);
		}else{
			delete state->single_farm_emitter;
			delete state->single_farm;
			delete state->single_farm_collector;
			while(!state->single_farm_workers->empty()){
				delete (dpi::dpi_L7_worker*) state->
						single_farm_workers->back();
				state->single_farm_workers->pop_back();
			}
			delete state->single_farm_workers;
		}
		dpi_terminate(state->sequential_state);

#if DPI_MULTICORE_USE_TASKS_POOL
		state->tasks_pool->~SWSR_Ptr_Buffer();
		free(state->tasks_pool);
#endif
		if(state->energy_counters){
			energy_counters_terminate(state->energy_counters);
		}

		if(state->load_samples){
			u_int16_t i;
			for(i=0; i<state->available_processors - 2; i++){
				if(state->load_samples[i]){
					free(state->load_samples[i]);
				}
			}
			free(state->load_samples);
		}
		if(state->one_core_per_socket){
			free(state->one_core_per_socket);
		}

		delete[] state->mapping;
		free(state);
	}

}

/**
 * Sets the reading and processing callbacks. It can be done only after
 * that the state has been initialized and before calling run().
 *
 * @param state                 A pointer to the state of the library.
 * @param reading_callback      A pointer to the reading callback. It must
 *                              be different from NULL.
 * @param processing_callback   A pointer to the processing callback. It
 *                              must be different from NULL.
 * @param user_data             A pointer to the user data to be passed to
 *                              the callbacks.
 */
void mc_dpi_set_read_and_process_callbacks(
		mc_dpi_library_state_t* state,
		mc_dpi_packet_reading_callback* reading_callback,
		mc_dpi_processing_result_callback* processing_callback,
		void* user_data){
	state->reading_callback=reading_callback;
	state->processing_callback=processing_callback;
	state->read_process_callbacks_user_data=user_data;
}


/***************************************/
/*          Other API calls            */
/***************************************/

/**
 * Starts the library.
 * @param state A pointer to the state of the library.
 */
void mc_dpi_run(mc_dpi_library_state_t* state){
	// Real start
	debug_print("%s\n","[mc_dpi_api.cpp]: Run preparation...");
	state->is_running=1;
	mc_dpi_unfreeze(state);
	gettimeofday(&state->start_time,NULL);
	debug_print("%s\n","[mc_dpi_api.cpp]: Running...");
}

/**
 * Reads the joules counters.
 * ATTENTION: The counters may wrap. Use mc_dpi_joules_counters_wrapping_interval
 *            to get the maximum amount of second you can wait between two successive
 *            readings.
 * @param state A pointer to the state of the library.
 * @return The values of the counters at the current time.
 *         ATTENTION: The result is not meaningful for the user but only for the
 *                    framework. It MUST only be used as a parameter for the 
 *                    mc_dpi_joules_counters_diff. Only the values returned by 
 *                    mc_dpi_joules_counters_diff call are meaningful for the user.
 */
mc_dpi_joules_counters mc_dpi_joules_counters_read(mc_dpi_library_state_t* state){
	mc_dpi_joules_counters r;
	memset(&r, 0, sizeof(mc_dpi_joules_counters));

	if(state && state->energy_counters){
		energy_counters_read(state->energy_counters);
		unsigned int i;
		assert(state->energy_counters->num_sockets < DPI_MAX_CPU_SOCKETS);
		r.num_sockets=state->energy_counters->num_sockets;
		for(i=0; i<state->energy_counters->num_sockets; i++){
			r.joules_socket[i]=state->energy_counters->sockets[i].energy_units_socket;
			r.joules_cores[i]=state->energy_counters->sockets[i].energy_units_cores;
			r.joules_offcores[i]=state->energy_counters->sockets[i].energy_units_offcores;
			r.joules_dram[i]=state->energy_counters->sockets[i].energy_units_dram;
		}
	}

	return r;
}

/**
 * Returns the maximum number of seconds that the user can wait before
 * performing a new counters read.
 * @param state A pointer to the state of the library.
 * @return The maximum number of seconds that the user can wait before
 *         performing a new counters read.
 */
u_int32_t mc_dpi_joules_counters_wrapping_interval(mc_dpi_library_state_t* state){
	return energy_counters_wrapping_time(state->energy_counters);
}

/**
 * Sets the statistics collection callback.
 * @param state A pointer to the state of the library.
 * @param collection_interval stats_callback will be called every
 *                            collection_interval seconds.
 * @param stats_callback The statistics collection callback.
 * @return 0 if the callback have been successfully set, 1 otherwise.
 */
u_int8_t mc_dpi_set_stats_collection_callback(mc_dpi_library_state_t* state,
                                              u_int32_t collection_interval,
                                              mc_dpi_stats_collection_callback* stats_callback){
	if(!state || collection_interval >= mc_dpi_joules_counters_wrapping_interval(state)){
		return 1;
	}else{
		state->collection_interval = collection_interval;
		state->stats_callback = stats_callback;
		return 0;
	}
}



#define DELTA_WRAP_JOULES(new, old, diff)			                                   \
	if (new > old) {					                                   \
		diff = (u_int32_t)new - (u_int32_t)old;		                                   \
	} else {                                                                                   \
		diff = (((u_int32_t)0xffffffff) - (u_int32_t)old) + (u_int32_t)1 + (u_int32_t)new; \
	}

/**
 * Returns the joules consumed between two calls to mc_dpi_joules_counters_read.
 * @param state A pointer to the state of the library.
 * @param after A joules counter.
 * @param before A joules counter.
 * @return The difference after-before (in joules).
 */
mc_dpi_joules_counters mc_dpi_joules_counters_diff(mc_dpi_library_state_t* state,
                                                   mc_dpi_joules_counters after,
                                                   mc_dpi_joules_counters before){
	mc_dpi_joules_counters result;
	memset(&result, 0, sizeof(result));
	unsigned int i;
	result.num_sockets=after.num_sockets;
	for(i=0; i<after.num_sockets; i++){
		DELTA_WRAP_JOULES(after.joules_socket[i], before.joules_socket[i], result.joules_socket[i]);
		result.joules_socket[i]=result.joules_socket[i]*state->energy_counters->sockets[i].energy_per_unit;

		DELTA_WRAP_JOULES(after.joules_cores[i], before.joules_cores[i], result.joules_cores[i]);
		result.joules_cores[i]=result.joules_cores[i]*state->energy_counters->sockets[i].energy_per_unit;

		DELTA_WRAP_JOULES(after.joules_offcores[i], before.joules_offcores[i], result.joules_offcores[i]);
		result.joules_offcores[i]=result.joules_offcores[i]*state->energy_counters->sockets[i].energy_per_unit;

		DELTA_WRAP_JOULES(after.joules_dram[i], before.joules_dram[i], result.joules_dram[i]);
		result.joules_dram[i]=result.joules_dram[i]*state->energy_counters->sockets[i].energy_per_unit;
	}
	return result;
}

/**
 * Computes the instantaneous load of each worker of the farm. Works only if
 * MC_DPI_PARALLELISM_FORM_ONE_FARM is used as parallelism form.
 * @param state A pointer to the state of the library.
 * @param loads An array that will be filled by the call with the
 *              load of each worker (in the range [0, 100]).
 * @return 0 if the loads have been successfully computed, 1 otherwise.
 */
u_int8_t mc_dpi_reconfiguration_get_workers_instantaneous_load(mc_dpi_library_state_t* state, double* loads){
	if(state->parallel_module_type == MC_DPI_PARALLELISM_FORM_ONE_FARM){
		unsigned int i;
	  	for(i=0; i<state->single_farm_active_workers; i++){
			dpi::dpi_L7_worker* current_worker = (dpi::dpi_L7_worker*)state->single_farm_workers->at(i);
			state->load_samples[i][state->current_load_sample] =
			                    current_worker->get_worktime_percentage();
			current_worker->reset_worktime_percentage();
		}
		return 0;
	}else{
		return 1;
	}
}

/**
 * Sets the parameters for the automatic reconfiguration of the farm.
 * @param state A pointer to the state of the library.
 * @param reconf_params The reconfiguration parameters.
 * @return 0 if the parameters have been successfully set, 1 otherwise.
 */
u_int8_t mc_dpi_reconfiguration_set_parameters(mc_dpi_library_state_t* state,
                                               mc_dpi_reconfiguration_parameters reconf_params){
	if(state->parallel_module_type == MC_DPI_PARALLELISM_FORM_ONE_FARM){
		state->reconf_params=reconf_params;
		u_int16_t i;
		if(!state->load_samples){
			state->load_samples=(double**)malloc(sizeof(double*)*(state->available_processors - 2));
			memset(state->load_samples, 0, sizeof(double*)*(state->available_processors - 2));
		}

		for(i=0; i<state->available_processors - 2; i++){
			if(state->load_samples[i]){
				free(state->load_samples[i]);
			}
			state->load_samples[i]=(double*)malloc(sizeof(double)*reconf_params.num_samples);
			memset(state->load_samples[i], 0, sizeof(double)*reconf_params.num_samples);
		}
		state->current_load_sample=0;

		unsigned long starting_frequency;

		if(state->reconf_params.freq_strategy == MC_DPI_RECONF_STRAT_CORES_CONSERVATIVE){
			starting_frequency = state->available_frequencies[state->num_available_frequencies - 1];
			state->current_frequency_id = state->num_available_frequencies - 1;
		}else if(state->reconf_params.freq_strategy == MC_DPI_RECONF_STRAT_POWER_CONSERVATIVE){
			starting_frequency = state->available_frequencies[0];
			state->current_frequency_id = 0;
		}

		if(state->reconf_params.freq_type != MC_DPI_RECONF_FREQ_NO){
			for(i=0; i<state->available_processors; i++){
				if(state->reconf_params.freq_strategy == MC_DPI_RECONF_STRAT_GOVERNOR_ON_DEMAND){
					energy_counters_set_ondemand_governor(state->mapping[i]);
				}else if(state->reconf_params.freq_strategy == MC_DPI_RECONF_STRAT_GOVERNOR_CONSERVATIVE){
					energy_counters_set_conservative_governor(state->mapping[i]);
					state->current_frequency_id = 0;
				}else if(state->reconf_params.freq_strategy == MC_DPI_RECONF_STRAT_GOVERNOR_PERFORMANCE){
					energy_counters_set_performance_governor(state->mapping[i]);
					state->current_frequency_id = state->num_available_frequencies - 1;
				}else{
					energy_counters_set_userspace_governor(state->mapping[i]);
				}
				energy_counters_set_bounds(state->available_frequencies[0], 
				                           state->available_frequencies[state->num_available_frequencies - 1], 
				                           state->mapping[i]);
			}

			if(state->reconf_params.freq_strategy != MC_DPI_RECONF_STRAT_GOVERNOR_ON_DEMAND &&
			   state->reconf_params.freq_strategy != MC_DPI_RECONF_STRAT_GOVERNOR_CONSERVATIVE &&
			   state->reconf_params.freq_strategy != MC_DPI_RECONF_STRAT_GOVERNOR_PERFORMANCE){
				if(state->reconf_params.freq_type == MC_DPI_RECONF_FREQ_SINGLE){
					energy_counters_set_frequency(starting_frequency,
					                              state->mapping,
					                              state->available_processors,
					                              0);

					/** Emitter and collector always run to the highest frequency. **/
					energy_counters_set_frequency(state->available_frequencies[state->num_available_frequencies - 1], 
					                              state->mapping[0]);
					energy_counters_set_frequency(state->available_frequencies[state->num_available_frequencies - 1], 
					                              state->mapping[state->single_farm_active_workers + 1]);
				}else if(state->reconf_params.freq_type == MC_DPI_RECONF_FREQ_GLOBAL){
					energy_counters_get_core_identifier_per_socket(state->mapping,
					                                               state->available_processors,
					                                               &(state->one_core_per_socket),
					                                               &state->num_sockets);
					energy_counters_set_frequency(state->available_frequencies[state->current_frequency_id],
					                              state->one_core_per_socket,
					                              state->num_sockets,
					                              1);
				}
			}
		}
		return 0;
	}else{
		return 1;
	}
}

/**
 * Freezes the library.
 * @param state A pointer to the state of the library.
 */
void mc_dpi_freeze(mc_dpi_library_state_t* state){
	if(unlikely(!state->is_running || mc_dpi_is_frozen(state))){
		return;
	}else{
		debug_print("%s\n","[mc_dpi_api.cpp]: Acquiring freeze lock.");
		/**
		 * All state modifications pass from mc_dpi_freeze().
		 * To avoid that more state modifications start together,
		 * we can simply protect the mc_dpi_freeze() function.
		 * Accordingly, more state modifications can be started
		 * concurrently by different threads. 
		 * WARNING: State modifications are expensive, it would
		 * be better if only one thread freezes the library, 
		 * performs all the modifications and the unfreezes the 
		 * library (not possible at the moment since each state
		 * modification function calls freezes and then unfreezes
		 * the library.
		 **/
		ff::spin_lock(&(state->state_update_lock),
				DPI_MULTICORE_STATUS_UPDATER_TID);
		debug_print("%s\n","[mc_dpi_api.cpp]: Freeze lock acquired, "
				    "sending freeze message.");

		state->freeze_flag=1;

		debug_print("%s\n","[mc_dpi_api.cpp]: Freeze message sent, "
				    "wait for freezing.");
		if(state->parallel_module_type==
				MC_DPI_PARALLELISM_FORM_DOUBLE_FARM){
			state->pipeline->wait_freezing();
		}else{
			state->single_farm->wait_freezing();
		}

		assert(mc_dpi_is_frozen(state));

		debug_print("%s\n","[mc_dpi_api.cpp]: Skeleton freezed.");
	}
}

/**
 * Check if the library is frozen.
 * @param state A pointer to the state of the library.
 * @return 1 if the library is frozen, 0 otherwise.
 */
u_int8_t mc_dpi_is_frozen(mc_dpi_library_state_t* state){
	return (state->freeze_flag>0)?1:0;
}

/**
 * Unfreezes the library.
 * @param state A pointer to the state of the library.
 */
void mc_dpi_unfreeze(mc_dpi_library_state_t* state){
	if(unlikely(!state->is_running || !mc_dpi_is_frozen(state) || state->terminating)){
		debug_print("%s %d %s %d %s %d\n","[mc_dpi_api.cpp]: isRunning: ",
			    state->is_running, " isFrozen: ", mc_dpi_is_frozen(state),
			    " terminating: ", state->terminating);
		return;
	}else{
		state->freeze_flag=0;
		debug_print("%s\n","[mc_dpi_api.cpp]: Prepare to run.");
		if(state->parallel_module_type==MC_DPI_PARALLELISM_FORM_DOUBLE_FARM){
			assert(state->pipeline->run_then_freeze()>=0);
		}else{
			assert(state->single_farm->run_then_freeze(
				state->single_farm_active_workers)>=0);
		}
		debug_print("%s\n","[mc_dpi_api.cpp]: Running.");
		ff::spin_unlock(&(state->state_update_lock), 
				DPI_MULTICORE_STATUS_UPDATER_TID);
		debug_print("%s\n","[mc_dpi_api.cpp]: Unlocked.");
	}
}

/**
 * Gets the average load (average over the samples) of a worker.
 * @param state A pointer to the state of the library.
 * @param worker_id The identifier of the worker.
 * @return The average load of the worker in the interval [0, 100].
 */
static double mc_dpi_reconfiguration_get_worker_average_load(mc_dpi_library_state_t* state, u_int16_t worker_id){
	double avg = 0;
	unsigned int num_samples = std::min(state->current_num_samples, state->reconf_params.num_samples);
	if(state->load_samples == NULL || state->load_samples[worker_id] == NULL){
		return 0;
	}
	for(unsigned long i = 0; i<num_samples; i++){
		avg += state->load_samples[worker_id][i];
	}
	return (avg / (double)num_samples);
}

static void mc_dpi_reconfiguration_update_system_frequencies(mc_dpi_library_state_t* state, unsigned int frequency_id){
	if(state->reconf_params.freq_type == MC_DPI_RECONF_FREQ_NO || frequency_id == state->current_frequency_id){
		return;
	}
	
	state->current_frequency_id = frequency_id;
	
	if(state->reconf_params.freq_type == MC_DPI_RECONF_FREQ_SINGLE){
		energy_counters_set_frequency(state->available_frequencies[state->current_frequency_id],
		                              &(state->mapping[1]),
		                              state->single_farm_active_workers,
		                              0);
		if(state->reconf_params.migrate_collector){
			energy_counters_set_frequency(state->available_frequencies[state->num_available_frequencies - 1],
			                              state->mapping[state->single_farm_active_workers + 1]);
		}
	}else if(state->reconf_params.freq_type == MC_DPI_RECONF_FREQ_GLOBAL){
		energy_counters_set_frequency(state->available_frequencies[state->current_frequency_id],
		                              state->one_core_per_socket,
					      state->num_sockets,
		                              1);
	}
}

static unsigned long mc_dpi_reconfiguration_model_power(mc_dpi_library_state_t* state, unsigned long frequency, u_int16_t workers){
#if MC_DPI_POWER_USE_MODEL == 1
	return (std::pow(frequency,1.3)*(workers+2));
#else
	return frequency;
#endif
}

static double mc_dpi_reconfiguration_predict_load(mc_dpi_library_state_t* state, u_int16_t future_workers, unsigned int future_frequency_id){
	double prediction_modifier = ((double)future_workers * (double)state->available_frequencies[future_frequency_id]) / 
	                             ((double) state->single_farm_active_workers * (double)state->available_frequencies[state->current_frequency_id]);
	
	/**
	 *  Legend: 
	 *  
	 *  Rho    = Utilization Factor
	 *  Ts     = Service Time
	 *  Ta     = Interarrival Time
	 *  Psleep = % of time spent sleeping because the input queue of the worker was empty
	 *  Pwork  = % of time spent working on tasks ( = 100 - Psleep)
	 *
	 *  Rho = Ts / Ta or Rho = Pwork 
	 *  Rho is proportional to the inverse of the 'Power' of the configuration. More 'Power' we have (Workers * Frequency),
	 *  less utilized the system will be.
	 *  
	 *  CurrentRho : 1 / (CurrentWorkers * CurrentFrequency) = NextRho : 1 / (NextWorkers * NextFrequency)
	 *  NextRho = [(CurrentWorkers * CurrentFrequency) / (NextWorkers * NextFrequency)] * CurrentRho
	 **/

 	return ( (1.0 / prediction_modifier) * state->current_system_load );
}

#define ERROR_PERC 3.0

static void mc_dpi_reconfiguration_compute_best_feasible_solution(mc_dpi_library_state_t* state, unsigned int* next_workers, unsigned int* next_frequency_id){
	unsigned int i = 0, j = 0;
	double predicted_load = 0;

	double best_metric = DBL_MAX;
	double current_metric = 0;
	bool num_solutions_found = 0;

	double best_suboptimal_load = 0;
	unsigned int next_suboptimal_workers = 0;
	unsigned int next_suboptimal_frequency_id = 0;

	double best_rho_value = state->reconf_params.system_load_down_threshold + ((state->reconf_params.system_load_up_threshold - state->reconf_params.system_load_down_threshold) / 2.0);


        if(state->current_system_load >= 100.0 - ERROR_PERC){
		*next_workers = state->available_processors - 2;
		*next_frequency_id = state->num_available_frequencies - 1;
		return;
        }

	*next_workers = state->single_farm_active_workers;
	*next_frequency_id = state->current_frequency_id;

	for(i = 1; i <= state->available_processors - 2; i++){
		for(j = 0; j < state->num_available_frequencies; j++){

			switch(state->reconf_params.freq_strategy){
				case MC_DPI_RECONF_STRAT_GOVERNOR_CONSERVATIVE:
				case MC_DPI_RECONF_STRAT_GOVERNOR_ON_DEMAND:
				case MC_DPI_RECONF_STRAT_GOVERNOR_PERFORMANCE:{
					if(j != state->current_frequency_id){
						continue;
					}
				}break;
				default:{
					;
				}break;
			}

			predicted_load = mc_dpi_reconfiguration_predict_load(state, i, j);
			if(predicted_load < state->reconf_params.system_load_down_threshold + ERROR_PERC){
				if(predicted_load > best_suboptimal_load){
					best_suboptimal_load = predicted_load;
					next_suboptimal_workers = i;
					next_suboptimal_frequency_id = j;
				}
			}else if(predicted_load >= state->reconf_params.system_load_down_threshold + ERROR_PERC && 
			         predicted_load <= state->reconf_params.system_load_up_threshold - ERROR_PERC){
				//Feasible solution
				switch(state->reconf_params.freq_strategy){
					case MC_DPI_RECONF_STRAT_GOVERNOR_CONSERVATIVE:
					case MC_DPI_RECONF_STRAT_GOVERNOR_ON_DEMAND:
					case MC_DPI_RECONF_STRAT_GOVERNOR_PERFORMANCE:{
						*next_workers = i;
						state->last_prediction = predicted_load;
					}break;
					case MC_DPI_RECONF_STRAT_CORES_CONSERVATIVE:{
						if(num_solutions_found){
							return;
						}else{
							*next_workers = i;
							*next_frequency_id = j;
							state->last_prediction = predicted_load;
						}
					}break;

					case MC_DPI_RECONF_STRAT_POWER_CONSERVATIVE:{
						current_metric = mc_dpi_reconfiguration_model_power(state, state->available_frequencies[j], i);
						if(current_metric < best_metric){
							best_metric = current_metric;
							*next_workers = i;
							*next_frequency_id = j;
							state->last_prediction = predicted_load;
						}	       
					}break;

					default:{
						;					
       					}break;

				}
				++num_solutions_found;
			}
		}
	}

	if(num_solutions_found == 0){
		*next_workers = next_suboptimal_workers;
		*next_frequency_id = next_suboptimal_frequency_id;
		state->last_prediction = predicted_load;
	}
}

static void mc_dpi_reconfiguration_perform(mc_dpi_library_state_t* state){
	unsigned int next_frequency_id;
	unsigned int next_workers;

	mc_dpi_reconfiguration_compute_best_feasible_solution(state, &next_workers, &next_frequency_id);

	mc_dpi_set_num_workers(state, next_workers);
	mc_dpi_reconfiguration_update_system_frequencies(state, next_frequency_id);

	for(u_int16_t i = 0; i<state->available_processors - 2; i++){
		memset(state->load_samples[i], 0, sizeof(double)*state->reconf_params.num_samples);
	}
	state->current_load_sample=0;
	state->current_num_samples=0;
}

static void mc_dpi_reconfiguration_apply_policies(mc_dpi_library_state_t* state){
	double current_worker_load = 0;
	bool single_worker_out_up = false;
	bool single_worker_out_down = false;
	bool global_out_up = false;
	bool global_out_down = false;
	state->current_system_load = 0;

        for(u_int16_t i = 0; i<state->single_farm_active_workers; i++){
		dpi::dpi_L7_worker* current_worker = (dpi::dpi_L7_worker*)state->single_farm_workers->at(i);
		current_worker_load = mc_dpi_reconfiguration_get_worker_average_load(state, i);
		if(state->reconf_params.worker_load_up_threshold && current_worker_load > state->reconf_params.worker_load_up_threshold){
			single_worker_out_up = true;
		}else if(current_worker_load < state->reconf_params.worker_load_down_threshold){
			single_worker_out_down = true;
		}
	 	
		state->current_system_load += current_worker_load;
        }
	state->current_system_load = state->current_system_load / state->single_farm_active_workers;

	if(state->current_num_samples < state->reconf_params.stabilization_period + state->reconf_params.num_samples){
		return;
	}

	// Check thresholds 

	if(state->reconf_params.system_load_up_threshold && state->current_system_load > state->reconf_params.system_load_up_threshold){
		global_out_up = true;
	}else if(state->current_system_load < state->reconf_params.system_load_down_threshold){
		global_out_down = true;
	}

	if(single_worker_out_up || global_out_up || single_worker_out_down || global_out_down){
		mc_dpi_reconfiguration_perform(state);
	}
}

static void mc_dpi_reconfiguration_store_current_sample(mc_dpi_library_state_t* state){
	if(state->parallel_module_type == MC_DPI_PARALLELISM_FORM_ONE_FARM){
		unsigned int i;
		double instantaneous_system_load = 0;
		for(i=0; i<state->single_farm_active_workers; i++){
			dpi::dpi_L7_worker* current_worker = (dpi::dpi_L7_worker*)state->single_farm_workers->at(i);
			state->load_samples[i][state->current_load_sample] = 
			                    current_worker->get_worktime_percentage();
			instantaneous_system_load += state->load_samples[i][state->current_load_sample];
			current_worker->reset_worktime_percentage();
		}
		state->current_load_sample = (state->current_load_sample + 1) % state->reconf_params.num_samples;
		state->current_instantaneous_system_load = instantaneous_system_load / state->single_farm_active_workers;
		++state->current_num_samples;
	}
}

/**
 * Wait the end of the data processing.
 * @param state A pointer to the state of the library.
 */
void mc_dpi_wait_end(mc_dpi_library_state_t* state){
  	u_int64_t waited_secs = 0;
	mc_dpi_joules_counters now_joules, last_joules_counters;
	double load = 0;
	memset(&now_joules, 0, sizeof(mc_dpi_joules_counters));
	memset(&last_joules_counters, 0, sizeof(mc_dpi_joules_counters));

	last_joules_counters = mc_dpi_joules_counters_read(state);

	while(!state->terminating){
		sleep(1);
		++waited_secs;

		if(state->load_samples && 
		   (waited_secs % state->reconf_params.sampling_interval) == 0){
		  	mc_dpi_reconfiguration_store_current_sample(state);
			mc_dpi_reconfiguration_apply_policies(state); 
		}

		if(state->stats_callback && 
		   (waited_secs % state->collection_interval) == 0){
#if MC_DPI_AVG_RHO
			load = state->current_system_load;
#else
			load = state->current_instantaneous_system_load;
#endif
			now_joules = mc_dpi_joules_counters_read(state);
			state->stats_callback(state->single_farm_active_workers,
					      state->available_frequencies[state->current_frequency_id],
			                      mc_dpi_joules_counters_diff(state, 
			                                                  now_joules,
			                                                  last_joules_counters),
			                                                  load);
			last_joules_counters = now_joules;
		}
	}

        gettimeofday(&state->stop_time,NULL);
	state->is_running=0;
}


/****************************************/
/*        Status change API calls       */
/****************************************/

/**
 * Changes the number of workers in the L7 farm. This
 * is possible only when the configuration with the single
 * farm is used.
 * It can be used while the farm is running.
 * @param state       A pointer to the state of the library.
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 **/
u_int8_t mc_dpi_set_num_workers(mc_dpi_library_state_t *state,
		                       u_int16_t num_workers){

#if (DPI_FLOW_TABLE_USE_MEMORY_POOL == 1) || (DPI_MULTICORE_DEFAULT_GRAIN_SIZE != 1) //TODO: Implement
	return DPI_STATE_UPDATE_FAILURE;
#endif
	if(num_workers == state->single_farm_active_workers){
		return DPI_STATE_UPDATE_SUCCESS;
	}

	if(state->parallel_module_type == MC_DPI_PARALLELISM_FORM_ONE_FARM && num_workers>=1 && num_workers<=state->available_processors - 2){
		ticks s = getticks();
		mc_dpi_freeze(state);
		state->single_farm_active_workers=num_workers;
		debug_print("%s\n","[mc_dpi_api.cpp]: Changing v4 table partitions");
		dpi_flow_table_setup_partitions_v4((dpi_flow_DB_v4_t*)state->sequential_state->db4, 
							state->single_farm_active_workers);
		debug_print("%s\n","[mc_dpi_api.cpp]: Changing v6 table partitions");
		dpi_flow_table_setup_partitions_v6((dpi_flow_DB_v6_t*)state->sequential_state->db6, 
							state->single_farm_active_workers);
		if(state->reconf_params.migrate_collector){
			state->collector_proc_id = state->mapping[num_workers + 1];
		}
		mc_dpi_unfreeze(state);
		return DPI_STATE_UPDATE_SUCCESS;
	}else{
		return DPI_STATE_UPDATE_FAILURE;
	}
}

/**
 * Sets the maximum number of times that the library tries to guess the
 * protocol. During the flow protocol identification, after this number
 * of trials, in the case in which it cannot decide between two or more
 * protocols, one of them will be chosen, otherwise DPI_PROTOCOL_UNKNOWN
 * will be returned.
 * @param state       A pointer to the state of the library.
 * @param max_trials  The maximum number of trials.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_set_max_trials(mc_dpi_library_state_t *state,
		                       u_int16_t max_trials){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_set_max_trials(state->sequential_state, max_trials);
	mc_dpi_unfreeze(state);
	return r;
}



/**
 * Enable IPv4 defragmentation.
 * @param state        A pointer to the library state.
 * @param table_size   The size of the table to be used to store IPv4
 *                     fragments informations.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *          updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv4_fragmentation_enable(mc_dpi_library_state_t *state,
		                                  u_int16_t table_size){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv4_fragmentation_enable(state->sequential_state,
		                        table_size);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Enable IPv6 defragmentation.
 * @param state        A pointer to the library state.
 * @param table_size   The size of the table to be used to store IPv6
 *                     fragments informations.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv6_fragmentation_enable(mc_dpi_library_state_t *state,
		                                  u_int16_t table_size){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv6_fragmentation_enable(state->sequential_state,
		                        table_size);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Sets the amount of memory that a single host can use for IPv4
 * defragmentation.
 * @param state                   A pointer to the library state.
 * @param per_host_memory_limit   The maximum amount of memory that
 *                                 any IPv4 host can use.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv4_fragmentation_set_per_host_memory_limit(
		mc_dpi_library_state_t *state,
		u_int32_t per_host_memory_limit){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv4_fragmentation_set_per_host_memory_limit(
			state->sequential_state,
			per_host_memory_limit);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Sets the amount of memory that a single host can use for IPv6
 * defragmentation.
 * @param state                   A pointer to the library state.
 * @param per_host_memory_limit   The maximum amount of memory that
 *                                any IPv6 host can use.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv6_fragmentation_set_per_host_memory_limit(
		mc_dpi_library_state_t *state,
		u_int32_t per_host_memory_limit){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv6_fragmentation_set_per_host_memory_limit(
			state->sequential_state,
			per_host_memory_limit);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Sets the total amount of memory that can be used for IPv4
 * defragmentation.
 * If fragmentation is disabled and then enabled, this information
 * must be passed again.
 * Otherwise default value will be used.
 * @param state               A pointer to the state of the library
 * @param totel_memory_limit  The maximum amount of memory that can
 *                             be used for IPv4 defragmentation.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv4_fragmentation_set_total_memory_limit(
		mc_dpi_library_state_t *state,
		u_int32_t total_memory_limit){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv4_fragmentation_set_total_memory_limit(
			state->sequential_state, total_memory_limit);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Sets the total amount of memory that can be used for
 * IPv6 defragmentation.
 * If fragmentation is disabled and then enabled, this information
 * must be passed again.
 * Otherwise default value will be used.
 * @param state               A pointer to the state of the library
 * @param totel_memory_limit  The maximum amount of memory that can
 *                            be used for IPv6 defragmentation.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv6_fragmentation_set_total_memory_limit(
		mc_dpi_library_state_t *state,
		u_int32_t total_memory_limit){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv6_fragmentation_set_total_memory_limit(
			state->sequential_state, total_memory_limit);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Sets the maximum time (in seconds) that can be spent to
 * reassembly an IPv4 fragmented datagram.
 * Is the maximum time gap between the first and last fragments
 * of the datagram.
 * @param state            A pointer to the state of the library.
 * @param timeout_seconds  The reassembly timeout.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been
 *         successfully updated. DPI_STATE_UPDATE_FAILURE if the
 *         state has not been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv4_fragmentation_set_reassembly_timeout(
		mc_dpi_library_state_t *state,
		u_int8_t timeout_seconds){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv4_fragmentation_set_reassembly_timeout(
			state->sequential_state, timeout_seconds);
	mc_dpi_unfreeze(state);
	return r;

}

/**
 * Sets the maximum time (in seconds) that can be spent to reassembly
 * an IPv6 fragmented datagram.
 * Is the maximum time gap between the first and last fragments of
 * the datagram.
 * @param state            A pointer to the state of the library.
 * @param timeout_seconds  The reassembly timeout.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv6_fragmentation_set_reassembly_timeout(
		mc_dpi_library_state_t *state,
		u_int8_t timeout_seconds){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv6_fragmentation_set_reassembly_timeout(
			state->sequential_state, timeout_seconds);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Disable IPv4 defragmentation.
 * @param state A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been
 *         successfully updated. DPI_STATE_UPDATE_FAILURE if the
 *         state has not been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv4_fragmentation_disable(mc_dpi_library_state_t *state){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv4_fragmentation_disable(state->sequential_state);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Disable IPv6 defragmentation.
 * @param state A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_ipv6_fragmentation_disable(mc_dpi_library_state_t *state){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_ipv6_fragmentation_disable(state->sequential_state);
	mc_dpi_unfreeze(state);
	return r;
}



/**
 * If enabled, the library will reorder out of order TCP packets
 * (enabled by default).
 * @param state  A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been
 *         successfully updated. DPI_STATE_UPDATE_FAILURE if the state
 *         has not been changed because a problem happened.
 */
u_int8_t mc_dpi_tcp_reordering_enable(mc_dpi_library_state_t* state){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_tcp_reordering_enable(state->sequential_state);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * If it is called, the library will not reorder out of order TCP packets.
 * Out-of-order segments will be delivered to the inspector as they
 * arrive. This means that the inspector may not be able to identify
 * the application protocol. Moreover, if there are callbacks saved
 * for TCP based protocols, if TCP reordering is disabled, the
 * extracted informations could be erroneous or incomplete.
 * @param state A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_tcp_reordering_disable(mc_dpi_library_state_t* state){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_tcp_reordering_disable(state->sequential_state);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Enable a protocol inspector.
 * @param state         A pointer to the state of the library.
 * @param protocol      The protocol to enable.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_set_protocol(mc_dpi_library_state_t *state,
		                     dpi_protocol_t protocol){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_set_protocol(state->sequential_state, protocol);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Disable a protocol inspector.
 * @param state       A pointer to the state of the library.
 * @param protocol    The protocol to disable.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_delete_protocol(mc_dpi_library_state_t *state,
		                        dpi_protocol_t protocol){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_delete_protocol(state->sequential_state, protocol);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Enable all the protocol inspector.
 * @param state      A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_inspect_all(mc_dpi_library_state_t *state){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_inspect_all(state->sequential_state);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Disable all the protocol inspector.
 * @param state      A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_inspect_nothing(mc_dpi_library_state_t *state){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_inspect_nothing(state->sequential_state);
	mc_dpi_unfreeze(state);
	return r;
}


/**
 * Sets the callback that will be called when a flow expires.
 * (Valid only if stateful API is used).
 * @param state     A pointer to the state of the library.
 * @param cleaner   The callback used to clear the user state.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been
 *         successfully updated. DPI_STATE_UPDATE_FAILURE if
 *         the state has not been changed because a problem
 *         happened.
 */
u_int8_t mc_dpi_set_flow_cleaner_callback(
		mc_dpi_library_state_t* state,
		dpi_flow_cleaner_callback* cleaner){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_set_flow_cleaner_callback(
			state->sequential_state, cleaner);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Sets callbacks informations. When a protocol is identified the
 * default behavior is to not inspect the packets belonging to that
 * flow anymore and keep simply returning the same protocol identifier.
 *
 * If a callback is enabled for a certain protocol, then we keep
 * inspecting all the new flows with that protocol in order to
 * invoke the callbacks specified by the user on the various parts
 * of the message. Moreover, if the application protocol uses TCP,
 * then we have the additional cost of TCP reordering for all the
 * segments. Is highly recommended to enable TCP reordering if it is
 * not already enabled (remember that is enabled by default).
 * Otherwise the informations extracted could be erroneous/incomplete.
 *
 * The pointers to the data passed to the callbacks are valid only for
 * the duration of the callback.
 *
 * @param state       A pointer to the state of the library.
 * @param callbacks   A pointer to HTTP callbacks.
 * @param user_data   A pointer to global user HTTP data. This pointer
 *                    will be passed to any HTTP callback when it is
 *                    invoked.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 *
 **/
u_int8_t mc_dpi_http_activate_callbacks(
		mc_dpi_library_state_t* state,
		dpi_http_callbacks_t* callbacks,
		void* user_data){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_http_activate_callbacks(state->sequential_state,
		                      callbacks,
		                      user_data);
	mc_dpi_unfreeze(state);
	return r;
}

/**
 * Remove the internal structure used to store callbacks informations.
 * user_data is not freed/modified.
 * @param state       A pointer to the state of the library.
 *
 * @return DPI_STATE_UPDATE_SUCCESS If the state has been successfully
 *         updated. DPI_STATE_UPDATE_FAILURE if the state has not
 *         been changed because a problem happened.
 */
u_int8_t mc_dpi_http_disable_callbacks(mc_dpi_library_state_t* state){
	u_int8_t r;
	mc_dpi_freeze(state);
	r=dpi_http_disable_callbacks(state->sequential_state);
	mc_dpi_unfreeze(state);
	return r;
}

