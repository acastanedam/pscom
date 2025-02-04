/*
 * ParaStation
 *
 * Copyright (C) 2020-2021 ParTec Cluster Competence Center GmbH, Munich
 * Copyright (C) 2021      ParTec AG, Munich
 *
 * This file may be distributed under the terms of the Q Public License
 * as defined in the file LICENSE.QPL included in the packaging of this
 * file.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>

#include "pscom_con.h"
#include "pscom_io.h"
#include "pscom_priv.h"
#include "pscom_queues.h"
#include "pscom_util.h"
#include "pscom_req.h"

#include "util/test_utils_con.h"

/* we need to access some static functions */
#include "pscom_io.c"

////////////////////////////////////////////////////////////////////////////////
/// pscom_post_recv()
////////////////////////////////////////////////////////////////////////////////

typedef enum { TESTCON_STATE_CLOSED=0, TESTCON_STATE_OPENED } connection_state_t;
typedef enum { TESTCON_OP_NOP=0, TESTCON_OP_STOP_READ, TESTCON_OP_START_READ } connection_action_t;

connection_state_t transition_table[2][3] = {
	{TESTCON_STATE_CLOSED, TESTCON_STATE_CLOSED, TESTCON_STATE_OPENED},
	{TESTCON_STATE_OPENED, TESTCON_STATE_CLOSED, TESTCON_STATE_OPENED}
};

static
int connection_state(connection_action_t action)
{
	static connection_state_t connection_state = TESTCON_STATE_CLOSED;
	connection_state_t old_state = connection_state;

	connection_state = transition_table[connection_state][action];

	return old_state;
}

static
void check_read_start_called(pscom_con_t *con)
{
	function_called();
	check_expected(con);

	connection_state(TESTCON_OP_START_READ);
}

static
void check_read_stop_called(pscom_con_t *con)
{
	function_called();
	check_expected(con);

	connection_state(TESTCON_OP_STOP_READ);
}

static
void init_gen_req(pscom_connection_t *connection, pscom_req_t *gen_req)
{
	gen_req->pub.connection = connection;
	gen_req->cur_data.iov_base = NULL;
	gen_req->partner_req = NULL;
	pscom_header_net_prepare(&gen_req->pub.header, PSCOM_MSGTYPE_USER, 0, 0);
}

/**
 * \brief Test pscom_post_recv() for partially received message
 *
 * Given: A partially received message in a generated request
 * When: pscom_post_recv() is called
 * Then: the request should be merged properly and the connection should be
 *       open for reading.
 */
void test_post_recv_partial_genreq(void **state)
{
	/* obtain the dummy connection from the test setup */
	pscom_con_t *recv_con =  (pscom_con_t*)(*state);

	/* create generated requests and enqueue to the list of net requests */
	pscom_req_t *gen_req = pscom_req_create(0, 100);
	init_gen_req(&recv_con->pub, gen_req);
	_pscom_net_recvq_user_enq(recv_con, gen_req);

	/* set the read_start()/read_stop() functions */
	recv_con->read_start = &check_read_start_called;
	recv_con->read_stop = &check_read_stop_called;

	/*
	 * set the appropriate request state:
	 * -> generated requests
	 * -> IO has been started
	 */
	gen_req->pub.state = PSCOM_REQ_STATE_GRECV_REQUEST;
	gen_req->pub.state |= PSCOM_REQ_STATE_IO_STARTED;

	/* the request shall be the current request of the connection */
	recv_con->in.req = gen_req;

	/* create the receive request */
	pscom_request_t *recv_req = pscom_request_create(0, 100);
	recv_req->connection = &recv_con->pub;

	/* read_start() should be called at least once */
	expect_function_call_any(check_read_start_called);
	expect_value(check_read_start_called, con, recv_con);

	/* post the actual receive request */
	pscom_post_recv(recv_req);

	/*
	 * read_start() should be called lastly
	 * TODO: check connection state, once this is available within the pscom
	 */
	assert_int_equal(connection_state(TESTCON_OP_NOP), TESTCON_STATE_OPENED);
}

/**
 * \brief Test pscom_post_recv() for generated requests
 *
 * Given: A generated request
 * When: pscom_post_recv() is called
 * Then: the matching user request should not be identified as a generated
 *       request
 */
