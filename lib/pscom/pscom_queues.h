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

#ifndef _PSCOM_QUEUES_H_
#define _PSCOM_QUEUES_H_

#include "pscom_priv.h"

/*************
 * Sendq
 */

void _pscom_sendq_enq(pscom_con_t *con, pscom_req_t *req);
void _pscom_sendq_deq(pscom_con_t *con, pscom_req_t *req);


/*************
 * Receive requests
 */
void _pscom_recv_req_cnt_inc(pscom_con_t *con);
void _pscom_recv_req_cnt_dec(pscom_con_t *con);
void _pscom_recv_req_cnt_any_inc(pscom_sock_t *sock);
void _pscom_recv_req_cnt_any_dec(pscom_sock_t *sock);
void _pscom_recv_req_cnt_check_start(pscom_con_t *con);
void _pscom_recv_req_cnt_check_stop(pscom_con_t *con);


/*************
 * Recvq user
 */

void _pscom_recvq_user_enq(pscom_req_t *req);
void _pscom_recvq_user_deq(pscom_req_t *req);


pscom_req_t *_pscom_recvq_user_find_and_deq(pscom_con_t *con, pscom_header_net_t *header);


/* used for debug: */
int _pscom_recvq_user_is_inside(pscom_req_t *req);


/* if possible, move all req's from recvq_any to recvq_user. */
void _pscom_recvq_any_cleanup(pscom_sock_t *sock);


/*************
 * Recvq ctrl
 */

void _pscom_recvq_ctrl_enq(pscom_con_t *con, pscom_req_t *req);
void _pscom_recvq_ctrl_deq(pscom_con_t *con, pscom_req_t *req);

pscom_req_t *_pscom_recvq_ctrl_find_and_deq(pscom_con_t *con, pscom_header_net_t *header);


/*************
 * Net recvq user (network generated requests)
 */


/* enqueue a network generated user request */
void _pscom_net_recvq_user_enq(pscom_con_t *con, pscom_req_t *req);


void _pscom_net_recvq_user_deq(pscom_req_t *req);


/* find req matching net generated user request. */
pscom_req_t *_pscom_net_recvq_user_find(pscom_req_t *req);


/*************
 * Net recvq ctrl (network generated requests)
 */


/* enqueue a network generated ctrl request */
void _pscom_net_recvq_ctrl_enq(pscom_con_t *con, pscom_req_t *req);


void _pscom_net_recvq_ctrl_deq(pscom_req_t *req);


/* find req matching net generated ctrl request. */
pscom_req_t *_pscom_net_recvq_ctrl_find(pscom_req_t *req);


/*************
 * Recvq RMA
 */


void _pscom_recvq_rma_enq(pscom_con_t *con, pscom_req_t *req);


void _pscom_recvq_rma_deq(pscom_con_t *con, pscom_req_t *req);


/*************
 * Recvq bcast
 */
//void _pscom_recvq_bcast_enq(pscom_req_t *req);
//void _pscom_recvq_bcast_deq(pscom_req_t *req);
//void _pscom_net_recvq_bcast_deq(pscom_req_t *req);


#endif /* _PSCOM_QUEUES_H_ */
