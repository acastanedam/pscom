/*
 * ParaStation
 *
 * Copyright (C) 2008 ParTec Cluster Competence Center GmbH, Munich
 *
 * This file may be distributed under the terms of the Q Public License
 * as defined in the file LICENSE.QPL included in the packaging of this
 * file.
 *
 * Author:	Jens Hauke <hauke@par-tec.com>
 */

#include "pscom_queues.h"


static inline
int req_recv_user_accept(pscom_req_t *req, pscom_connection_t *connection, pscom_header_net_t *header)
{
	int (*recv_accept)(pscom_request_t *request,
			   pscom_connection_t *connection,
			   pscom_header_net_t *header_net);

	D_TR(printf("%s(req:%p)\n", __func__, req));

	recv_accept = req->pub.ops.recv_accept;

	return !recv_accept || recv_accept(&req->pub, connection, header);
}


static inline
int req_recv_ctrl_accept(pscom_req_t *req, pscom_connection_t *connection, pscom_header_net_t *header)
{
	int (*recv_accept)(pscom_request_t *request,
			   pscom_connection_t *connection,
			   pscom_header_net_t *header_net);
	D_TR(printf("%s(req:%p)\n", __func__, req));

	if (req->pub.header.msg_type != header->msg_type) {
		return 0;
	}

	recv_accept = req->pub.ops.recv_accept;

	return !recv_accept || recv_accept(&req->pub, connection, header);
}


/*************
 * Sendq
 */


void _pscom_sendq_enq(pscom_con_t *con, pscom_req_t *req)
{
	D_TR(printf("%s(req:%p)\n", __func__, req));

	req->pub.state = PSCOM_REQ_STATE_SEND_REQUEST | PSCOM_REQ_STATE_POSTED;

	list_add_tail(&req->next, &con->sendq);

	con->write_start(con);
}


void _pscom_sendq_deq(pscom_con_t *con, pscom_req_t *req)
{
	assert(!(req->cur_header.iov_len + req->cur_data.iov_len) || /* io done or */
	       !(req->pub.state & PSCOM_REQ_STATE_IO_STARTED));      /* io not started */

	list_del(&req->next); // dequeue
	if (list_empty(&con->sendq)) {
		con->write_stop(con);
	}
}


/*************
 * Receive requests
 */

void _pscom_recv_req_cnt_inc(pscom_con_t *con)
{
	int start = !con->recv_req_cnt;

	con->recv_req_cnt++;

	if (start) {
		/* First increment, than call read_start(con)!
		   read_start(con) could recursive call _pscom_recv_req_cnt_inc() */
		con->read_start(con);
	}
}


void _pscom_recv_req_cnt_dec(pscom_con_t *con)
{
	con->recv_req_cnt--;
}


void _pscom_recv_req_cnt_check_stop(pscom_con_t *con)
{
	if (!con->recv_req_cnt && !con->in.req && !pscom.env.unexpected_receives) {
		con->read_stop(con);
	}
}


void _pscom_recv_req_cnt_check_start(pscom_con_t *con)
{
	if (con->recv_req_cnt || con->in.req) {
		con->read_start(con);
	}
}


void _pscom_recv_req_cnt_any_inc(pscom_sock_t *sock)
{
	struct list_head *pos;

	if (!sock->recv_req_cnt_any++ && !pscom.env.unexpected_receives) {
		/* Loop only the first time and if not unexpected_receives is enabled */

		list_for_each(pos, &sock->connections) {
			pscom_con_t *con = list_entry(pos, pscom_con_t, next);
			_pscom_recv_req_cnt_inc(con);
		}
	}
}


void _pscom_recv_req_cnt_any_dec(pscom_sock_t *sock)
{
	struct list_head *pos;

	if (!--sock->recv_req_cnt_any && !pscom.env.unexpected_receives) {
		/* Loop only if recv_req_cnt_any is back zero and if
		   not unexpected_receives is enabled */

		list_for_each(pos, &sock->connections) {
			pscom_con_t *con = list_entry(pos, pscom_con_t, next);
			_pscom_recv_req_cnt_dec(con);
		}
	}
}


/*************
 * Recvq user
 */


static
void _pscom_recvq_user_enq_con(pscom_con_t *con, pscom_req_t *req)
{
	list_add_tail(&req->next, &con->recvq_user);
	_pscom_recv_req_cnt_inc(con);
}