void test_post_recv_genreq_state(void **state)
{
	/* obtain the dummy connection from the test setup */
	pscom_con_t *recv_con =  (pscom_con_t*)(*state);

	/* create generated requests and enqueue to the list of net requests */
	pscom_header_net_t nh = {
		.xheader_len = 0,
		.data_len = 0,
	};
	pscom_req_t *gen_req = _pscom_generate_recv_req(NULL, &nh);
	init_gen_req(&recv_con->pub, gen_req);
	_pscom_net_recvq_user_enq(recv_con, gen_req);

	/*
	 * set the appropriate request state:
	 * -> generated requests
	 * -> IO has been started
	 */

	/* create the receive request *not* being a generated request */
	pscom_request_t *recv_req = pscom_request_create(0, 100);
	recv_req->state &= ~PSCOM_REQ_STATE_GRECV_REQUEST;
	recv_req->connection = &recv_con->pub;

	/* post the actual receive request */
	pscom_post_recv(recv_req);

	/* the recv request should *not* be marked as a generated request */
	assert_false(recv_req->state & PSCOM_REQ_STATE_GRECV_REQUEST);
}

/**
 * \brief Test pscom_post_recv() with a regular recv request with a given connection
 *
 * Given: A public recv request handle with an associated connection but without a socket
 * When: pscom_post_recv() is called with this request
 * Then: the request is enqueued on the recvq_user of the connection
 */
void test_post_recv_on_con(void **state)
{
	/* obtain the dummy connection from the test setup */
	pscom_con_t *recv_con =  (pscom_con_t*)(*state);

	/* assume that all queues are empty */
	assert_true(list_empty(&recv_con->recvq_user));
	assert_true(list_empty(&pscom.recvq_any_global));
	assert_true(list_empty(&get_sock(recv_con->pub.socket)->recvq_any));

	/* create and post a regular recv request on the connection */
	pscom_request_t *recv_req = pscom_request_create(0, 0);
	recv_req->connection = &recv_con->pub;
	recv_req->socket = NULL;

	pscom_post_recv(recv_req);

	/* assume that the request is now enqueued on the recvq_user of the connection */
	assert_int_equal(list_count(&recv_con->recvq_user), 1);
	assert_ptr_equal(list_entry(recv_con->recvq_user.next, pscom_req_t, next), get_req(recv_req));

	/* assume that the request's socket has been set properly */
	assert_ptr_equal(recv_req->socket, recv_con->pub.socket);

	recv_req->state |= PSCOM_REQ_STATE_DONE;
	pscom_request_free(recv_req);
}

/**
 * \brief Test pscom_post_recv() with an any-source recv request with a given socket
 *
 * Given: A public recv request handle with an associated socket but without a connection
 * When: pscom_post_recv() is called with this any-source request
 * Then: the any-source request is enqueued on the recvq_any queue of the socket
 */
void test_post_any_recv_on_sock(void **state)
{
	/* obtain the dummy connection from the test setup */
	pscom_con_t *recv_con =  (pscom_con_t*)(*state);

	/* assume that all queues are empty */
	assert_true(list_empty(&recv_con->recvq_user));
	assert_true(list_empty(&pscom.recvq_any_global));
	assert_true(list_empty(&get_sock(recv_con->pub.socket)->recvq_any));

	/* create and post an any recv request on the socket */
	pscom_request_t *recv_any_req = pscom_request_create(0, 0);
	recv_any_req->connection = NULL;
	recv_any_req->socket = recv_con->pub.socket;

	pscom_post_recv(recv_any_req);

	/* assume that the request as been enqueued to the socket and that the
	   global any-source queue is still empty */
	assert_int_equal(list_count(&get_sock(recv_con->pub.socket)->recvq_any), 1);
	assert_true(list_empty(&pscom.recvq_any_global));

	recv_any_req->state |= PSCOM_REQ_STATE_DONE;
	pscom_request_free(recv_any_req);
}

/**
 * \brief Test pscom_post_recv() with an any-source recv request without a socket
 *
 * Given: A public recv request handle with no socket and no connection given
 * When: pscom_post_recv() is called with this any-source request
 * Then: the any-source request is enqueued on the recvq_any_global queue
 */
