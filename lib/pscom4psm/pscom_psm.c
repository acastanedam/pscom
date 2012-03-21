/*
 * ParaStation
 *
 * Copyright (C) 2011 ParTec Cluster Competence Center GmbH, Munich
 *
 * This file may be distributed under the terms of the Q Public License
 * as defined in the file LICENSE.QPL included in the packaging of this
 * file.
 *
 * Author: Thomas Moschny <moschny@par-tec.com>
 */
/**
 * pscom_psm.c: PSM communication
 */

#include "pscom_psm.h"


pscom_plugin_t pscom_plugin = {
	.name		= "psm",
	.version	= PSCOM_PLUGIN_VERSION,
	.arch_id	= PSCOM_ARCH_PSM,
	.priority	= PSCOM_PSM_PRIO,
	.init		= pscom_psm_init,
	.destroy	= pscom_psm_finalize,
	.sock_init	= NULL,
	.sock_destroy	= NULL,
	.con_connect	= pscom_psm_connect,
	.con_accept	= pscom_psm_accept,
};


/*
 * use 48 bits for the peer id
 * and 16 bits for other information
 */
static const uint64_t mask = (UINTMAX_C(1) << 48) - 1;
static const uint64_t PSPSM_MAGIC_IO = UINTMAX_C(1) << 48;
static const uint64_t PSPSM_MAGIC_EOF = UINTMAX_C(2) << 48;

int pspsm_debug = 2;
FILE *pspsm_debug_stream = NULL;

/*
 * For now, psm allows only one endpoint per process, so we can safely
 * use a global variable.
 */
static char *pspsm_err_str = NULL; /* last error string */
static char* sendbuf = NULL;
static pspsm_uuid_t pspsm_uuid;
static psm_epid_t pspsm_epid;
static psm_ep_t pspsm_ep;
static psm_mq_t pspsm_mq;

static pspsm_poll_t pspsm_poll;


static
void poll_user_inc(void)
{
	if (!pspsm_poll.poll_user) {
		/* enqueue to polling reader */
		list_add_tail(&pspsm_poll.poll.next, &pscom.poll_reader);
	}
	pspsm_poll.poll_user++;
}


static
void poll_user_dec(void)
{
	pspsm_poll.poll_user--;
	if (!pspsm_poll.poll_user) {
		/* dequeue from polling reader */
		list_del_init(&pspsm_poll.poll.next);
	}
}


static
void pscom_psm_read_start(pscom_con_t *con)
{
	if (!con->arch.psm.reading) {
		con->arch.psm.reading = 1;
		poll_user_inc();
	}
	/* post a receive */
	pscom_psm_do_read(con);
}


static
void pscom_psm_read_stop(pscom_con_t *con)
{
	if (con->arch.psm.reading) {
		con->arch.psm.reading = 0;
		poll_user_dec();
	}
}


/* Process a mq_status. return 1, if a read made progress. 0 else */
static
int pscom_psm_process(psm_mq_status_t *status)
{
	uintptr_t c = (uintptr_t)status->context & 7;
	pspsm_con_info_t *ci = (pspsm_con_info_t *)((uintptr_t)status->context & ~(uintptr_t)7);
	pscom_con_t *con = ci->con;

	assert(ci->magic == UINTMAX_C(0xdeadbeefcafebabe));

	switch (c) {
	case 0:
		/* first send request */
		assert(ci->sreqs[0] != PSM_MQ_REQINVALID);
		poll_user_dec();
		ci->sreqs[0] = PSM_MQ_REQINVALID;
		/* pspsm_dprint(0, "Send0 done %p len %d con %s\n", ci->iov[0].iov_base,
		   (int)ci->iov[0].iov_len, ci->con->pub.remote_con_info.name); */
		if (ci->sreqs[1] == PSM_MQ_REQINVALID){
			pscom_write_done(con, ci->req, ci->iov[0].iov_len + ci->iov[1].iov_len);
			ci->req = NULL;
		}
		break;
	case 1:
		/* second send request */
		assert(ci->sreqs[1] != PSM_MQ_REQINVALID);
		poll_user_dec();
		ci->sreqs[1] = PSM_MQ_REQINVALID;
		/* pspsm_dprint(0, "Send1 done %p len %d con %s\n", ci->iov[1].iov_base,
		   (int)ci->iov[1].iov_len, ci->con->pub.remote_con_info.name); */
		if (ci->sreqs[0] == PSM_MQ_REQINVALID){
			pscom_write_done(con, ci->req, ci->iov[0].iov_len + ci->iov[1].iov_len);
			ci->req = NULL;
		}
		break;
	case 2:
		/* receive request */
		assert(ci->rbuf);
		assert(status->msg_length == status->nbytes);
		ci->rreq = PSM_MQ_REQINVALID;
		/* pspsm_dprint(0, "read done %p len %d con %s\n", ci->rbuf,
		   (int)status->msg_length, ci->con->pub.remote_con_info.name); */
		pscom_read_done_unlock(con, ci->rbuf, status->msg_length);
		ci->rbuf = NULL;
		if (con->arch.psm.reading) {
			/* There is more to read. Post the next receive request */
			pscom_psm_do_read(con);
		}
		return 1;
		break;
	default:
		/* this shouldn't happen */
		assert(0);
	}
	return 0;
}

