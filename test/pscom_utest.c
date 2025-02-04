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
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <cmocka.h>

#include "pscom_utest.h"
#include "pscom/test_cuda.h"
#include "pscom/test_env.h"
#include "pscom/test_io.h"

#include "pscom4ucp/test_pscom4ucp.h"

#include "util/test_utils_con.h"
#include "util/test_utils_cuda.h"
#include "util/test_utils_env.h"

pscom_utest_t pscom_utest = {
	.mock_functions = {
		.memcpy = 0,
		.malloc = 0,
	},
};

#define TEST_GROUP_SIZE(test_group) (sizeof(test_group)/sizeof(struct CMUnitTest))

////////////////////////////////////////////////////////////////////////////////
/// Setup/teardown helpers
////////////////////////////////////////////////////////////////////////////////


int main(void)
{
	size_t failed_tests = 0;
	size_t total_tests = 0;

	/* determine output type */
	char *output_format = getenv("PSP_UTEST_OUTPUT");
	if (output_format && !strcmp(output_format, "xml")) {
		cmocka_set_message_output(CM_OUTPUT_XML);
	}

	/* pscom_io tests */
	const struct CMUnitTest pscom_io_tests[] = {
		cmocka_unit_test(test_req_prepare_send_pending_valid_send_request),
		cmocka_unit_test(test_req_prepare_send_pending_truncate_data_len),
		cmocka_unit_test_setup_teardown(
			test_post_recv_partial_genreq,
			setup_dummy_con,
			teardown_dummy_con),
		cmocka_unit_test_setup_teardown(
			test_post_recv_genreq_state,
			setup_dummy_con,
			teardown_dummy_con),
		cmocka_unit_test_setup_teardown(
			test_post_recv_on_con,
			setup_dummy_con,
			teardown_dummy_con),
		cmocka_unit_test_setup_teardown(
			test_post_any_recv_on_sock,
			setup_dummy_con,
			teardown_dummy_con),
		cmocka_unit_test_setup_teardown(
			test_post_any_recv_on_global_queue,
			setup_dummy_con,
			teardown_dummy_con),
		cmocka_unit_test_setup_teardown(
			test_post_recv_on_con_after_any_recv_on_sock,
			setup_dummy_con,
			teardown_dummy_con),
		cmocka_unit_test_setup_teardown(
			test_post_any_recv_on_sock_after_recv_on_con,
			setup_dummy_con,
			teardown_dummy_con),
		cmocka_unit_test_setup_teardown(
			test_pscom_get_rma_read_receiver_failing_rma_write,
			setup_dummy_con,
			teardown_dummy_con),
		cmocka_unit_test_setup_teardown(
			test_rndv_recv_read_error,
			setup_dummy_con_pair,
			teardown_dummy_con_pair),
	};
	total_tests += TEST_GROUP_SIZE(pscom_io_tests);
	failed_tests += cmocka_run_group_tests(pscom_io_tests, NULL, NULL);

	/* pscom_env tests */
	const struct CMUnitTest pscom_env_tests[] = {
		cmocka_unit_test(test_env_table_parse_empty_table),
		cmocka_unit_test(test_env_table_parse_null_table),
		cmocka_unit_test(test_env_table_parse_null_var),
		cmocka_unit_test(test_env_table_parse_null_parser),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_uint_default,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_uint,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_uint_inf,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_uint_auto,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_uint_typo,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_int_default,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_int,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_int_empty,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_size_t_default,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_size_t,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_size_t_typo,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_str_default,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_str,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_dir_default,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_dir,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_multi_entry,
			backup_three_test_val_env,
			restore_three_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_multi_entry_failing_entry,
			backup_three_test_val_env,
			restore_three_test_val_env),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_size_t_parent_set,
			backup_test_val_env_and_parent,
			restore_test_val_env_and_parent),
		cmocka_unit_test_setup_teardown(
			test_env_table_parse_single_size_t_parent_set_and_env_set,
			backup_test_val_env_and_parent,
			restore_test_val_env_and_parent),
		cmocka_unit_test(test_env_parser_get_int),
		cmocka_unit_test(test_env_parser_get_uint),
		cmocka_unit_test(test_env_parser_get_uint_inf),
		cmocka_unit_test(test_env_parser_get_uint_auto),
		cmocka_unit_test(test_env_parser_get_size_t),
		cmocka_unit_test(test_env_parser_get_str),
		cmocka_unit_test(test_env_parser_get_dir),
		cmocka_unit_test(test_env_table_register_simple),
		cmocka_unit_test(test_env_table_register_no_mem),
		cmocka_unit_test(test_env_table_register_and_parse_no_mem),
		cmocka_unit_test(test_env_table_register_and_parse_simple),
		cmocka_unit_test_setup_teardown(
			test_env_table_register_and_parse_env_var,
			backup_test_val_env,
			restore_test_val_env),
		cmocka_unit_test(test_env_clear_table_list_empty),
		cmocka_unit_test(test_env_clear_table_list_filled),
	};
	total_tests += TEST_GROUP_SIZE(pscom_env_tests);
	failed_tests += cmocka_run_group_tests(pscom_env_tests, NULL, NULL);