void test_post_any_recv_on_global_queue(void **state)
{
	/* obtain the dummy connection from the test setup */
	pscom_con_t *recv_con =  (pscom_con_t*)(*state);

	/* assume that all queues are empty */
	assert_true(list_empty(&recv_con->recvq_user));
	assert_true(list_empty(&pscom.recvq_any_global));
	assert_true(list_empty(&get_sock(recv_con->pub.socket)->recvq_any));

	/* create and post an any recv request without a given socket */
	pscom_request_t *recv_any_req = pscom_request_create(0, 0);
	recv_any_req->connection = NULL;
	recv_any_req->socket = NULL;

	pscom_post_recv(recv_any_req);

	/* assume that the request as been enqueued to the global any-source queue
	   and that the socket's any-source queue is still empty */
	assert_int_equal(list_count(&pscom.recvq_any_global), 1);
	assert_true(list_empty(&get_sock(recv_con->pub.socket)->recvq_any));

	recv_any_req->state |= PSCOM_REQ_STATE_DONE;
	pscom_request_free(recv_any_req);
}

/**
 * \brief Test pscom_post_recv() with a regular recv request after an any-source request
 *
 * Given: An any-source request is enqueued on the recvq_any queue of a socket
 * When: pscom_post_recv() is called with a subsequent regular receive request
 * Then: both requests are enqueued on the recvq_any of the socket
 */
void test_post_recv_on_con_after_any_recv_on_sock(void **state)
{
	/* obtain the dummy connection from the test setup */
	pscom_con_t *recv_con =  (pscom_con_t*)(*state);

	/* assume that all queues are empty */
	assert_true(list_empty(&recv_con->recvq_user));
	assert_true(list_empty(&pscom.recvq_any_global));
	assert_true(list_empty(&get_sock(recv_con->pub.socket)->recvq_any));

	/* create and post an any recv request on the socket */
	pscom_request_t *recv_any_req = pscom_request_create(0, 0);
	recv_any_req->connection = NULL;
	recv_any_req->socket = recv_con->pub.socket;

	pscom_post_recv(recv_any_req);

	/* assume that the request as been enqueued to the socket and that the
	   global any-source queue is still empty */
	assert_int_equal(list_count(&get_sock(recv_con->pub.socket)->recvq_any), 1);
	assert_true(list_empty(&pscom.recvq_any_global));

	/* create and post a regular recv request on the connection */
	pscom_request_t *recv_req = pscom_request_create(0, 0);
	recv_req->connection = &recv_con->pub;
	recv_req->socket = recv_con->pub.socket;

	pscom_post_recv(recv_req);

	/* assume that both requests are now enqueued on the recvq_any of the socket */
	assert_int_equal(list_count(&get_sock(recv_con->pub.socket)->recvq_any), 2);
	assert_true(list_empty(&recv_con->recvq_user));

	/* assume that the global any-source queue is still empty */
	assert_true(list_empty(&pscom.recvq_any_global));

	recv_req->state |= PSCOM_REQ_STATE_DONE;
	pscom_request_free(recv_req);

	recv_any_req->state |= PSCOM_REQ_STATE_DONE;
	pscom_request_free(recv_any_req);
}

/**
 * \brief Test pscom_post_recv() with an any-source recv following a regular recv request
 *
 * Given: A regular request is enqueued on the recvq_user queue of a connection
 * When: pscom_post_recv() is called with a subsequent any-source request
 * Then: both requests are enqueued to the two separate queues (con and sock)
 */