static
int pscom_psm_peek()
{
	unsigned read_progress = 0;
	psm_mq_req_t req;
	psm_mq_status_t status;
	psm_error_t ret;
	do {
		ret = psm_mq_ipeek(pspsm_mq, &req, /* status */ NULL);
		if (ret == PSM_MQ_INCOMPLETE)
			return read_progress;
		if (ret != PSM_OK)
			goto err;
		ret = psm_mq_test(&req, &status);
		if (ret != PSM_OK)
			goto err;
		read_progress += pscom_psm_process(&status);
	}
	while (1);

 err:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "pscom_psm_peek: %s", pspsm_err_str);
	return read_progress;

}


static
int pscom_psm_make_progress(pscom_poll_reader_t *reader)
{
	return pscom_psm_peek();
}


static
int pscom_psm_do_read(pscom_con_t *con)
{
	pspsm_con_info_t *ci = con->arch.psm.ci;

	/* old request outstanding? */
	if (ci->rbuf) return 0;

	/* post a new request */
	pscom_read_get_buf_locked(con, &ci->rbuf, &ci->rbuflen);
	int ret = pspsm_recvlook(ci);

	if (ret == -EPIPE) goto err;
	assert(ret == -EAGAIN);
	return 0;

 err:
	errno = -ret;
	pscom_con_error(con, PSCOM_OP_READ, PSCOM_ERR_STDERROR);
	return 1;
}


static
void pscom_psm_do_write(pscom_con_t *con)
{
	pspsm_con_info_t *ci = con->arch.psm.ci;

	if (ci->req) {
		/* send in progress. wait for completion before
		   transmiting the next message. */
		return;
	}

	/* FIXME: we might want to send more than one message at a
	   time. */

	/* get and post a new write request */
	pscom_req_t *req = pscom_write_get_iov(con, ci->iov);
	if (req) {
		int ret = pspsm_sendv(ci);
		if (ret == 0){
			/* was a direct send */
			size_t size = ci->iov[0].iov_len + ci->iov[1].iov_len;
			pscom_write_done(con, req, size);
		}
		else if (ret == -EAGAIN){
			/* pspsm_sendv was successful */
			ci->req = req;
		}
		else if (ret == -EPIPE){
			errno = -ret;
			pscom_con_error(con, PSCOM_OP_WRITE, PSCOM_ERR_STDERROR);
		}
	}
}


static
void pscom_psm_close(pscom_con_t *con)
{
	pspsm_con_info_t *ci = con->arch.psm.ci;

	if (!ci) return;

	pspsm_send_eof(ci);

	pspsm_con_cleanup(ci);
	pspsm_con_free(ci);

	con->arch.psm.ci = NULL;
}


static
void pscom_psm_con_init(pscom_con_t *con, int con_fd,
			pspsm_con_info_t *ci)
{
	con->pub.state = PSCOM_CON_STATE_RW;
	con->pub.type = PSCOM_CON_TYPE_PSM;

	close(con_fd);

	con->arch.psm.ci = ci;
	con->arch.psm.reading = 0;

	con->write_start = pscom_poll_write_start;
	con->write_stop = pscom_poll_write_stop;
	con->read_start = pscom_psm_read_start;
	con->read_stop = pscom_psm_read_stop;

	con->do_write = pscom_psm_do_write;
	con->close = pscom_psm_close;

	ci->con = con;
}


static
void pscom_psm_init(void)
{
	pspsm_debug = pscom.env.debug;
	pspsm_debug_stream = pscom_debug_stream();

	/* see comment in pspsm_init() */
	pscom_env_get_uint(&pscom.env.psm_uniq_id, ENV_PSM_UNIQ_ID);
	if (!pscom.env.psm_uniq_id) {
		pscom_env_get_uint(&pscom.env.psm_uniq_id, ENV_PMI_ID);
	}

	INIT_LIST_HEAD(&pspsm_poll.poll.next);
	pspsm_poll.poll.do_read = pscom_psm_make_progress;
}


