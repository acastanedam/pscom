/*
 * ParaStation
 *
 * Copyright (C) 2002-2004 ParTec AG, Karlsruhe
 * Copyright (C) 2005-2007 ParTec Cluster Competence Center GmbH, Munich
 *
 * This file may be distributed under the terms of the Q Public License
 * as defined in the file LICENSE.QPL included in the packaging of this
 * file.
 *
 * Author:	Jens Hauke <hauke@par-tec.com>
 */
/**
 * pscom_openib.c: OPENIB/Infiniband communication
 */

#include "psoib.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include <assert.h>

#include "pscom_priv.h"
#include "pscom_io.h"
#include "pscom_openib.h"


static
struct pscom_poll_reader pscom_cq_poll;

int pscom_poll_cq(pscom_poll_reader_t *reader)
{
	psoib_progress();

	if (!psoib_outstanding_cq_entries) {
		/* Stop polling on cq */
		/* it's save to dequeue more then once */
		list_del_init(&reader->next);
	}

	return 0;
}

static inline
void pscom_check_cq_poll(void)
{
	if (psoib_outstanding_cq_entries &&
	    list_empty(&pscom_cq_poll.next)) {
		// There are outstanding cq events and
		// we do not already poll the cq

		// Start polling:
		list_add_tail(&pscom_cq_poll.next, &pscom.poll_reader);
	}
}


static
int _pscom_openib_do_read(pscom_con_t *con, psoib_con_info_t *mcon)
{
	void *buf;
	int size;

	size = psoib_recvlook(mcon, &buf);

	if (size >= 0) {
		pscom_read_done(con, buf, size);

		psoib_recvdone(mcon);
		return 1;
	} else if ((size == -EINTR) || (size == -EAGAIN)) {
		// Nothing received
		return 0;
	} else {
		// Error
		errno = -size;
		pscom_con_error(con, PSCOM_OP_READ, PSCOM_ERR_STDERROR);
		return 1;
	}
}


static
int pscom_openib_do_read(pscom_poll_reader_t *reader)
{
	pscom_con_t *con = list_entry(reader, pscom_con_t, poll_reader);
	psoib_con_info_t *mcon = con->arch.openib.mcon;

	return _pscom_openib_do_read(con, mcon);
}


static
void pscom_openib_do_write(pscom_con_t *con)
{
	unsigned int len;
	struct iovec iov[2];
	pscom_req_t *req;

	req = pscom_write_get_iov(con, iov);

	if (req) {
		psoib_con_info_t *mcon = con->arch.openib.mcon;
		len = iov[0].iov_len + iov[1].iov_len;

		int rlen = psoib_sendv(mcon, iov, len);

		if (rlen >= 0) {
			pscom_write_done(con, req, rlen);
			pscom_check_cq_poll();
		} else if ((rlen == -EINTR) || (rlen == -EAGAIN)) {
			// Busy: Maybe out of tokens? try to read more tokens:
			_pscom_openib_do_read(con, mcon);
		} else {
			// Error
			pscom_con_error(con, PSCOM_OP_WRITE, PSCOM_ERR_STDERROR);
		}
	}
}


/*
 * ++ RMA rendezvous begin
 */
#ifdef IB_USE_RNDV

typedef struct pscom_rendezvous_data_openib {
	struct psoib_rma_req	rma_req;
	pscom_req_t		*rendezvous_req; // Receiving side: users receive request (or generated request)
} pscom_rendezvous_data_openib_t;


static inline
pscom_rendezvous_data_openib_t *get_req_data(pscom_rendezvous_data_t *rd)
{
	_pscom_rendezvous_data_openib_t *data = &rd->arch.openib;
	pscom_rendezvous_data_openib_t *res = (pscom_rendezvous_data_openib_t *) data;
	assert(sizeof(*res) <= sizeof(*data));
	return res;
}