void test_post_any_recv_on_sock_after_recv_on_con(void **state)
{
	/* obtain the dummy connection from the test setup */
	pscom_con_t *recv_con =  (pscom_con_t*)(*state);

	/* assume that all queues are empty */
	assert_true(list_empty(&recv_con->recvq_user));
	assert_true(list_empty(&pscom.recvq_any_global));
	assert_true(list_empty(&get_sock(recv_con->pub.socket)->recvq_any));

	/* create and post a regular recv request on the connection */
	pscom_request_t *recv_req = pscom_request_create(0, 0);
	recv_req->connection = &recv_con->pub;
	recv_req->socket = recv_con->pub.socket;

	pscom_post_recv(recv_req);

	/* assume that the request as been enqueued to the connection queue and that the
	   any-source queues are is still empty */
	assert_int_equal(list_count(&recv_con->recvq_user), 1);
	assert_true(list_empty(&pscom.recvq_any_global));
	assert_true(list_empty(&get_sock(recv_con->pub.socket)->recvq_any));

	/* create and post an any recv request on the socket */
	pscom_request_t *recv_any_req_sock = pscom_request_create(0, 0);
	recv_any_req_sock->connection = NULL;
	recv_any_req_sock->socket = recv_con->pub.socket;

	pscom_post_recv(recv_any_req_sock);

	/* assume that the request as been enqueued to the socket and that the
	   global any-source queue is still empty */
	assert_int_equal(list_count(&recv_con->recvq_user), 1);
	assert_int_equal(list_count(&get_sock(recv_con->pub.socket)->recvq_any), 1);
	assert_true(list_empty(&pscom.recvq_any_global));

	recv_req->state |= PSCOM_REQ_STATE_DONE;
	pscom_request_free(recv_req);

	recv_any_req_sock->state |= PSCOM_REQ_STATE_DONE;
	pscom_request_free(recv_any_req_sock);
}

////////////////////////////////////////////////////////////////////////////////
/// pscom_req_prepare_send_pending()
////////////////////////////////////////////////////////////////////////////////
/**
 * \brief Test pscom_req_prepare_send_pending() for a regular user request
 *
 * Given: A send request
 * When: pscom_req_prepare_send_pending() is called
 * Then: the request is prepared for sending
 */
void test_req_prepare_send_pending_valid_send_request(void **state)
{
	(void) state;

	const uint16_t xheader_len = 42;
	const size_t data_len = 1024;
	pscom_req_t req = {
		.magic = MAGIC_REQUEST,
		.pub.xheader_len = xheader_len,
		.pub.data_len = data_len,
		.pub.data = (void*)0x42,
	};

	pscom_req_prepare_send_pending(&req, PSCOM_MSGTYPE_USER, 0);

	assert_int_equal(req.pub.header.msg_type, PSCOM_MSGTYPE_USER);
	assert_int_equal(req.pub.header.xheader_len, xheader_len);
	assert_int_equal(req.pub.header.data_len, data_len);

	assert_ptr_equal(req.cur_header.iov_base, &req.pub.header);
	assert_ptr_equal(req.cur_data.iov_base, req.pub.data);
	assert_int_equal(req.cur_header.iov_len, sizeof(pscom_header_net_t)+req.pub.xheader_len);
	assert_int_equal(req.cur_data.iov_len, req.pub.data_len);

	assert_int_equal(req.skip, 0);
}

/**
 * \brief Test pscom_req_prepare_send_pending() very long data lengths
 *
 * Given: A send request with a data_len exceeding  PSCOM_DATA_LEN_MASK
 * When: pscom_req_prepare_send_pending() is called
 * Then: the data_len is truncated to PSCOM_DATA_LEN_MASK in the network header
 *
 * TODO: What is the purpose of this behavior?
 */
void test_req_prepare_send_pending_truncate_data_len(void **state)
{
	(void) state;

	const size_t data_len = PSCOM_DATA_LEN_MASK + 1024;
	pscom_req_t req = {
		.magic = MAGIC_REQUEST,
		.pub.data_len = data_len,
	};

	pscom_req_prepare_send_pending(&req, PSCOM_MSGTYPE_USER, 0);

	assert_true(req.pub.header.data_len <= PSCOM_DATA_LEN_MASK);
}


////////////////////////////////////////////////////////////////////////////////
/// pscom_get_rma_read_receiver_failing_rma_write()
////////////////////////////////////////////////////////////////////////////////
static
int rma_write_error(pscom_con_t *con, void *src, pscom_rendezvous_msg_t *des,
			   void (*io_done)(void *priv, int err), void *priv)
{
	/* simply call io_done with error */
	io_done(priv, 1);

	return 0;
}