static
int pscom_psm_connect(pscom_con_t *con, int con_fd)
{
	int arch = PSCOM_ARCH_PSM;
	pspsm_con_info_t *ci = pspsm_con_create();
	pspsm_info_msg_t msg;
	pspsm_info_msg_t my_msg;

	if (pspsm_init() || !ci) goto dont_use;
	if (pspsm_con_init(ci)) goto dont_use;

	ci->con = con;

	/* We want talk psm */
	pscom_writeall(con_fd, &arch, sizeof(arch));

	/* step 1 */
	if ((pscom_readall(con_fd, &arch, sizeof(arch)) != sizeof(arch)) ||
	    (arch != PSCOM_ARCH_PSM)) {
		goto err_remote;
	}

	/* step 2 : recv connection id's */
	if ((pscom_readall(con_fd, &msg, sizeof(msg)) != sizeof(msg))) {
		goto err_remote;
	}

	/* step 3: send my connection id's */
	pspsm_con_get_info_msg(ci, &my_msg);
	pscom_writeall(con_fd, &my_msg, sizeof(my_msg));

	/* Connect */
	if (pspsm_con_connect(ci, &msg)) {
		/* ToDo: bad! How to inform the peer about the error? */
		DPRINT(0, "Psm pspsm_con_connect() failed!");
		goto err_local;
	}

	pscom_psm_con_init(con, con_fd, ci);

	return 1;
	/* --- */
 err_local:
 err_remote:
 dont_use:
	if (ci) {
		pspsm_con_cleanup(ci);
		pspsm_con_free(ci);
	}
	return 0;
}


static
int pscom_psm_accept(pscom_con_t *con, int con_fd)
{
	int arch = PSCOM_ARCH_PSM;
	pspsm_con_info_t *ci = pspsm_con_create();
	pspsm_info_msg_t msg;

	if (pspsm_init() || !ci) goto out_nopsm;
	if (pspsm_con_init(ci)) goto dont_use;

	/* step 1:  Yes, we talk psm. */
	pscom_writeall(con_fd, &arch, sizeof(arch));

	/* step 2: Send Connection id's */
	pspsm_con_get_info_msg(ci, &msg);
	pscom_writeall(con_fd, &msg, sizeof(msg));

	/* step 3 : recv connection id's */
	if ((pscom_readall(con_fd, &msg, sizeof(msg)) != sizeof(msg))) {
		goto err_remote;
	}

	/* Connect */
	if (pspsm_con_connect(ci, &msg)) {
		/* ToDo: bad! How to inform the peer about the error? */
		DPRINT(0, "Psm pspsm_con_connect() failed!");
		goto err_local;
	}

	pscom_psm_con_init(con, con_fd, ci);

	return 1;
	/* --- */
 err_local:
 err_remote:
	if (ci) pspsm_con_cleanup(ci);
 dont_use:
	if (ci) pspsm_con_free(ci);
	return 0;
	/* --- */
 out_nopsm:
	arch = PSCOM_ARCH_ERROR;
	pscom_writeall(con_fd, &arch, sizeof(arch));
	return 0;
	/* --- */
}


static
void pspsm_err(const char *str)
{
	if (pspsm_err_str) free(pspsm_err_str);

	if (str) {
		pspsm_err_str = strdup(str);
	} else {
		pspsm_err_str = strdup("");
	}
	return;
}

static
int pspsm_open_endpoint(void)
{
	psm_error_t ret;

	if (!pspsm_ep){
		struct psm_ep_open_opts opts;

		ret = psm_ep_open_opts_get_defaults(&opts);
		if (ret != PSM_OK) goto err;

		ret = psm_ep_open(pspsm_uuid.as_uuid, &opts,
				  &pspsm_ep, &pspsm_epid);
		if (ret != PSM_OK) goto err;

		sendbuf = malloc(pscom.env.readahead);

		pspsm_dprint(2, "pspsm_open_endpoint: OK");
	}
	return 0;

 err:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "pspsm_open_endpoint: %s", pspsm_err_str);
	return -1;
}


static
int pspsm_init_mq(void)
{
	psm_error_t ret;

	if (!pspsm_mq){
		ret = psm_mq_init(pspsm_ep, PSM_MQ_ORDERMASK_ALL, NULL, 0,
				  &pspsm_mq);

		if (ret != PSM_OK) goto err;
		pspsm_dprint(2, "pspsm_init_mq: OK");
	}
	return 0;

 err:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "pspsm_init_mq: %s", pspsm_err_str);
	return -1;
}


