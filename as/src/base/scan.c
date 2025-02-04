/*
 * scan.c
 *
 * Copyright (C) 2015 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/
 */

//==============================================================================
// Includes.
//

#include "base/scan.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "aerospike/as_atomic.h"
#include "aerospike/as_list.h"
#include "aerospike/as_module.h"
#include "aerospike/as_string.h"
#include "aerospike/as_val.h"
#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_clock.h"
#include "citrusleaf/cf_digest.h"
#include "citrusleaf/cf_ll.h"
#include "citrusleaf/cf_vector.h"

#include "cf_mutex.h"
#include "cf_thread.h"
#include "dynbuf.h"
#include "fault.h"
#include "socket.h"

#include "base/aggr.h"
#include "base/cfg.h"
#include "base/datamodel.h"
#include "base/index.h"
#include "base/monitor.h"
#include "base/predexp.h"
#include "base/proto.h"
#include "base/scan_job.h"
#include "base/scan_manager.h"
#include "base/secondary_index.h"
#include "base/service.h"
#include "base/transaction.h"
#include "fabric/exchange.h"
#include "fabric/partition.h"
#include "transaction/rw_utils.h"
#include "transaction/udf.h"
#include "transaction/write.h"



//==============================================================================
// Typedefs and forward declarations.
//

//----------------------------------------------------------
// Scan types.
//

typedef enum {
	SCAN_TYPE_BASIC		= 0,
	SCAN_TYPE_AGGR		= 1,
	SCAN_TYPE_UDF_BG	= 2,
	SCAN_TYPE_OPS_BG	= 3,

	SCAN_TYPE_UNKNOWN	= -1
} scan_type;

static inline const char*
scan_type_str(scan_type type)
{
	switch (type) {
	case SCAN_TYPE_BASIC:
		return "basic";
	case SCAN_TYPE_AGGR:
		return "aggregation";
	case SCAN_TYPE_UDF_BG:
		return "background-udf";
	case SCAN_TYPE_OPS_BG:
		return "background-ops";
	default:
		return "?";
	}
}

//----------------------------------------------------------
// scan_job - derived classes' public methods.
//

int basic_scan_job_start(as_transaction* tr, as_namespace* ns, uint16_t set_id);
int aggr_scan_job_start(as_transaction* tr, as_namespace* ns, uint16_t set_id);
int udf_bg_scan_job_start(as_transaction* tr, as_namespace* ns, uint16_t set_id);
int ops_bg_scan_job_start(as_transaction* tr, as_namespace* ns, uint16_t set_id);

//----------------------------------------------------------
// Non-class-specific utilities.
//

typedef struct scan_options_s {
	int			priority;
	bool		fail_on_cluster_change;
	uint32_t	sample_pct;
} scan_options;

int get_scan_set_id(as_transaction* tr, as_namespace* ns, uint16_t* p_set_id);
scan_type get_scan_type(as_transaction* tr);
bool get_scan_options(as_transaction* tr, scan_options* options);
bool get_scan_rps(as_transaction* tr, uint32_t* rps);
void convert_old_priority(int old_priority, uint32_t* rps);
bool validate_background_scan_rps(const as_namespace* ns, uint32_t* rps);
bool get_scan_socket_timeout(as_transaction* tr, uint32_t* timeout);
bool get_scan_predexp(as_transaction* tr, predexp_eval_t** p_predexp);
size_t send_blocking_response_chunk(as_file_handle* fd_h, uint8_t* buf, size_t size, int32_t timeout);
static inline bool excluded_set(as_index* r, uint16_t set_id);



//==============================================================================
// Constants.
//

#define LOW_PRIORITY_RPS 5000 // for compatibility with old clients

const size_t INIT_BUF_BUILDER_SIZE = 1024 * 1024 * 2;
const size_t SCAN_CHUNK_LIMIT = 1024 * 1024;

#define MAX_ACTIVE_TRANSACTIONS 200



//==============================================================================
// Public API.
//

void
as_scan_init(void)
{
	as_scan_manager_init();
}

int
as_scan(as_transaction* tr, as_namespace* ns)
{
	int result;
	uint16_t set_id = INVALID_SET_ID;

	if ((result = get_scan_set_id(tr, ns, &set_id)) != AS_OK) {
		return result;
	}

	switch (get_scan_type(tr)) {
	case SCAN_TYPE_BASIC:
		result = basic_scan_job_start(tr, ns, set_id);
		break;
	case SCAN_TYPE_AGGR:
		result = aggr_scan_job_start(tr, ns, set_id);
		break;
	case SCAN_TYPE_UDF_BG:
		result = udf_bg_scan_job_start(tr, ns, set_id);
		break;
	case SCAN_TYPE_OPS_BG:
		result = ops_bg_scan_job_start(tr, ns, set_id);
		break;
	default:
		cf_warning(AS_SCAN, "can't identify scan type");
		result = AS_ERR_PARAMETER;
		break;
	}

	return result;
}

void
as_scan_limit_finished_jobs(void)
{
	as_scan_manager_limit_finished_jobs();
}

int
as_scan_get_active_job_count(void)
{
	return as_scan_manager_get_active_job_count();
}

int
as_scan_list(char* name, cf_dyn_buf* db)
{
	(void)name;

	as_mon_info_cmd(AS_MON_MODULES[SCAN_MOD], NULL, 0, 0, db);
	return 0;
}

as_mon_jobstat*
as_scan_get_jobstat(uint64_t trid)
{
	return as_scan_manager_get_job_info(trid);
}

as_mon_jobstat*
as_scan_get_jobstat_all(int* size)
{
	return as_scan_manager_get_info(size);
}

int
as_scan_abort(uint64_t trid)
{
	return as_scan_manager_abort_job(trid) ? 0 : -1;
}

int
as_scan_abort_all(void)
{
	return as_scan_manager_abort_all_jobs();
}


//==============================================================================
// Non-class-specific utilities.
//

int
get_scan_set_id(as_transaction* tr, as_namespace* ns, uint16_t* p_set_id)
{
	uint16_t set_id = INVALID_SET_ID;
	as_msg_field* f = as_transaction_has_set(tr) ?
			as_msg_field_get(&tr->msgp->msg, AS_MSG_FIELD_TYPE_SET) : NULL;

	if (f && as_msg_field_get_value_sz(f) != 0) {
		uint32_t set_name_len = as_msg_field_get_value_sz(f);
		char set_name[set_name_len + 1];

		memcpy(set_name, f->data, set_name_len);
		set_name[set_name_len] = '\0';
		set_id = as_namespace_get_set_id(ns, set_name);

		if (set_id == INVALID_SET_ID) {
			cf_warning(AS_SCAN, "scan msg from %s has unrecognized set %s",
					tr->from.proto_fd_h->client, set_name);
			return AS_ERR_NOT_FOUND;
		}
	}

	*p_set_id = set_id;

	return AS_OK;
}