static
void _pscom_recvq_user_reenq_con(pscom_con_t *con, pscom_req_t *req)
{
	list_del(&req->next); // probably from sock->recvq_any
	list_add_tail(&req->next, &con->recvq_user); // to con->recvq_user
	/* No need for:
	   _pscom_recv_req_cnt_dec(con);
	   _pscom_recv_req_cnt_inc(con);
	*/
}


static
void _pscom_recvq_user_deq_con(pscom_con_t *con, pscom_req_t *req)
{
	// req in sock->recvq_any or con->recvq_user
	list_del(&req->next);
	_pscom_recv_req_cnt_dec(get_con(req->pub.connection));
}



static
void _pscom_recvq_user_enq_any(pscom_sock_t *sock, pscom_req_t *req)
{
	list_add_tail(&req->next, &sock->recvq_any);
	pscom.stat.recvq_any++;

	if (req->pub.connection) {
		_pscom_recv_req_cnt_inc(get_con(req->pub.connection));
	} else {
		_pscom_recv_req_cnt_any_inc(sock);
		pscom.stat.reqs_any_source++;
	}
}


static
void _pscom_recvq_user_deq_any(pscom_sock_t *sock, pscom_req_t *req)
{
	// req in sock->recvq_any
	list_del(&req->next);
	_pscom_recv_req_cnt_any_dec(sock);
}


void _pscom_recvq_user_enq(pscom_req_t *req)
{
	pscom_sock_t *sock;

	D_TR(printf("%s(req:%p)\n", __func__, req));

	if (req->pub.connection) {
		req->pub.socket = req->pub.connection->socket;
	}

	sock = get_sock(req->pub.socket);

	if (list_empty(&sock->recvq_any) && req->pub.connection) {
		pscom_con_t *con = get_con(req->pub.connection);

		_pscom_recvq_user_enq_con(con, req);
	} else {
		_pscom_recvq_user_enq_any(sock, req);
	}
}


void _pscom_recvq_user_deq(pscom_req_t *req)
{
	if (req->pub.connection) {
		_pscom_recvq_user_deq_con(get_con(req->pub.connection), req);
	} else {
		_pscom_recvq_user_deq_any(get_sock(req->pub.socket), req);
	}
}


pscom_req_t *_pscom_recvq_user_find_and_deq(pscom_con_t *con, pscom_header_net_t *header)
{
	struct list_head *pos;
	pscom_sock_t *sock;

	list_for_each(pos, &con->recvq_user) {
		pscom_req_t *req = list_entry(pos, pscom_req_t, next);

		if (req_recv_user_accept(req, &con->pub, header)) {
			_pscom_recvq_user_deq_con(con, req);
			return req;
		}
	}

	sock = get_sock(con->pub.socket);

	list_for_each(pos, &sock->recvq_any) {
		pscom_req_t *req = list_entry(pos, pscom_req_t, next);
		if (((!req->pub.connection) || (req->pub.connection == &con->pub)) &&
		    req_recv_user_accept(req, &con->pub, header)) {
			_pscom_recvq_user_deq(req); // con or any request
			_pscom_recvq_any_cleanup(sock);
			return req;
		}
	}
	return NULL;
}


// for debug:
int _pscom_recvq_user_is_inside(pscom_req_t *req)
{
	struct list_head *pos;
	pscom_sock_t *sock;

	if (req->pub.connection) { // if req is not an ANY_SOURCE receive:
		pscom_con_t *con = get_con(req->pub.connection);

		assert(con->magic == MAGIC_CONNECTION);

		list_for_each(pos, &con->recvq_user) {
			pscom_req_t *qreq = list_entry(pos, pscom_req_t, next);

			if (qreq == req) return 1;
		}
	}

	sock = get_sock(req->pub.socket);

	assert(sock->magic == MAGIC_SOCKET);

	list_for_each(pos, &sock->recvq_any) {
		pscom_req_t *qreq = list_entry(pos, pscom_req_t, next);
		if (qreq == req) return 1;
	}

	return 0;
}


void _pscom_recvq_any_cleanup(pscom_sock_t *sock)
{
	struct list_head *pos, *next;

	list_for_each_safe(pos, next, &sock->recvq_any) {
		pscom_req_t *req = list_entry(pos, pscom_req_t, next);
		if (req->pub.connection) {
			/* Move request from any queue to con queue */
			pscom_con_t *con = get_con(req->pub.connection);
			_pscom_recvq_user_reenq_con(con, req);
		} else {
			break;
		}
	}
}

/*************
 * Recvq ctrl
 */