static
void pscom_psm_finalize(void){
	if (pspsm_close_endpoint() == -1) goto err;
	if (pspsm_finalize_mq() == -1) goto err;

 err:
	pspsm_dprint(1, "pspsm_psm_finalize not successful");
}


static
int pspsm_close_endpoint(void)
{
	psm_error_t ret;

	if (pspsm_ep){
		ret = psm_ep_close(pspsm_ep, PSM_EP_CLOSE_GRACEFUL, 0);
		if (ret != PSM_OK) goto err;

		if (sendbuf) free(sendbuf);

		pspsm_dprint(2, "pspsm_close_endpoint: OK");
	}
	return 0;

 err:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "pspsm_close_endpoint: %s", pspsm_err_str);
	return -1;
}


int pspsm_finalize_mq(void)
{
	psm_error_t ret;

	if (pspsm_mq){
		ret = psm_mq_finalize(pspsm_mq);
		if (ret != PSM_OK) goto err;
		pspsm_dprint(2, "pspsm_finalize_mq: OK");
	}
	return 0;

 err:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "pspsm_finalize_mq: %s", pspsm_err_str);
	return -1;
}


static
int pspsm_con_init(pspsm_con_info_t *con_info)
{
	static uint64_t id = 42;

	if (pspsm_open_endpoint()) goto err_ep;
	if (pspsm_init_mq()) goto err_mq;

	con_info->con_broken = 0;
	con_info->recv_id = id++;
	con_info->rbuf = NULL;
	con_info->req = NULL;

	con_info->rreq = PSM_MQ_REQINVALID;
	con_info->sreqs[0] = PSM_MQ_REQINVALID;
	con_info->sreqs[1] = PSM_MQ_REQINVALID;

	/* debug */
	con_info->magic = UINTMAX_C(0xdeadbeefcafebabe);

	pspsm_dprint(2, "pspsm_con_init: OK");
	return 0;

 err_mq:
	pspsm_close_endpoint();
 err_ep:
	pspsm_dprint(1, "pspsm_con_init: NOT OK");
	return -1;
}


static
int pspsm_con_connect(pspsm_con_info_t *con_info, pspsm_info_msg_t *info_msg)
{
	psm_error_t ret, ret1;

	ret = psm_ep_connect(pspsm_ep, 1, &info_msg->epid, NULL, &ret1,
			     &con_info->epaddr, 0);
	con_info->send_id = info_msg->id;

	if (ret != PSM_OK) goto err_connect;
	pspsm_dprint(2, "pspsm_con_connect: OK");
	pspsm_dprint(2, "sending with %"PRIx64", receiving %"PRIx64,
		     con_info->send_id, con_info->recv_id);
	return 0;

 err_connect:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "pspsm_con_connect: %s", pspsm_err_str);
	return -1;
}


static
int pspsm_init(void)
{
	static pspsm_init_state_t init_state = PSPSM_INIT_START;
	int verno_minor = PSM_VERNO_MINOR;
	int verno_major = PSM_VERNO_MAJOR;
	psm_error_t ret;

	if (init_state == PSPSM_INIT_START) {
		ret = psm_init(&verno_major, &verno_minor);
		if (ret != PSM_OK) {
			init_state = PSPSM_INIT_DISABLED;
			goto err_init;
		}

		/*
		 * All processes wanting to communicate need to use
		 * the same UUID.
		 *
		 * It is unclear whether there are drawbacks from
		 * simply using the same UUID for groups of processes
		 * that will never communicate.
		 *
		 * On top of a constant fill pattern, we use:
		 *
		 * - PSP_PSM_UNIQ_ID if set and not zero, or
		 * - PMI_ID, if set and not zero - that's not entirely
		 *   clean, but a practical solution for MPI apps (as
		 *   long as we do not implement communication between
		 *   two sets of MPI processes not sharing a
		 *   communicator).
		 */
		memset(pspsm_uuid.as_uuid, DEFAULT_UUID_PATTERN,
		       sizeof(pspsm_uuid.as_uuid));

		if (pscom.env.psm_uniq_id) {
			pspsm_dprint(2, "seeding PSM UUID with %u", pscom.env.psm_uniq_id);
			pspsm_uuid.as_uint = pscom.env.psm_uniq_id;
		}
		pspsm_dprint(2, "pspsm_init: OK");
	}
	return 0;

 err_init:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "pspsm_init: %s", pspsm_err_str);
	return -1;
}