scan_type
get_scan_type(as_transaction* tr)
{
	if (! as_transaction_is_udf(tr)) {
		return (tr->msgp->msg.info2 & AS_MSG_INFO2_WRITE) != 0 ?
				SCAN_TYPE_OPS_BG : SCAN_TYPE_BASIC;
	}

	as_msg_field* udf_op_f = as_msg_field_get(&tr->msgp->msg,
			AS_MSG_FIELD_TYPE_UDF_OP);

	if (udf_op_f && *udf_op_f->data == (uint8_t)AS_UDF_OP_AGGREGATE) {
		return SCAN_TYPE_AGGR;
	}

	if (udf_op_f && *udf_op_f->data == (uint8_t)AS_UDF_OP_BACKGROUND) {
		return SCAN_TYPE_UDF_BG;
	}

	return SCAN_TYPE_UNKNOWN;
}

bool
get_scan_options(as_transaction* tr, scan_options* options)
{
	if (! as_transaction_has_scan_options(tr)) {
		return true;
	}

	as_msg_field* f = as_msg_field_get(&tr->msgp->msg,
			AS_MSG_FIELD_TYPE_SCAN_OPTIONS);

	if (as_msg_field_get_value_sz(f) != 2) {
		cf_warning(AS_SCAN, "scan msg options field size not 2");
		return false;
	}

	options->priority = AS_MSG_FIELD_SCAN_PRIORITY(f->data[0]);
	options->fail_on_cluster_change =
			(AS_MSG_FIELD_SCAN_FAIL_ON_CLUSTER_CHANGE & f->data[0]) != 0;
	options->sample_pct = f->data[1];

	return true;
}

bool
get_scan_rps(as_transaction* tr, uint32_t* rps)
{
	if (! as_transaction_has_recs_per_sec(tr)) {
		return true;
	}

	as_msg_field* f = as_msg_field_get(&tr->msgp->msg,
			AS_MSG_FIELD_TYPE_RECS_PER_SEC);

	if (as_msg_field_get_value_sz(f) != 4) {
		cf_warning(AS_SCAN, "scan recs-per-sec field size not 4");
		return false;
	}

	*rps = cf_swap_from_be32(*(uint32_t*)f->data);

	return true;
}

void
convert_old_priority(int old_priority, uint32_t* rps)
{
	if (old_priority != 0 && *rps != 0) {
		cf_warning(AS_SCAN, "unexpected - scan has rps %u and priority %d",
				*rps, old_priority);
		return;
	}

	if (old_priority == 1 && *rps == 0) {
		cf_info(AS_SCAN, "low-priority scan from old client will use %u rps",
				LOW_PRIORITY_RPS);

		*rps = LOW_PRIORITY_RPS;
	}
}

bool
validate_background_scan_rps(const as_namespace* ns, uint32_t* rps)
{
	if (*rps > ns->background_scan_max_rps) {
		cf_warning(AS_SCAN, "scan rps %u exceeds 'background-scan-max-rps' %u",
				*rps, ns->background_scan_max_rps);
		return false;
	}

	if (*rps == 0) {
		*rps = ns->background_scan_max_rps;
	}

	return true;
}

bool
get_scan_socket_timeout(as_transaction* tr, uint32_t* timeout)
{
	if (! as_transaction_has_socket_timeout(tr)) {
		return true;
	}

	as_msg_field* f = as_msg_field_get(&tr->msgp->msg,
			AS_MSG_FIELD_TYPE_SOCKET_TIMEOUT);

	if (as_msg_field_get_value_sz(f) != 4) {
		cf_warning(AS_SCAN, "scan socket timeout field size not 4");
		return false;
	}

	*timeout = cf_swap_from_be32(*(uint32_t*)f->data);

	return true;
}

bool
get_scan_predexp(as_transaction* tr, predexp_eval_t** p_predexp)
{
	if (! as_transaction_has_predexp(tr)) {
		return true;
	}

	as_msg_field* f = as_msg_field_get(&tr->msgp->msg,
			AS_MSG_FIELD_TYPE_PREDEXP);

	*p_predexp = predexp_build(f);

	return *p_predexp != NULL;
}

size_t
send_blocking_response_chunk(as_file_handle* fd_h, uint8_t* buf, size_t size,
		int32_t timeout)
{
	cf_socket* sock = &fd_h->sock;
	as_proto proto;

	proto.version = PROTO_VERSION;
	proto.type = PROTO_TYPE_AS_MSG;
	proto.sz = size;
	as_proto_swap(&proto);

	if (cf_socket_send_all(sock, (uint8_t*)&proto, sizeof(as_proto),
			MSG_NOSIGNAL | MSG_MORE, timeout) < 0) {
		cf_warning(AS_SCAN, "error sending to %s - fd %d %s", fd_h->client,
				CSFD(sock), cf_strerror(errno));
		return 0;
	}

	if (cf_socket_send_all(sock, buf, size, MSG_NOSIGNAL, timeout) < 0) {
		cf_warning(AS_SCAN, "error sending to %s - fd %d sz %lu %s",
				fd_h->client, CSFD(sock), size, cf_strerror(errno));
		return 0;
	}

	return sizeof(as_proto) + size;
}

static inline bool
excluded_set(as_index* r, uint16_t set_id)
{
	return set_id != INVALID_SET_ID && set_id != as_index_get_set_id(r);
}

static inline void
throttle_sleep(as_scan_job* _job)
{
	uint32_t sleep_us = as_scan_job_throttle(_job);

	if (sleep_us != 0) {
		usleep(sleep_us);
	}
}



//==============================================================================
// conn_scan_job derived class implementation - not final class.
//

//----------------------------------------------------------
// conn_scan_job typedefs and forward declarations.
//

typedef struct conn_scan_job_s {
	// Base object must be first:
	as_scan_job		_base;

	// Derived class data:
	cf_mutex		fd_lock;
	as_file_handle*	fd_h;
	int32_t			fd_timeout;

	uint64_t		net_io_bytes;
} conn_scan_job;

void conn_scan_job_own_fd(conn_scan_job* job, as_file_handle* fd_h, uint32_t timeout);
void conn_scan_job_disown_fd(conn_scan_job* job);
void conn_scan_job_finish(conn_scan_job* job);
bool conn_scan_job_send_response(conn_scan_job* job, uint8_t* buf, size_t size);
void conn_scan_job_release_fd(conn_scan_job* job, bool force_close);
void conn_scan_job_info(conn_scan_job* job, as_mon_jobstat* stat);

//----------------------------------------------------------
// conn_scan_job API.
//

void
conn_scan_job_own_fd(conn_scan_job* job, as_file_handle* fd_h, uint32_t timeout)
{
	cf_mutex_init(&job->fd_lock);

	job->fd_h = fd_h;
	job->fd_timeout = timeout == 0 ? -1 : (int32_t)timeout;

	job->net_io_bytes = 0;
}

void
conn_scan_job_disown_fd(conn_scan_job* job)
{
	// Just undo conn_scan_job_own_fd(), nothing more.

	cf_mutex_destroy(&job->fd_lock);
}