void _pscom_recvq_ctrl_enq(pscom_con_t *con, pscom_req_t *req)
{
	req->pub.connection = &con->pub;
	req->pub.socket = con->pub.socket;

	list_add_tail(&req->next, &con->recvq_ctrl);
	_pscom_recv_req_cnt_inc(con);
}


void _pscom_recvq_ctrl_deq(pscom_con_t *con, pscom_req_t *req)
{
	list_del(&req->next);
	_pscom_recv_req_cnt_dec(con);
}


pscom_req_t *_pscom_recvq_ctrl_find_and_deq(pscom_con_t *con, pscom_header_net_t *header)
{
	struct list_head *pos;

	list_for_each(pos, &con->recvq_ctrl) {
		pscom_req_t *req = list_entry(pos, pscom_req_t, next);

		if (req_recv_ctrl_accept(req, &con->pub, header)) {
			_pscom_recvq_ctrl_deq(con, req);
			return req;
		}
	}

	return NULL;
}


/*************
 * Net recvq user (network generated requests)
 */


static inline
pscom_req_t *_pscom_net_recvq_user_find_from_con(pscom_con_t *con, pscom_req_t *req)
{
	struct list_head *pos;

	D_TR(printf("%s(con:%p, req:%p)\n", __func__, con, req));

	list_for_each(pos, &con->net_recvq_user) {
		pscom_req_t *genreq = list_entry(pos, pscom_req_t, next);

		if (req_recv_user_accept(req, genreq->pub.connection, &genreq->pub.header)) {
			return genreq;
		}
	}
	return NULL;
}


static inline
pscom_req_t *_pscom_net_recvq_user_find_from_any(pscom_sock_t *sock, pscom_req_t *req)
{
	struct list_head *pos;
	D_TR(printf("%s(sock:%p, req:%p)\n", __func__, sock, req));

	list_for_each(pos, &sock->genrecvq_any) {
		pscom_req_t *genreq = list_entry(pos, pscom_req_t, next_alt);

		if (req_recv_user_accept(req, genreq->pub.connection, &genreq->pub.header)) {
			return genreq;
		}
	}
	return NULL;
}



void _pscom_net_recvq_user_enq(pscom_con_t *con, pscom_req_t *req)
{
	pscom_sock_t *sock = get_sock(con->pub.socket);

	list_add_tail(&req->next, &con->net_recvq_user);
	list_add_tail(&req->next_alt, &sock->genrecvq_any);
}


void _pscom_net_recvq_user_deq(pscom_req_t *req)
{
	list_del(&req->next);
	list_del(&req->next_alt);
}


/* find net generated user request. */
pscom_req_t *_pscom_net_recvq_user_find(pscom_req_t *req)
{
	if (req->pub.connection) {
		return _pscom_net_recvq_user_find_from_con(get_con(req->pub.connection), req);
	} else {
		// receive "any"
		return _pscom_net_recvq_user_find_from_any(get_sock(req->pub.socket), req);
	}
}


/*************
 * Net recvq ctrl (network generated requests)
 */


/* enqueue a network generated ctrl request */
void _pscom_net_recvq_ctrl_enq(pscom_con_t *con, pscom_req_t *req)
{
	list_add_tail(&req->next, &con->net_recvq_ctrl);
}


void _pscom_net_recvq_ctrl_deq(pscom_req_t *req)
{
	list_del(&req->next);
}


/* find req matching net generated ctrl request. */
pscom_req_t *_pscom_net_recvq_ctrl_find(pscom_req_t *req)
{
	struct list_head *pos;

	pscom_con_t *con = get_con(req->pub.connection);

	D_TR(printf("%s(con:%p, req:%p)\n", __func__, con, req));

	list_for_each(pos, &con->net_recvq_ctrl) {
		pscom_req_t *genreq = list_entry(pos, pscom_req_t, next);

		if (req_recv_ctrl_accept(req, genreq->pub.connection, &genreq->pub.header)) {
			return genreq;
		}
	}
	return NULL;
}


/*************
 * Recvq RMA
 */


void _pscom_recvq_rma_enq(pscom_con_t *con, pscom_req_t *req)
{
	list_add_tail(&req->next, &con->recvq_rma);
	_pscom_recv_req_cnt_inc(con);
}


void _pscom_recvq_rma_deq(pscom_con_t *con, pscom_req_t *req)
{
	list_del(&req->next);
	_pscom_recv_req_cnt_dec(con);
}


/*************
 * Recvq bcast
 */


//void _pscom_recvq_bcast_deq(pscom_req_t *req)
//{
//	// ToDo:
//	// list_del(&req->next); // dequeue
//}