#if 0
static
void pspsm_iov_print(const struct iovec *iov, size_t len)
{
	while (len > 0) {
		if (iov->iov_len) {
			pspsm_dprint(2, "SENDV %p %zu", iov->iov_base, iov->iov_len);
			len -= iov->iov_len;
		}
		iov++;
	}
}
#endif


static inline
int _pspsm_send_buf(pspsm_con_info_t *con_info, char *buf, size_t len,
		    uint64_t tag, psm_mq_req_t *req, unsigned long nr)
{
	void *context = (void *)((uintptr_t)con_info | nr);
	psm_error_t ret;
	assert(*req == PSM_MQ_REQINVALID);
	ret = psm_mq_isend(pspsm_mq, con_info->epaddr,
			   /* flags */ 0, tag, buf, len,
			   context, req);
	if (ret != PSM_OK) goto err;
	return 0;

 err:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "_pspsm_send_buf: %s", pspsm_err_str);
	return -EPIPE;
}


/* sends an iov. FIXME: returns 0 if the send is complete, -EAGAIN if
   it created one or more requests for it, and -EPIPE in case of an
   error. */
static
int _pspsm_sendv(pspsm_con_info_t *con_info, uint64_t magic)
{
	uint64_t tag = con_info->send_id | magic;
	unsigned int i=0;
	psm_error_t ret;
	size_t len = con_info->iov[0].iov_len + con_info->iov[1].iov_len;

	if (len <= pscom.env.readahead){
		pscom_memcpy_from_iov(sendbuf, con_info->iov, len);
		/* we hope that doesn't block - it shouldn't, as the
		 * message is sufficiently small */
		ret = psm_mq_send(pspsm_mq, con_info->epaddr,
				  /* flags*/ 0, tag, sendbuf, len);
		if (ret != PSM_OK) goto err;
		return 0;
	}

	for (i=0; i<2; i++){
		if (con_info->iov[i].iov_len){
			/* pspsm_dprint(0, "Send part[%d], %p len %d to con %s\n", i,
			   con_info->iov[i].iov_base, (int)con_info->iov[i].iov_len,
			   con_info->con->pub.remote_con_info.name); */
			if (_pspsm_send_buf(con_info, con_info->iov[i].iov_base,
					    con_info->iov[i].iov_len,
					    tag, &con_info->sreqs[i], i)){
				return -EPIPE;
			}
			/* inc for each outstanding send request */
			poll_user_inc();
		}
	}
	return -EAGAIN;

 err:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "_pspsm_send_buf: %s", pspsm_err_str);
	return -EPIPE;
}


static
int pspsm_sendv(pspsm_con_info_t *con_info)
{
	return _pspsm_sendv(con_info, PSPSM_MAGIC_IO);
}


static
void pspsm_send_eof(pspsm_con_info_t *con_info)
{
	psm_mq_req_t req = PSM_MQ_REQINVALID;
	char dummy;

	_pspsm_send_buf(con_info, &dummy, 0, con_info->send_id | PSPSM_MAGIC_EOF, &req, 3);
	psm_mq_wait(&req, NULL);
	con_info->con_broken = 1; /* stop sending */
}


static
int pspsm_recvlook(pspsm_con_info_t *con_info)
{
	/* ToDo: rename me to something like "post a receive". */
	psm_error_t ret;
	uint64_t rtag = con_info->recv_id;
	void *context = (void *)((uintptr_t)con_info | 2);

	assert(con_info->rreq == PSM_MQ_REQINVALID);
	ret = psm_mq_irecv(pspsm_mq, rtag, mask, 0 /*flags*/,
			   con_info->rbuf, con_info->rbuflen,
			   context, &con_info->rreq);
	if (ret != PSM_OK) goto out_err;

	/* FIXME: Should probably not return an error code to indicate
	   success. */
	return -EAGAIN;

 out_err:
	pspsm_err(psm_error_get_string(ret));
	pspsm_dprint(1, "pspsm_recvlook: %s", pspsm_err_str);
	return -1;
}


static
pspsm_con_info_t *pspsm_con_create(void)
{
	pspsm_con_info_t *con_info = memalign(8, sizeof(*con_info));
	return con_info;
}


static
void pspsm_con_free(pspsm_con_info_t *con_info)
{
	free(con_info);
}


static
void pspsm_con_cleanup(pspsm_con_info_t *con_info)
{
	/* FIXME: implement */
}


static
void pspsm_con_get_info_msg(pspsm_con_info_t *con_info,
			    pspsm_info_msg_t *info_msg)
{
	info_msg->epid = pspsm_epid;
	info_msg->id = con_info->recv_id;
}