void
conn_scan_job_finish(conn_scan_job* job)
{
	as_scan_job* _job = (as_scan_job*)job;

	if (job->fd_h) {
		// TODO - perhaps reflect in monitor if send fails?
		size_t size_sent = as_msg_send_fin_timeout(&job->fd_h->sock,
				_job->abandoned, job->fd_timeout);

		job->net_io_bytes += size_sent;
		conn_scan_job_release_fd(job, size_sent == 0);
	}

	cf_mutex_destroy(&job->fd_lock);
}

bool
conn_scan_job_send_response(conn_scan_job* job, uint8_t* buf, size_t size)
{
	as_scan_job* _job = (as_scan_job*)job;

	cf_mutex_lock(&job->fd_lock);

	if (! job->fd_h) {
		cf_mutex_unlock(&job->fd_lock);
		// Job already abandoned.
		return false;
	}

	size_t size_sent = send_blocking_response_chunk(job->fd_h, buf, size,
			job->fd_timeout);

	if (size_sent == 0) {
		int reason = errno == ETIMEDOUT ?
				AS_SCAN_ERR_RESPONSE_TIMEOUT : AS_SCAN_ERR_RESPONSE_ERROR;

		conn_scan_job_release_fd(job, true);
		cf_mutex_unlock(&job->fd_lock);
		as_scan_manager_abandon_job(_job, reason);
		return false;
	}

	job->net_io_bytes += size_sent;

	cf_mutex_unlock(&job->fd_lock);
	return true;
}

void
conn_scan_job_release_fd(conn_scan_job* job, bool force_close)
{
	job->fd_h->last_used = cf_getns();
	as_end_of_transaction(job->fd_h, force_close);
	job->fd_h = NULL;
}

void
conn_scan_job_info(conn_scan_job* job, as_mon_jobstat* stat)
{
	stat->net_io_bytes = job->net_io_bytes;
	stat->socket_timeout = job->fd_timeout;
}



//==============================================================================
// basic_scan_job derived class implementation.
//

//----------------------------------------------------------
// basic_scan_job typedefs and forward declarations.
//

typedef struct basic_scan_job_s {
	// Base object must be first:
	conn_scan_job	_base;

	// Derived class data:
	uint64_t		cluster_key;
	bool			fail_on_cluster_change;
	bool			no_bin_data;
	uint32_t		sample_pct;
	predexp_eval_t*	predexp;
	cf_vector*		bin_names;
} basic_scan_job;

void basic_scan_job_slice(as_scan_job* _job, as_partition_reservation* rsv);
void basic_scan_job_finish(as_scan_job* _job);
void basic_scan_job_destroy(as_scan_job* _job);
void basic_scan_job_info(as_scan_job* _job, as_mon_jobstat* stat);

const as_scan_vtable basic_scan_job_vtable = {
		basic_scan_job_slice,
		basic_scan_job_finish,
		basic_scan_job_destroy,
		basic_scan_job_info
};

typedef struct basic_scan_slice_s {
	basic_scan_job*		job;
	cf_buf_builder**	bb_r;
} basic_scan_slice;

void basic_scan_job_reduce_cb(as_index_ref* r_ref, void* udata);
bool basic_scan_predexp_filter_meta(const basic_scan_job* job, const as_record* r, predexp_eval_t** predexp);
cf_vector* bin_names_from_op(as_msg* m, int* result);

//----------------------------------------------------------
// basic_scan_job public API.
//