/**
 * \brief Test pscom_get_rma_read_receiver() for an error in con->rma_write
 *
 * Given: A rendezvous send requests and incomming PSCOM_MSGTYPE_RMA_READ
 * When: con->rma_write() fails
 * Then: the request should be marked with an error; pending IO should be zero
 *       and the connection should be closed for writing
 */
void test_pscom_get_rma_read_receiver_failing_rma_write(void **state)
{
	/* obtain the dummy connection from the test setup */
	pscom_con_t *recv_con =  (pscom_con_t*)(*state);
	recv_con->rma_read = NULL;
	recv_con->rma_write = rma_write_error;

	/* poen connection for reading and writing */
	recv_con->pub.state = PSCOM_CON_STATE_RW;


	/* create the appropriate network header */
	pscom_header_net_t nh = {
		.msg_type = PSCOM_MSGTYPE_RMA_READ,
	};

	/* create user rndv request an append to pending IO queue */
	pscom_req_t *user_req = pscom_req_create(0, 100);
	user_req->pub.state = PSCOM_REQ_STATE_RENDEZVOUS_REQUEST |
		PSCOM_REQ_STATE_SEND_REQUEST | PSCOM_REQ_STATE_POSTED;
	user_req->pub.connection = &recv_con->pub;

	pscom_lock(); {
		_pscom_pendingio_cnt_inc(recv_con, user_req);
		_pscom_get_rma_read_receiver(recv_con, &nh);
	} pscom_unlock();

	assert_true(user_req->pub.state & PSCOM_REQ_STATE_ERROR);
	assert_true(user_req->pending_io == 0);
	assert_false(recv_con->pub.state & PSCOM_CON_STATE_W);
}


////////////////////////////////////////////////////////////////////////////////
/// Rendezvous Receiver (RMA write)
////////////////////////////////////////////////////////////////////////////////
static
int rma_write_null(pscom_con_t *con, void *src, pscom_rendezvous_msg_t *des,
			   void (*io_done)(void *priv, int err), void *priv)
{
	/* do nothing here */

	return 0;
}
/**
 * \brief Test read error during rendezvous on the receiver side
 *
 * Given: An RMA read request on a connection relying on RMA put
 * When: a read error occurs
 * Then: the request should be marked with an error and the connection should
 *       be closed for reading
 */
void test_rndv_recv_read_error(void **state)
{
	const size_t data_len = 32768;

	/* obtain the dummy connections from the test setup */
	dummy_con_pair_t *con_pair = (dummy_con_pair_t*)(*state);
	pscom_con_t *recv_con, *send_con;

	recv_con = con_pair->recv_con;
	recv_con->rma_read = NULL;
	recv_con->rma_write = rma_write_null;

	send_con = con_pair->send_con;
	send_con->rma_read = NULL;
	send_con->rma_write = rma_write_null;

	/* open recv connection for reading and writing */
	recv_con->pub.state = PSCOM_CON_STATE_RW;

	/* create and post a user recv request on the recv con */
	pscom_request_t *user_recv_req = pscom_request_create(0, 100);
	user_recv_req->connection = &recv_con->pub;
	user_recv_req->data_len = data_len;
	pscom_post_recv(user_recv_req);

	/* create a matching send requests, i.e., the correct network header*/
	pscom_req_t *send_req = pscom_req_create(100, 0);
	send_req->pub.data_len = data_len;
	send_req->pub.connection = &send_con->pub;
	pscom_req_t *rndv_req = pscom_prepare_send_rendezvous_inline(send_req, PSCOM_MSGTYPE_USER);
	pscom_req_prepare_send(rndv_req, PSCOM_MSGTYPE_RENDEZVOUS_REQ);
	pscom_header_net_t *nh = &rndv_req->pub.header;

	/* assume we received a PSCOM_MSGTYPE_RENDEZVOUS_REQ */
	pscom_get_rendezvous_receiver(recv_con, nh);

	/* force READ error on the connection */
	pscom_lock(); {
		pscom_read_done(recv_con, NULL, 0);
	} pscom_unlock();

	/* check request and conection state */
	assert_true(user_recv_req->state & PSCOM_REQ_STATE_ERROR);
	assert_false(recv_con->pub.state & PSCOM_CON_STATE_R);
}