static
unsigned int pscom_openib_rma_mem_register(pscom_con_t *con, pscom_rendezvous_data_t *rd)
{
	int err = 0;
	pscom_rendezvous_data_openib_t *openib_rd = get_req_data(rd);
	psoib_con_info_t *ci = con->arch.openib.mcon;
	psoib_rma_mreg_t *mreg = &openib_rd->rma_req.mreg;

#ifdef IB_RNDV_USE_PADDING

        rd->msg.arch.openib.padding_size = (IB_RNDV_PADDING_SIZE - ((long long int)rd->msg.data) % IB_RNDV_PADDING_SIZE) % IB_RNDV_PADDING_SIZE;

        memcpy(rd->msg.arch.openib.padding_data, rd->msg.data, rd->msg.arch.openib.padding_size);

        rd->msg.data += rd->msg.arch.openib.padding_size;
        rd->msg.data_len -= rd->msg.arch.openib.padding_size;

        /* get mem region */
        err = psoib_acquire_rma_mreg(mreg, rd->msg.data, rd->msg.data_len, ci);
        assert(!err);

        if (err) goto err_get_region;

        rd->msg.arch.openib.mr_key  = mreg->key;
        rd->msg.arch.openib.mr_addr = (uint64_t)mreg->mem_info.ptr;

        return sizeof(rd->msg.arch.openib) - IB_RNDV_PADDING_SIZE + rd->msg.arch.openib.padding_size;
#else

	/* get mem region */
	err = psoib_acquire_rma_mreg(mreg, rd->msg.data, rd->msg.data_len, ci);
	assert(!err);

	if (err) goto err_get_region;

	rd->msg.arch.openib.mr_key  = mreg->key;
	rd->msg.arch.openib.mr_addr = (uint64_t)mreg->mem_info.ptr;

	return sizeof(rd->msg.arch.openib);
#endif

err_get_region:
	// ToDo: Handle Errors!
	return 0;
}


static
void pscom_openib_rma_mem_deregister(pscom_con_t *con, pscom_rendezvous_data_t *rd)
{
	pscom_rendezvous_data_openib_t *openib_rd = get_req_data(rd);
	psoib_rma_mreg_t *mreg = &openib_rd->rma_req.mreg;

	psoib_release_rma_mreg(mreg);
}


static
void pscom_openib_rma_read_io_done(psoib_rma_req_t *dreq)
{
	pscom_rendezvous_data_openib_t *psopenib_rd =
		(pscom_rendezvous_data_openib_t *)dreq->priv;

	pscom_req_t *rendezvous_req = psopenib_rd->rendezvous_req;
	psoib_rma_mreg_t *mreg = &psopenib_rd->rma_req.mreg;

	psoib_release_rma_mreg(mreg);

	_pscom_recv_req_done(rendezvous_req);
}


static
int pscom_openib_rma_read(pscom_req_t *rendezvous_req, pscom_rendezvous_data_t *rd)
{
	int err, ret;
	pscom_rendezvous_data_openib_t *psopenib_rd = get_req_data(rd);
	psoib_rma_req_t *dreq = &psopenib_rd->rma_req;
	pscom_con_t *con = get_con(rendezvous_req->pub.connection);
	psoib_con_info_t *ci = con->arch.openib.mcon;

#ifdef IB_RNDV_USE_PADDING
        memcpy(rendezvous_req->pub.data, rd->msg.arch.openib.padding_data, rd->msg.arch.openib.padding_size);
        rendezvous_req->pub.data += rd->msg.arch.openib.padding_size;
#endif

	err = psoib_acquire_rma_mreg(&dreq->mreg, rendezvous_req->pub.data, rendezvous_req->pub.data_len, ci);
	assert(!err); // ToDo: Catch error

	dreq->remote_addr = rd->msg.arch.openib.mr_addr;
	dreq->remote_key  = rd->msg.arch.openib.mr_key;
	dreq->data_len = rendezvous_req->pub.data_len;
	dreq->ci = ci;
	dreq->io_done = pscom_openib_rma_read_io_done;
	dreq->priv = psopenib_rd;

	psopenib_rd->rendezvous_req = rendezvous_req;

	err = psoib_post_rma_get(dreq);
	assert(!err); // ToDo: Catch error

	return 0;
}
#endif
/*
 * -- RMA rendezvous end
 */