int
basic_scan_job_start(as_transaction* tr, as_namespace* ns, uint16_t set_id)
{
	basic_scan_job* job = cf_malloc(sizeof(basic_scan_job));
	as_scan_job* _job = (as_scan_job*)job;

	scan_options options = { .sample_pct = 100 };
	uint32_t rps = 0;
	uint32_t timeout = CF_SOCKET_TIMEOUT;
	predexp_eval_t* predexp = NULL;

	if (! get_scan_options(tr, &options) || ! get_scan_rps(tr, &rps) ||
			! get_scan_socket_timeout(tr, &timeout) ||
			! get_scan_predexp(tr, &predexp)) {
		cf_warning(AS_SCAN, "basic scan job failed msg field processing");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	convert_old_priority(options.priority, &rps);

	as_scan_job_init(_job, &basic_scan_job_vtable, as_transaction_trid(tr), ns,
			set_id, rps, tr->from.proto_fd_h->client);

	job->cluster_key = as_exchange_cluster_key();
	job->fail_on_cluster_change = options.fail_on_cluster_change;
	job->no_bin_data = (tr->msgp->msg.info1 & AS_MSG_INFO1_GET_NO_BINS) != 0;
	job->sample_pct = options.sample_pct;
	job->predexp = predexp;

	int result;

	job->bin_names = bin_names_from_op(&tr->msgp->msg, &result);

	if (! job->bin_names && result != AS_OK) {
		as_scan_job_destroy(_job);
		return result;
	}

	if (job->fail_on_cluster_change &&
			(ns->migrate_tx_partitions_remaining != 0 ||
			 ns->migrate_rx_partitions_remaining != 0)) {
		cf_warning(AS_SCAN, "basic scan job not started - migration");
		as_scan_job_destroy(_job);
		return AS_ERR_CLUSTER_KEY_MISMATCH;
	}

	// Take ownership of socket from transaction.
	conn_scan_job_own_fd((conn_scan_job*)job, tr->from.proto_fd_h, timeout);

	cf_info(AS_SCAN, "starting basic scan job %lu {%s:%s} rps %u sample-pct %u%s%s socket-timeout %u from %s",
			_job->trid, ns->name, as_namespace_get_set_name(ns, set_id),
			_job->rps, job->sample_pct,
			job->no_bin_data ? ", metadata-only" : "",
			job->fail_on_cluster_change ? ", fail-on-cluster-change" : "",
			timeout, _job->client);

	if ((result = as_scan_manager_start_job(_job)) != 0) {
		cf_warning(AS_SCAN, "basic scan job %lu failed to start (%d)",
				_job->trid, result);
		conn_scan_job_disown_fd((conn_scan_job*)job);
		as_scan_job_destroy(_job);
		return result;
	}

	return AS_OK;
}

//----------------------------------------------------------
// basic_scan_job mandatory scan_job interface.
//

void
basic_scan_job_slice(as_scan_job* _job, as_partition_reservation* rsv)
{
	basic_scan_job* job = (basic_scan_job*)_job;
	as_index_tree* tree = rsv->tree;
	cf_buf_builder* bb = cf_buf_builder_create(INIT_BUF_BUILDER_SIZE);
	uint64_t slice_start = cf_getms();
	basic_scan_slice slice = { job, &bb };

	if (job->sample_pct == 100) {
		as_index_reduce_live(tree, basic_scan_job_reduce_cb, (void*)&slice);
	}
	else {
		uint64_t sample_count =
				((as_index_tree_size(tree) * job->sample_pct) / 100);

		as_index_reduce_partial_live(tree, sample_count,
				basic_scan_job_reduce_cb, (void*)&slice);
	}

	if (bb->used_sz != 0) {
		conn_scan_job_send_response((conn_scan_job*)job, bb->buf, bb->used_sz);
	}

	cf_buf_builder_free(bb);

	cf_detail(AS_SCAN, "%s:%u basic scan job %lu in thread %d took %lu ms",
			rsv->ns->name, rsv->p->id, _job->trid, cf_thread_sys_tid(),
			cf_getms() - slice_start);
}

void
basic_scan_job_finish(as_scan_job* _job)
{
	conn_scan_job_finish((conn_scan_job*)_job);

	switch (_job->abandoned) {
	case 0:
		as_incr_uint64(&_job->ns->n_scan_basic_complete);
		break;
	case AS_SCAN_ERR_USER_ABORT:
		as_incr_uint64(&_job->ns->n_scan_basic_abort);
		break;
	case AS_SCAN_ERR_UNKNOWN:
	case AS_SCAN_ERR_CLUSTER_KEY:
	case AS_SCAN_ERR_RESPONSE_ERROR:
	case AS_SCAN_ERR_RESPONSE_TIMEOUT:
	default:
		as_incr_uint64(&_job->ns->n_scan_basic_error);
		break;
	}

	cf_info(AS_SCAN, "finished basic scan job %lu (%d)", _job->trid,
			_job->abandoned);
}

void
basic_scan_job_destroy(as_scan_job* _job)
{
	basic_scan_job* job = (basic_scan_job*)_job;

	if (job->bin_names) {
		cf_vector_destroy(job->bin_names);
	}

	predexp_destroy(job->predexp);
}

void
basic_scan_job_info(as_scan_job* _job, as_mon_jobstat* stat)
{
	strcpy(stat->job_type, scan_type_str(SCAN_TYPE_BASIC));
	conn_scan_job_info((conn_scan_job*)_job, stat);
}

//----------------------------------------------------------
// basic_scan_job utilities.
//

void
basic_scan_job_reduce_cb(as_index_ref* r_ref, void* udata)
{
	basic_scan_slice* slice = (basic_scan_slice*)udata;
	basic_scan_job* job = slice->job;
	as_scan_job* _job = (as_scan_job*)job;
	as_namespace* ns = _job->ns;

	if (_job->abandoned != 0) {
		as_record_done(r_ref, ns);
		return;
	}

	if (job->fail_on_cluster_change &&
			job->cluster_key != as_exchange_cluster_key()) {
		as_record_done(r_ref, ns);
		as_scan_manager_abandon_job(_job, AS_ERR_CLUSTER_KEY_MISMATCH);
		return;
	}

	as_index* r = r_ref->r;

	if (excluded_set(r, _job->set_id) || as_record_is_doomed(r, ns)) {
		as_record_done(r_ref, ns);
		return;
	}

	predexp_eval_t* predexp = NULL;

	if (! basic_scan_predexp_filter_meta(job, r, &predexp)) {
		as_record_done(r_ref, ns);
		as_incr_uint64(&_job->n_filtered_meta);
		return;
	}

	as_storage_rd rd;

	as_storage_record_open(ns, r, &rd);

	if (predexp != NULL && predexp_read_and_filter_bins(&rd, predexp) != 0) {
		as_storage_record_close(&rd);
		as_record_done(r_ref, ns);
		as_incr_uint64(&_job->n_filtered_bins);

		if (! ns->storage_data_in_memory) {
			throttle_sleep(_job);
		}

		return;
	}

	if (job->no_bin_data) {
		as_msg_make_response_bufbuilder(slice->bb_r, &rd, true, NULL);
	}
	else {
		if (as_storage_rd_load_n_bins(&rd) < 0) {
			cf_warning(AS_SCAN, "job %lu - record unreadable", _job->trid);
			as_storage_record_close(&rd);
			as_record_done(r_ref, ns);
			as_incr_uint64(&_job->n_failed);
			return;
		}

		as_bin stack_bins[ns->storage_data_in_memory ? 0 : rd.n_bins];

		if (as_storage_rd_load_bins(&rd, stack_bins)) {
			cf_warning(AS_SCAN, "job %lu - record unreadable", _job->trid);
			as_storage_record_close(&rd);
			as_record_done(r_ref, ns);
			as_incr_uint64(&_job->n_failed);
			return;
		}

		as_msg_make_response_bufbuilder(slice->bb_r, &rd, false,
				job->bin_names);
	}

	as_storage_record_close(&rd);
	as_record_done(r_ref, ns);
	as_incr_uint64(&_job->n_succeeded);

	throttle_sleep(_job);

	cf_buf_builder* bb = *slice->bb_r;

	// If we exceed the proto size limit, send accumulated data back to client
	// and reset the buf-builder to start a new proto.
	if (bb->used_sz > SCAN_CHUNK_LIMIT) {
		if (! conn_scan_job_send_response((conn_scan_job*)job, bb->buf,
				bb->used_sz)) {
			return;
		}

		cf_buf_builder_reset(bb);
	}
}

bool
basic_scan_predexp_filter_meta(const basic_scan_job* job, const as_record* r,
		predexp_eval_t** predexp)
{
	*predexp = job->predexp;

	if (*predexp == NULL) {
		return true;
	}

	as_namespace* ns = ((as_scan_job*)job)->ns;
	predexp_args_t predargs = { .ns = ns, .md = (as_record*)r };
	predexp_retval_t predrv = predexp_matches_metadata(*predexp, &predargs);

	if (predrv == PREDEXP_UNKNOWN) {
		return true; // caller must later check bins using *predexp
	}
	// else - caller will not need to apply filter later.

	*predexp = NULL;

	return predrv == PREDEXP_TRUE;
}

cf_vector*
bin_names_from_op(as_msg* m, int* result)
{
	*result = AS_OK;

	if (m->n_ops == 0) {
		return NULL;
	}

	cf_vector* v  = cf_vector_create(AS_BIN_NAME_MAX_SZ, m->n_ops, 0);

	as_msg_op* op = NULL;
	int n = 0;

	while ((op = as_msg_op_iterate(m, op, &n)) != NULL) {
		if (op->name_sz >= AS_BIN_NAME_MAX_SZ) {
			cf_warning(AS_SCAN, "basic scan job bin name too long");
			cf_vector_destroy(v);
			*result = AS_ERR_BIN_NAME;
			return NULL;
		}

		char bin_name[AS_BIN_NAME_MAX_SZ];

		memcpy(bin_name, op->name, op->name_sz);
		bin_name[op->name_sz] = 0;
		cf_vector_append_unique(v, (void*)bin_name);
	}

	return v;
}



//==============================================================================
// aggr_scan_job derived class implementation.
//

//----------------------------------------------------------
// aggr_scan_job typedefs and forward declarations.
//

typedef struct aggr_scan_job_s {
	// Base object must be first:
	conn_scan_job	_base;

	// Derived class data:
	as_aggr_call	aggr_call;
} aggr_scan_job;

void aggr_scan_job_slice(as_scan_job* _job, as_partition_reservation* rsv);
void aggr_scan_job_finish(as_scan_job* _job);
void aggr_scan_job_destroy(as_scan_job* _job);
void aggr_scan_job_info(as_scan_job* _job, as_mon_jobstat* stat);

const as_scan_vtable aggr_scan_job_vtable = {
		aggr_scan_job_slice,
		aggr_scan_job_finish,
		aggr_scan_job_destroy,
		aggr_scan_job_info
};

typedef struct aggr_scan_slice_s {
	aggr_scan_job*				job;
	cf_ll*						ll;
	cf_buf_builder**			bb_r;
	as_partition_reservation*	rsv;
} aggr_scan_slice;

bool aggr_scan_init(as_aggr_call* call, const as_transaction* tr);
void aggr_scan_job_reduce_cb(as_index_ref* r_ref, void* udata);
bool aggr_scan_add_digest(cf_ll* ll, cf_digest* keyd);
as_partition_reservation* aggr_scan_ptn_reserve(void* udata, as_namespace* ns,
		uint32_t pid, as_partition_reservation* rsv);
as_stream_status aggr_scan_ostream_write(void* udata, as_val* val);

const as_aggr_hooks scan_aggr_hooks = {
	.ostream_write = aggr_scan_ostream_write,
	.set_error     = NULL,
	.ptn_reserve   = aggr_scan_ptn_reserve,
	.ptn_release   = NULL,
	.pre_check     = NULL
};

void aggr_scan_add_val_response(aggr_scan_slice* slice, const as_val* val,
		bool success);

//----------------------------------------------------------
// aggr_scan_job public API.
//

int
aggr_scan_job_start(as_transaction* tr, as_namespace* ns, uint16_t set_id)
{
	aggr_scan_job* job = cf_malloc(sizeof(aggr_scan_job));
	as_scan_job* _job = (as_scan_job*)job;

	scan_options options = { .sample_pct = 100 };
	uint32_t rps = 0;
	uint32_t timeout = CF_SOCKET_TIMEOUT;

	if (! get_scan_options(tr, &options) || ! get_scan_rps(tr, &rps) ||
			! get_scan_socket_timeout(tr, &timeout)) {
		cf_warning(AS_SCAN, "aggregation scan job failed msg field processing");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	if (as_transaction_has_predexp(tr)) {
		cf_warning(AS_SCAN, "aggregation scans do not support predexp filters");
		cf_free(job);
		return AS_ERR_UNSUPPORTED_FEATURE;
	}

	convert_old_priority(options.priority, &rps);

	as_scan_job_init(_job, &aggr_scan_job_vtable, as_transaction_trid(tr), ns,
			set_id, rps, tr->from.proto_fd_h->client);

	if (! aggr_scan_init(&job->aggr_call, tr)) {
		cf_warning(AS_SCAN, "aggregation scan job failed call init");
		as_scan_job_destroy(_job);
		return AS_ERR_PARAMETER;
	}

	// Take ownership of socket from transaction.
	conn_scan_job_own_fd((conn_scan_job*)job, tr->from.proto_fd_h, timeout);

	cf_info(AS_SCAN, "starting aggregation scan job %lu {%s:%s} rps %u socket-timeout %u from %s",
			_job->trid, ns->name, as_namespace_get_set_name(ns, set_id),
			_job->rps, timeout, _job->client);

	int result = as_scan_manager_start_job(_job);

	if (result != 0) {
		cf_warning(AS_SCAN, "aggregation scan job %lu failed to start (%d)",
				_job->trid, result);
		conn_scan_job_disown_fd((conn_scan_job*)job);
		as_scan_job_destroy(_job);
		return result;
	}

	return AS_OK;
}

//----------------------------------------------------------
// aggr_scan_job mandatory scan_job interface.
//

void
aggr_scan_job_slice(as_scan_job* _job, as_partition_reservation* rsv)
{
	aggr_scan_job* job = (aggr_scan_job*)_job;
	cf_ll ll;

	cf_ll_init(&ll, as_index_keys_ll_destroy_fn, false);

	cf_buf_builder* bb = cf_buf_builder_create(INIT_BUF_BUILDER_SIZE);
	aggr_scan_slice slice = { job, &ll, &bb, rsv };

	as_index_reduce_live(rsv->tree, aggr_scan_job_reduce_cb, (void*)&slice);

	if (cf_ll_size(&ll) != 0) {
		as_result result;
		as_result_init(&result);

		int ret = as_aggr_process(_job->ns, &job->aggr_call, &ll, (void*)&slice,
				&result);

		if (ret != 0) {
			char* rs = as_module_err_string(ret);

			if (result.value) {
				as_string* lua_s = as_string_fromval(result.value);
				char* lua_err = (char*)as_string_tostring(lua_s);

				if (lua_err) {
					int l_rs_len = strlen(rs);

					rs = cf_realloc(rs, l_rs_len + strlen(lua_err) + 4);
					sprintf(&rs[l_rs_len], " : %s", lua_err);
				}
			}

			const as_val* v = (as_val*)as_string_new(rs, false);

			aggr_scan_add_val_response(&slice, v, false);
			as_val_destroy(v);
			cf_free(rs);
			as_scan_manager_abandon_job(_job, AS_ERR_UNKNOWN);
		}

		as_result_destroy(&result);
	}

	cf_ll_reduce(&ll, true, as_index_keys_ll_reduce_fn, NULL);

	if (bb->used_sz != 0) {
		conn_scan_job_send_response((conn_scan_job*)job, bb->buf, bb->used_sz);
	}

	cf_buf_builder_free(bb);
}

void
aggr_scan_job_finish(as_scan_job* _job)
{
	aggr_scan_job* job = (aggr_scan_job*)_job;

	conn_scan_job_finish((conn_scan_job*)job);

	if (job->aggr_call.def.arglist) {
		as_list_destroy(job->aggr_call.def.arglist);
		job->aggr_call.def.arglist = NULL;
	}

	switch (_job->abandoned) {
	case 0:
		as_incr_uint64(&_job->ns->n_scan_aggr_complete);
		break;
	case AS_SCAN_ERR_USER_ABORT:
		as_incr_uint64(&_job->ns->n_scan_aggr_abort);
		break;
	case AS_SCAN_ERR_UNKNOWN:
	case AS_SCAN_ERR_CLUSTER_KEY:
	case AS_SCAN_ERR_RESPONSE_ERROR:
	case AS_SCAN_ERR_RESPONSE_TIMEOUT:
	default:
		as_incr_uint64(&_job->ns->n_scan_aggr_error);
		break;
	}

	cf_info(AS_SCAN, "finished aggregation scan job %lu (%d)", _job->trid,
			_job->abandoned);
}

void
aggr_scan_job_destroy(as_scan_job* _job)
{
	aggr_scan_job* job = (aggr_scan_job*)_job;

	if (job->aggr_call.def.arglist) {
		as_list_destroy(job->aggr_call.def.arglist);
	}
}

void
aggr_scan_job_info(as_scan_job* _job, as_mon_jobstat* stat)
{
	strcpy(stat->job_type, scan_type_str(SCAN_TYPE_AGGR));
	conn_scan_job_info((conn_scan_job*)_job, stat);
}

//----------------------------------------------------------
// aggr_scan_job utilities.
//

bool
aggr_scan_init(as_aggr_call* call, const as_transaction* tr)
{
	if (! udf_def_init_from_msg(&call->def, tr)) {
		return false;
	}

	call->aggr_hooks = &scan_aggr_hooks;

	return true;
}

void
aggr_scan_job_reduce_cb(as_index_ref* r_ref, void* udata)
{
	aggr_scan_slice* slice = (aggr_scan_slice*)udata;
	aggr_scan_job* job = slice->job;
	as_scan_job* _job = (as_scan_job*)job;
	as_namespace* ns = _job->ns;

	if (_job->abandoned != 0) {
		as_record_done(r_ref, ns);
		return;
	}

	as_index* r = r_ref->r;

	if (excluded_set(r, _job->set_id) || as_record_is_doomed(r, ns)) {
		as_record_done(r_ref, ns);
		return;
	}

	if (! aggr_scan_add_digest(slice->ll, &r->keyd)) {
		as_record_done(r_ref, ns);
		as_scan_manager_abandon_job(_job, AS_ERR_UNKNOWN);
		return;
	}

	as_record_done(r_ref, ns);
	as_incr_uint64(&_job->n_succeeded);

	throttle_sleep(_job);
}

bool
aggr_scan_add_digest(cf_ll* ll, cf_digest* keyd)
{
	as_index_keys_ll_element* tail_e = (as_index_keys_ll_element*)ll->tail;
	as_index_keys_arr* keys_arr;

	if (tail_e) {
		keys_arr = tail_e->keys_arr;

		if (keys_arr->num == AS_INDEX_KEYS_PER_ARR) {
			tail_e = NULL;
		}
	}

	if (! tail_e) {
		if (! (keys_arr = as_index_get_keys_arr())) {
			return false;
		}

		tail_e = cf_malloc(sizeof(as_index_keys_ll_element));

		tail_e->keys_arr = keys_arr;
		cf_ll_append(ll, (cf_ll_element*)tail_e);
	}

	keys_arr->pindex_digs[keys_arr->num] = *keyd;
	keys_arr->num++;

	return true;
}

as_partition_reservation*
aggr_scan_ptn_reserve(void* udata, as_namespace* ns, uint32_t pid,
		as_partition_reservation* rsv)
{
	aggr_scan_slice* slice = (aggr_scan_slice*)udata;

	return slice->rsv;
}

as_stream_status
aggr_scan_ostream_write(void* udata, as_val* val)
{
	aggr_scan_slice* slice = (aggr_scan_slice*)udata;

	if (val) {
		aggr_scan_add_val_response(slice, val, true);
		as_val_destroy(val);
	}

	return AS_STREAM_OK;
}

void
aggr_scan_add_val_response(aggr_scan_slice* slice, const as_val* val,
		bool success)
{
	uint32_t size = as_particle_asval_client_value_size(val);

	as_msg_make_val_response_bufbuilder(val, slice->bb_r, size, success);

	cf_buf_builder* bb = *slice->bb_r;
	conn_scan_job* conn_job = (conn_scan_job*)slice->job;

	// If we exceed the proto size limit, send accumulated data back to client
	// and reset the buf-builder to start a new proto.
	if (bb->used_sz > SCAN_CHUNK_LIMIT) {
		if (! conn_scan_job_send_response(conn_job, bb->buf, bb->used_sz)) {
			return;
		}

		cf_buf_builder_reset(bb);
	}
}



//==============================================================================
// udf_bg_scan_job derived class implementation.
//

//----------------------------------------------------------
// udf_bg_scan_job typedefs and forward declarations.
//

typedef struct udf_bg_scan_job_s {
	// Base object must be first:
	as_scan_job		_base;

	// Derived class data:
	iudf_origin		origin;
	uint32_t		n_active_tr;
} udf_bg_scan_job;

void udf_bg_scan_job_slice(as_scan_job* _job, as_partition_reservation* rsv);
void udf_bg_scan_job_finish(as_scan_job* _job);
void udf_bg_scan_job_destroy(as_scan_job* _job);
void udf_bg_scan_job_info(as_scan_job* _job, as_mon_jobstat* stat);

const as_scan_vtable udf_bg_scan_job_vtable = {
		udf_bg_scan_job_slice,
		udf_bg_scan_job_finish,
		udf_bg_scan_job_destroy,
		udf_bg_scan_job_info
};

void udf_bg_scan_job_reduce_cb(as_index_ref* r_ref, void* udata);
void udf_bg_scan_tr_complete(void* udata, int result);

//----------------------------------------------------------
// udf_bg_scan_job public API.
//

int
udf_bg_scan_job_start(as_transaction* tr, as_namespace* ns, uint16_t set_id)
{
	udf_bg_scan_job* job = cf_malloc(sizeof(udf_bg_scan_job));
	as_scan_job* _job = (as_scan_job*)job;

	scan_options options = { .sample_pct = 100 };
	uint32_t rps = 0;

	if (! get_scan_options(tr, &options) || ! get_scan_rps(tr, &rps)) {
		cf_warning(AS_SCAN, "udf-bg scan job failed msg field processing");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	convert_old_priority(options.priority, &rps);

	if (! validate_background_scan_rps(ns, &rps)) {
		cf_warning(AS_SCAN, "udf-bg scan job failed rps check");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	predexp_eval_t* predexp = NULL;

	if (! get_scan_predexp(tr, &predexp)) {
		cf_warning(AS_SCAN, "udf-bg scan job failed predexp processing");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	as_scan_job_init(_job, &udf_bg_scan_job_vtable, as_transaction_trid(tr), ns,
			set_id, rps, tr->from.proto_fd_h->client);

	job->n_active_tr = 0;

	if (! udf_def_init_from_msg(&job->origin.def, tr)) {
		cf_warning(AS_SCAN, "udf-bg scan job failed def init");
		as_scan_job_destroy(_job);
		return AS_ERR_PARAMETER;
	}

	uint8_t info2 = AS_MSG_INFO2_WRITE |
			(tr->msgp->msg.info2 & AS_MSG_INFO2_DURABLE_DELETE);

	job->origin.msgp =
			as_msg_create_internal(ns->name, 0, info2, 0, 0, NULL, 0);

	job->origin.predexp = predexp;
	job->origin.cb = udf_bg_scan_tr_complete;
	job->origin.udata = (void*)job;

	cf_info(AS_SCAN, "starting udf-bg scan job %lu {%s:%s} rps %u from %s",
			_job->trid, ns->name, as_namespace_get_set_name(ns, set_id),
			_job->rps, _job->client);

	int result = as_scan_manager_start_job(_job);

	if (result != 0) {
		cf_warning(AS_SCAN, "udf-bg scan job %lu failed to start (%d)",
				_job->trid, result);
		as_scan_job_destroy(_job);
		return result;
	}

	if (as_msg_send_fin(&tr->from.proto_fd_h->sock, AS_OK)) {
		tr->from.proto_fd_h->last_used = cf_getns();
		as_end_of_transaction_ok(tr->from.proto_fd_h);
	}
	else {
		cf_warning(AS_SCAN, "udf-bg scan job error sending fin");
		as_end_of_transaction_force_close(tr->from.proto_fd_h);
		// No point returning an error - it can't be reported on this socket.
	}

	tr->from.proto_fd_h = NULL;

	return AS_OK;
}

//----------------------------------------------------------
// udf_bg_scan_job mandatory scan_job interface.
//

void
udf_bg_scan_job_slice(as_scan_job* _job, as_partition_reservation* rsv)
{
	as_index_reduce_live(rsv->tree, udf_bg_scan_job_reduce_cb, (void*)_job);
}

void
udf_bg_scan_job_finish(as_scan_job* _job)
{
	udf_bg_scan_job* job = (udf_bg_scan_job*)_job;

	while (job->n_active_tr != 0) {
		usleep(100);
	}

	switch (_job->abandoned) {
	case 0:
		as_incr_uint64(&_job->ns->n_scan_udf_bg_complete);
		break;
	case AS_SCAN_ERR_USER_ABORT:
		as_incr_uint64(&_job->ns->n_scan_udf_bg_abort);
		break;
	case AS_SCAN_ERR_UNKNOWN:
	case AS_SCAN_ERR_CLUSTER_KEY:
	default:
		as_incr_uint64(&_job->ns->n_scan_udf_bg_error);
		break;
	}

	cf_info(AS_SCAN, "finished udf-bg scan job %lu (%d)", _job->trid,
			_job->abandoned);
}

void
udf_bg_scan_job_destroy(as_scan_job* _job)
{
	udf_bg_scan_job* job = (udf_bg_scan_job*)_job;

	iudf_origin_destroy(&job->origin);
}

void
udf_bg_scan_job_info(as_scan_job* _job, as_mon_jobstat* stat)
{
	strcpy(stat->job_type, scan_type_str(SCAN_TYPE_UDF_BG));
	stat->net_io_bytes = sizeof(cl_msg); // size of original synchronous fin
	stat->socket_timeout = CF_SOCKET_TIMEOUT;

	udf_bg_scan_job* job = (udf_bg_scan_job*)_job;
	char* extra = stat->jdata + strlen(stat->jdata);

	sprintf(extra, ":udf-filename=%s:udf-function=%s:udf-active=%u",
			job->origin.def.filename, job->origin.def.function,
			job->n_active_tr);
}

//----------------------------------------------------------
// udf_bg_scan_job utilities.
//

void
udf_bg_scan_job_reduce_cb(as_index_ref* r_ref, void* udata)
{
	as_scan_job* _job = (as_scan_job*)udata;
	udf_bg_scan_job* job = (udf_bg_scan_job*)_job;
	as_namespace* ns = _job->ns;

	if (_job->abandoned != 0) {
		as_record_done(r_ref, ns);
		return;
	}

	as_index* r = r_ref->r;

	if (excluded_set(r, _job->set_id) || as_record_is_doomed(r, ns)) {
		as_record_done(r_ref, ns);
		return;
	}

	predexp_args_t predargs = { .ns = ns, .md = r };

	if (job->origin.predexp != NULL &&
			predexp_matches_metadata(job->origin.predexp, &predargs) ==
					PREDEXP_FALSE) {
		as_record_done(r_ref, ns);
		as_incr_uint64(&_job->n_filtered_meta);
		as_incr_uint64(&ns->n_udf_sub_udf_filtered_out);
		return;
	}

	// Save this before releasing record.
	cf_digest keyd = r->keyd;

	// Release record lock before throttling and enqueuing transaction.
	as_record_done(r_ref, ns);

	// Prefer not reaching target RPS to queue buildup and transaction timeouts.
	while (as_load_uint32(&job->n_active_tr) > MAX_ACTIVE_TRANSACTIONS) {
		usleep(1000);
	}

	throttle_sleep(_job);

	as_transaction tr;
	as_transaction_init_iudf(&tr, ns, &keyd, &job->origin);

	as_incr_uint32(&job->n_active_tr);
	as_service_enqueue_internal(&tr);
}

void
udf_bg_scan_tr_complete(void* udata, int result)
{
	as_scan_job* _job = (as_scan_job*)udata;
	udf_bg_scan_job* job = (udf_bg_scan_job*)_job;

	as_decr_uint32(&job->n_active_tr);

	switch (result) {
	case AS_OK:
		as_incr_uint64(&_job->n_succeeded);
		break;
	case AS_ERR_NOT_FOUND: // record deleted after generating tr
		break;
	case AS_ERR_FILTERED_OUT:
		as_incr_uint64(&_job->n_filtered_bins);
		break;
	default:
		as_incr_uint64(&_job->n_failed);
		break;
	}
}



//==============================================================================
// ops_bg_scan_job derived class implementation.
//

//----------------------------------------------------------
// ops_bg_scan_job typedefs and forward declarations.
//

typedef struct ops_bg_scan_job_s {
	// Base object must be first:
	as_scan_job		_base;

	// Derived class data:
	iops_origin		origin;
	uint32_t		n_active_tr;
} ops_bg_scan_job;

void ops_bg_scan_job_slice(as_scan_job* _job, as_partition_reservation* rsv);
void ops_bg_scan_job_finish(as_scan_job* _job);
void ops_bg_scan_job_destroy(as_scan_job* _job);
void ops_bg_scan_job_info(as_scan_job* _job, as_mon_jobstat* stat);

const as_scan_vtable ops_bg_scan_job_vtable = {
		ops_bg_scan_job_slice,
		ops_bg_scan_job_finish,
		ops_bg_scan_job_destroy,
		ops_bg_scan_job_info
};

uint8_t* ops_bg_validate_ops(const as_msg* m);
void ops_bg_scan_job_reduce_cb(as_index_ref* r_ref, void* udata);
void ops_bg_scan_tr_complete(void* udata, int result);

//----------------------------------------------------------
// ops_bg_scan_job public API.
//

int
ops_bg_scan_job_start(as_transaction* tr, as_namespace* ns, uint16_t set_id)
{
	ops_bg_scan_job* job = cf_malloc(sizeof(ops_bg_scan_job));
	as_scan_job* _job = (as_scan_job*)job;

	scan_options options = { .sample_pct = 100 };
	uint32_t rps = 0;

	if (! get_scan_options(tr, &options) || ! get_scan_rps(tr, &rps)) {
		cf_warning(AS_SCAN, "ops-bg scan job failed msg field processing");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	if (! validate_background_scan_rps(ns, &rps)) {
		cf_warning(AS_SCAN, "ops-bg scan job failed rps check");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	as_msg* om = &tr->msgp->msg;
	uint8_t* ops = ops_bg_validate_ops(om);

	if (ops == NULL) {
		cf_warning(AS_SCAN, "ops-bg scan job failed ops check");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	predexp_eval_t* predexp = NULL;

	if (! get_scan_predexp(tr, &predexp)) {
		cf_warning(AS_SCAN, "ops-bg scan job failed predexp processing");
		cf_free(job);
		return AS_ERR_PARAMETER;
	}

	as_scan_job_init(_job, &ops_bg_scan_job_vtable, as_transaction_trid(tr), ns,
			set_id, rps, tr->from.proto_fd_h->client);

	job->n_active_tr = 0;

	uint8_t info2 = AS_MSG_INFO2_WRITE |
			(om->info2 & AS_MSG_INFO2_DURABLE_DELETE);
	uint8_t info3 = AS_MSG_INFO3_UPDATE_ONLY |
			(om->info3 & AS_MSG_INFO3_REPLACE_ONLY);

	job->origin.msgp = as_msg_create_internal(ns->name, 0, info2, info3,
			om->n_ops, ops, tr->msgp->proto.sz - (ops - (uint8_t*)om));

	job->origin.predexp = predexp;
	job->origin.cb = ops_bg_scan_tr_complete;
	job->origin.udata = (void*)job;

	cf_info(AS_SCAN, "starting ops-bg scan job %lu {%s:%s} rps %u from %s",
			_job->trid, ns->name, as_namespace_get_set_name(ns, set_id),
			_job->rps, _job->client);

	int result = as_scan_manager_start_job(_job);

	if (result != 0) {
		cf_warning(AS_SCAN, "ops-bg scan job %lu failed to start (%d)",
				_job->trid, result);
		as_scan_job_destroy(_job);
		return result;
	}

	if (as_msg_send_fin(&tr->from.proto_fd_h->sock, AS_OK)) {
		tr->from.proto_fd_h->last_used = cf_getns();
		as_end_of_transaction_ok(tr->from.proto_fd_h);
	}
	else {
		cf_warning(AS_SCAN, "ops-bg scan job error sending fin");
		as_end_of_transaction_force_close(tr->from.proto_fd_h);
		// No point returning an error - it can't be reported on this socket.
	}

	tr->from.proto_fd_h = NULL;

	return AS_OK;
}

//----------------------------------------------------------
// ops_bg_scan_job mandatory scan_job interface.
//

void
ops_bg_scan_job_slice(as_scan_job* _job, as_partition_reservation* rsv)
{
	as_index_reduce_live(rsv->tree, ops_bg_scan_job_reduce_cb, (void*)_job);
}

void
ops_bg_scan_job_finish(as_scan_job* _job)
{
	ops_bg_scan_job* job = (ops_bg_scan_job*)_job;

	while (job->n_active_tr != 0) {
		usleep(100);
	}

	switch (_job->abandoned) {
	case 0:
		as_incr_uint64(&_job->ns->n_scan_ops_bg_complete);
		break;
	case AS_SCAN_ERR_USER_ABORT:
		as_incr_uint64(&_job->ns->n_scan_ops_bg_abort);
		break;
	case AS_SCAN_ERR_UNKNOWN:
	case AS_SCAN_ERR_CLUSTER_KEY:
	default:
		as_incr_uint64(&_job->ns->n_scan_ops_bg_error);
		break;
	}

	cf_info(AS_SCAN, "finished ops-bg scan job %lu (%d)", _job->trid,
			_job->abandoned);
}

void
ops_bg_scan_job_destroy(as_scan_job* _job)
{
	ops_bg_scan_job* job = (ops_bg_scan_job*)_job;

	iops_origin_destroy(&job->origin);
}

void
ops_bg_scan_job_info(as_scan_job* _job, as_mon_jobstat* stat)
{
	strcpy(stat->job_type, scan_type_str(SCAN_TYPE_OPS_BG));
	stat->net_io_bytes = sizeof(cl_msg); // size of original synchronous fin
	stat->socket_timeout = CF_SOCKET_TIMEOUT;

	ops_bg_scan_job* job = (ops_bg_scan_job*)_job;
	char* extra = stat->jdata + strlen(stat->jdata);

	sprintf(extra, ":ops-active=%u", job->n_active_tr);
}

//----------------------------------------------------------
// ops_bg_scan_job utilities.
//

uint8_t*
ops_bg_validate_ops(const as_msg* m)
{
	if ((m->info1 & AS_MSG_INFO1_READ) != 0) {
		cf_warning(AS_SCAN, "ops not write only");
		return NULL;
	}

	if (m->n_ops == 0) {
		cf_warning(AS_SCAN, "ops scan has no ops");
		return NULL;
	}

	// TODO - should we at least de-fuzz the ops, so all the sub-transactions
	// won't fail later?
	int i = 0;

	return (uint8_t*)as_msg_op_iterate(m, NULL, &i);
}

void
ops_bg_scan_job_reduce_cb(as_index_ref* r_ref, void* udata)
{
	as_scan_job* _job = (as_scan_job*)udata;
	ops_bg_scan_job* job = (ops_bg_scan_job*)_job;
	as_namespace* ns = _job->ns;

	if (_job->abandoned != 0) {
		as_record_done(r_ref, ns);
		return;
	}

	as_index* r = r_ref->r;

	if (excluded_set(r, _job->set_id) || as_record_is_doomed(r, ns)) {
		as_record_done(r_ref, ns);
		return;
	}

	predexp_args_t predargs = { .ns = ns, .md = r };

	if (job->origin.predexp != NULL &&
			predexp_matches_metadata(job->origin.predexp, &predargs) ==
					PREDEXP_FALSE) {
		as_record_done(r_ref, ns);
		as_incr_uint64(&_job->n_filtered_meta);
		as_incr_uint64(&ns->n_ops_sub_write_filtered_out);
		return;
	}

	// Save this before releasing record.
	cf_digest keyd = r->keyd;

	// Release record lock before throttling and enqueuing transaction.
	as_record_done(r_ref, ns);

	// Prefer not reaching target RPS to queue buildup and transaction timeouts.
	while (as_load_uint32(&job->n_active_tr) > MAX_ACTIVE_TRANSACTIONS) {
		usleep(1000);
	}

	throttle_sleep(_job);

	as_transaction tr;
	as_transaction_init_iops(&tr, ns, &keyd, &job->origin);

	as_incr_uint32(&job->n_active_tr);
	as_service_enqueue_internal(&tr);
}

void
ops_bg_scan_tr_complete(void* udata, int result)
{
	as_scan_job* _job = (as_scan_job*)udata;
	ops_bg_scan_job* job = (ops_bg_scan_job*)_job;

	as_decr_uint32(&job->n_active_tr);

	switch (result) {
	case AS_OK:
		as_incr_uint64(&_job->n_succeeded);
		break;
	case AS_ERR_NOT_FOUND: // record deleted after generating tr
		break;
	case AS_ERR_FILTERED_OUT:
		as_incr_uint64(&_job->n_filtered_bins);
		break;
	default:
		as_incr_uint64(&_job->n_failed);
		break;
	}
}