#ifdef UCP_ENABLED
	/* pscom4ucp tests */
	const struct CMUnitTest pscom4ucp_tests[] = {
		cmocka_unit_test(test_ucp_disable_fast_initialization),
		cmocka_unit_test(test_ucp_disable_fast_initialization_via_environment),
		/* this has has to go last due to the static varaiable in psucp_init() */
		cmocka_unit_test(test_ucp_is_initialized_within_plugin),
	};
	total_tests += TEST_GROUP_SIZE(pscom4ucp_tests);
	failed_tests += cmocka_run_group_tests(pscom4ucp_tests, NULL, NULL);
#endif /* UCP_ENABLED */


#ifdef PSCOM_CUDA_AWARENESS
	/* CUDA-related tests */
	const struct CMUnitTest pscom_cuda_tests[] = {
		cmocka_unit_test(test_is_cuda_enabled_returns_zero_if_disabled),
		cmocka_unit_test(test_is_cuda_enabled_returns_one_if_enabled),
		cmocka_unit_test(test_cuda_init_cuInit_error),
		cmocka_unit_test(test_cuda_init_returns_success_if_disabled),
		cmocka_unit_test(test_cuda_init_device_count_error),
		cmocka_unit_test(test_cuda_init_device_count_zero),
		cmocka_unit_test(test_cuda_init_uva_check_fails),
		cmocka_unit_test(test_cuda_init_no_uva_support),
		cmocka_unit_test(test_cuda_cleanup_returns_success_if_disabled),
		cmocka_unit_test_setup_teardown(
			test_cuda_cleanup_destroys_cuda_streams,
			setup_dummy_streams,
			clear_dummy_streams),
		cmocka_unit_test(test_cuda_cleanup_for_inactive_device),
		cmocka_unit_test(test_cuda_cleanup_for_unclear_device_status),
		cmocka_unit_test(test_cuda_cleanup_for_cuda_deinitialized),
		cmocka_unit_test_setup_teardown(
			test_cuda_cleanup_for_failing_stream_destroy,
			setup_dummy_streams,
			clear_dummy_streams),
		cmocka_unit_test(test_buffer_needs_staging_if_cuda_disabled),
		cmocka_unit_test(test_buffer_needs_staging_con_not_cuda_aware),
		cmocka_unit_test(test_buffer_needs_staging_con_cuda_aware),
		cmocka_unit_test(test_is_gpu_mem_if_cuda_disabled),
		cmocka_unit_test(test_is_gpu_mem_get_attributes_fails),
		cmocka_unit_test(test_is_gpu_mem_managed_memory),
		cmocka_unit_test(test_is_gpu_mem_device_memory),
		cmocka_unit_test(test_is_gpu_mem_wrapper_device_memory),
		cmocka_unit_test(test_is_gpu_mem_sync_memop_disabled),
		cmocka_unit_test(test_is_gpu_mem_sync_memop_enabled),
		cmocka_unit_test(test_pscom_memcpy_gpu_safe_from_user_host_mem),
		cmocka_unit_test_setup_teardown(
			test_pscom_memcpy_gpu_safe_from_user_device_mem,
			setup_dummy_streams,
			clear_dummy_streams),
		cmocka_unit_test(test_pscom_memcpy_gpu_safe_from_user_creates_cuda_stream),
		cmocka_unit_test(test_pscom_memcpy_gpu_safe_to_user_host_mem),
		cmocka_unit_test_setup_teardown(
			test_pscom_memcpy_gpu_safe_to_user_device_mem,
			setup_dummy_streams,
			clear_dummy_streams),
		cmocka_unit_test(test_pscom_memcpy_gpu_safe_default_host_mem),
		cmocka_unit_test_setup_teardown(
			test_pscom_memcpy_gpu_safe_default_device_mem,
			setup_dummy_streams,
			clear_dummy_streams),
		cmocka_unit_test(test_pscom_memcpy_host_mem),
		cmocka_unit_test_setup_teardown(
			test_pscom_stage_buffer_dev_mem_no_con,
			setup_dummy_streams,
			clear_dummy_streams),
		cmocka_unit_test(test_pscom_stage_buffer_host_mem),
		cmocka_unit_test_setup_teardown(
			test_pscom_unstage_buffer_dev_mem,
			setup_dummy_streams,
			clear_dummy_streams),
		cmocka_unit_test(test_pscom_unstage_buffer_dev_mem_no_copy),
		cmocka_unit_test(test_pscom_unstage_buffer_host_mem),
	};
	total_tests += TEST_GROUP_SIZE(pscom_cuda_tests);
	failed_tests += cmocka_run_group_tests(pscom_cuda_tests, NULL, NULL);
#endif /* PSCOM_CUDA_AWARENESS */

	printf("\n\n");
	printf("Total tests      : %lu\n", total_tests);
	printf("Succeeding tests : %lu\n", total_tests-failed_tests);
	printf("Failing tests    : %lu\n", failed_tests);
	printf("\n\n");

	return (int)failed_tests;
}