static
void pscom_openib_close(pscom_con_t *con)
{
	psoib_con_info_t *mcon = con->arch.openib.mcon;

	if (!mcon) return;

	psoib_send_eof(mcon);

	psoib_con_cleanup(mcon, NULL);
	psoib_con_free(mcon);

	con->arch.openib.mcon = NULL;
}


static
void pscom_openib_con_init(pscom_con_t *con, int con_fd,
			   psoib_con_info_t *mcon)
{
	con->pub.state = PSCOM_CON_STATE_RW;
	con->pub.type = PSCOM_CON_TYPE_OPENIB;

	close(con_fd);

	con->arch.openib.mcon = mcon;

	// Only Polling:
	con->write_start = pscom_poll_write_start;
	con->write_stop = pscom_poll_write_stop;
	con->read_start = pscom_poll_read_start;
	con->read_stop = pscom_poll_read_stop;

	con->poll_reader.do_read = pscom_openib_do_read;
	con->do_write = pscom_openib_do_write;
	con->close = pscom_openib_close;

#ifdef IB_USE_RNDV
	con->rma_mem_register = pscom_openib_rma_mem_register;
	con->rma_mem_deregister = pscom_openib_rma_mem_deregister;
	con->rma_read = pscom_openib_rma_read;

	con->rendezvous_size = IB_RNDV_THRESHOLD;
#endif
}

/*********************************************************************/
static
void pscom_openib_init(void)
{
	psoib_debug = pscom.env.debug;
	psoib_debug_stream = pscom_debug_stream();
	pscom_env_get_str(&psoib_hca, ENV_OPENIB_HCA);
	pscom_env_get_uint(&psoib_port, ENV_OPENIB_PORT);
	pscom_env_get_uint(&psoib_path_mtu, ENV_OPENIB_PATH_MTU);

	pscom_env_get_uint(&psoib_recvq_size, ENV_OPENIB_RECVQ_SIZE);

	pscom_env_get_int(&psoib_global_sendq, ENV_OPENIB_GLOBAL_SENDQ);
	pscom_env_get_uint(&psoib_compq_size, ENV_OPENIB_COMPQ_SIZE);
	if (psoib_global_sendq) {
		// One sendq for all connection. limit sendq to compq size.
		psoib_sendq_size = psoib_compq_size;
	} else {
		// One sendq for each connection. limit sendq to recvq size.
		psoib_sendq_size = pscom_min(psoib_sendq_size, psoib_recvq_size);
	}
	pscom_env_get_uint(&psoib_sendq_size, ENV_OPENIB_SENDQ_SIZE);

	psoib_pending_tokens = psoib_pending_tokens_suggestion();
	pscom_env_get_uint(&psoib_pending_tokens, ENV_OPENIB_PENDING_TOKENS);

//	if (!psoib_global_sendq && psoib_sendq_size == psoib_recvq_size) {
//		// Disable event counting:
//		psoib_event_count = 0;
//	}
	pscom_env_get_int(&psoib_event_count, ENV_OPENIB_EVENT_CNT);
	pscom_env_get_int(&psoib_ignore_wrong_opcodes, ENV_OPENIB_IGNORE_WRONG_OPCODES);
	pscom_env_get_int(&psoib_lid_offset, ENV_OPENIB_LID_OFFSET);

	INIT_LIST_HEAD(&pscom_cq_poll.next);
	pscom_cq_poll.do_read = pscom_poll_cq;

}


