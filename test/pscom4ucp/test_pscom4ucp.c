/*
 * ParaStation
 *
 * Copyright (C) 2021      ParTec Cluster Competence Center GmbH, Munich
 * Copyright (C) 2021      ParTec AG, Munich
 *
 * This file may be distributed under the terms of the Q Public License
 * as defined in the file LICENSE.QPL included in the packaging of this
 * file.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <setjmp.h>
#include <cmocka.h>

#include <errno.h>

#include "pscom_priv.h"
#include "pscom_utest.h"

#include "pscom_ucp.h"
//#include "pscom_ucp.c" /* we need to access some static functions */

////////////////////////////////////////////////////////////////////////////////
/// Some forward declarations
////////////////////////////////////////////////////////////////////////////////
extern pscom_plugin_t pscom_plugin;
void pscom_env_ucp_fastinit_set(unsigned int ucp_fastinit);

////////////////////////////////////////////////////////////////////////////////
/// fast initialization
////////////////////////////////////////////////////////////////////////////////
/**
 * \brief Test if UCP is initialized during plugin initialization by default
 *
 * Given: UCP fast initialization is enabled
 * When: the pscom4ucp plugin is initialized
 * Then: UCP is initialized as well
 */
void test_ucp_is_initialized_within_plugin(void **state)
{
	(void) state;

        /* enable UCP fast initialization */
	pscom.env.ucp_fastinit = 1;

	expect_function_calls(__wrap_ucp_init_version, 1);

        /* initialize the pscom4ucp plugin */
        pscom_plugin.init();
}


/**
 * \brief Test disabled fast initialization of UCP
 *
 * Given: Fast initialization of UCP is disabled via the pscom API
 * When: the pscom4ucp plugin is initialized
 * Then: UCP is not initialized
 */
void test_ucp_disable_fast_initialization(void **state)
{
	(void) state;

        /* save original value of PSP_UCP_FASTINIT */
        char *orig_fastinit = getenv("PSP_UCP_FASTINIT");

        pscom_env_ucp_fastinit_set(0);

        /* initialize the pscom4ucp plugin */
        pscom_plugin.init();

        /* restore original value of PSP_UCP_FASTINIT */
        if (orig_fastinit) {
                setenv("PSP_UCP_FASTINIT", orig_fastinit, 1);
        } else {
                unsetenv("PSP_UCP_FASTINIT");
        }
}


/**
 * \brief Test disabled fast initialization of UCP via environment
 *
 * Given: Fast initialization of UCP is disabled via environment variable
 * When: the pscom4ucp plugin is initialized
 * Then: UCP is not initialized
 */
void test_ucp_disable_fast_initialization_via_environment(void **state)
{
	(void) state;

        /* save original value of PSP_UCP_FASTINIT */
        char *orig_fastinit = getenv("PSP_UCP_FASTINIT");

        setenv("PSP_UCP_FASTINIT", "0", 1);

        /* initialize the pscom4ucp plugin */
        pscom_plugin.init();

        /* restore original value of PSP_UCP_FASTINIT */
        if (orig_fastinit) {
                setenv("PSP_UCP_FASTINIT", orig_fastinit, 1);
        } else {
                unsetenv("PSP_UCP_FASTINIT");
        }
}