static
int pscom_openib_connect(pscom_con_t *con, int con_fd)
{
	int arch = PSCOM_ARCH_OPENIB;
	psoib_con_info_t *mcon = psoib_con_create();
	psoib_info_msg_t msg;
	int call_cleanup_con = 0;
	int err;

	if (psoib_init() || !mcon)
		goto dont_use;  /* Dont use openib */

	/* We want talk openib */
	pscom_writeall(con_fd, &arch, sizeof(arch));

	/* step 1 */
	if ((pscom_readall(con_fd, &arch, sizeof(arch)) != sizeof(arch)) ||
	    (arch != PSCOM_ARCH_OPENIB))
		goto err_remote;

	/* step 2 : recv connection id's */
	if ((pscom_readall(con_fd, &msg, sizeof(msg)) != sizeof(msg)))
		goto err_remote;

	err = psoib_con_init(mcon, NULL, NULL);
	if (!err) {
		call_cleanup_con = 1;
		err = psoib_con_connect(mcon, &msg);
	}

	/* step 3 : send connection id's (or error) */
	if (!err) {
		psoib_con_get_info_msg(mcon, &msg);
	} else {
		msg.lid = 0xffff; // send error
	}

	pscom_writeall(con_fd, &msg, sizeof(msg));

	if (err) goto err_connect;

	/* step 4: openib initialized. Recv final ACK. */
	if ((pscom_readall(con_fd, &msg, sizeof(msg)) != sizeof(msg)) ||
	    (msg.lid == 0xffff)) goto err_ack;

	pscom_openib_con_init(con, con_fd, mcon);

	return 1;
	/* --- */
err_ack:
err_connect:
	if (call_cleanup_con) psoib_con_cleanup(mcon, NULL);
err_remote:
dont_use:
	if (mcon) psoib_con_free(mcon);
	return 0;
}


static
int pscom_openib_accept(pscom_con_t *con, int con_fd)
{
	int arch = PSCOM_ARCH_OPENIB;
	psoib_con_info_t *mcon = NULL;
	psoib_info_msg_t msg;

	if (psoib_init())
		goto out_noopenib;

	mcon = psoib_con_create();
	if (!mcon)
		goto out_noopenib;

	if (psoib_con_init(mcon, NULL, NULL))
		goto err_con_init;

	/* step 1:  Yes, we talk openib. */
	pscom_writeall(con_fd, &arch, sizeof(arch));

	/* step 2: Send Connection id's */
	psoib_con_get_info_msg(mcon, &msg);

	pscom_writeall(con_fd, &msg, sizeof(msg));

	/* step 3 : recv connection id's */
	if ((pscom_readall(con_fd, &msg, sizeof(msg)) != sizeof(msg)) ||
	    (msg.lid == 0xffff))
		goto err_remote;

	if (psoib_con_connect(mcon, &msg))
		goto err_connect_con;

	/* step 4: OPENIB mem initialized. Send final ACK. */
	msg.lid = 0;
	pscom_writeall(con_fd, &msg, sizeof(msg));

	pscom_openib_con_init(con, con_fd, mcon);

	return 1;
	/* --- */
err_connect_con:
	/* Send NACK */
	msg.lid = 0xffff;
	pscom_writeall(con_fd, &msg, sizeof(msg));
err_remote:
	psoib_con_cleanup(mcon, NULL);
err_con_init:
out_noopenib:
	if (mcon) psoib_con_free(mcon);
	arch = PSCOM_ARCH_ERROR;
	pscom_writeall(con_fd, &arch, sizeof(arch));
	return 0; /* Dont use openib */
	/* --- */
}


pscom_plugin_t pscom_plugin = {
	.name		= "openib",
	.version	= PSCOM_PLUGIN_VERSION,
	.arch_id	= PSCOM_ARCH_OPENIB,
	.priority	= PSCOM_OPENIB_PRIO,

	.init		= pscom_openib_init,
	.destroy	= NULL,
	.sock_init	= NULL,
	.sock_destroy	= NULL,
	.con_connect	= pscom_openib_connect,
	.con_accept	= pscom_openib_accept,
};
