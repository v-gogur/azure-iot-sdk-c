// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>

void* real_malloc(size_t size)
{
	return malloc(size);
}

void real_free(void* ptr)
{
	free(ptr);
}

#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include "testrunnerswitcher.h"
#include "azure_c_shared_utility/macro_utils.h"
#include "umock_c.h"
#include "umocktypes_charptr.h"
#include "umocktypes_stdint.h"
#include "umock_c_negative_tests.h"
#include "umocktypes.h"
#include "umocktypes_c.h"

#define ENABLE_MOCKS

#include "azure_c_shared_utility/crt_abstractions.h"
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/doublylinkedlist.h"
#include "azure_c_shared_utility/uniqueid.h"
#include "azure_uamqp_c/session.h"
#include "azure_uamqp_c/link.h"
#include "azure_uamqp_c/messaging.h"
#include "azure_uamqp_c/message_sender.h"
#include "azure_uamqp_c/message_receiver.h"
#include "iothub_client_private.h"
#include "iothub_message.h"
#include "uamqp_messaging.h"

#undef ENABLE_MOCKS

#include "iothubtransport_amqp_messenger.h"

static TEST_MUTEX_HANDLE g_testByTest;
static TEST_MUTEX_HANDLE g_dllByDll;

DEFINE_ENUM_STRINGS(UMOCK_C_ERROR_CODE, UMOCK_C_ERROR_CODE_VALUES)

static void on_umock_c_error(UMOCK_C_ERROR_CODE error_code)
{
    char temp_str[256];
    (void)snprintf(temp_str, sizeof(temp_str), "umock_c reported error :%s", ENUM_TO_STRING(UMOCK_C_ERROR_CODE, error_code));
    ASSERT_FAIL(temp_str);
}

#define TEST_DEVICE_ID                                    "my_device"
#define TEST_IOTHUB_HOST_FQDN                             "some.fqdn.com"
#define TEST_WAIT_TO_SEND_LIST                            (PDLIST_ENTRY)0x4444
#define TEST_ON_STATE_CHANGED_CB_CONTEXT                  (void*)0x4445
#define TEST_STRING_HANDLE                                (STRING_HANDLE)0x4446


// Helpers

#ifdef __cplusplus
extern "C"
{
#endif

static int g_STRING_sprintf_call_count;
static int g_STRING_sprintf_fail_on_count;
static STRING_HANDLE saved_STRING_sprintf_handle;

int STRING_sprintf(STRING_HANDLE handle, const char* format, ...)
{
	int result;
	saved_STRING_sprintf_handle = handle;
	(void)format;

	g_STRING_sprintf_call_count++;
	if (g_STRING_sprintf_call_count == g_STRING_sprintf_fail_on_count)
	{
		result = __LINE__;
	}
	else
	{
		result = 0;
	}

	return result;
}

#ifdef __cplusplus
}
#endif


static int saved_malloc_returns_count = 0;
static void* saved_malloc_returns[20];

static void* TEST_malloc(size_t size)
{
	saved_malloc_returns[saved_malloc_returns_count] = real_malloc(size);

	return saved_malloc_returns[saved_malloc_returns_count++];
}

static void TEST_free(void* ptr)
{
	int i, j;
	for (i = 0, j = 0; j < saved_malloc_returns_count; i++, j++)
	{
		if (saved_malloc_returns[i] == ptr) j++;

		saved_malloc_returns[i] = saved_malloc_returns[j];
	}

	if (i != j) saved_malloc_returns_count--;

	real_free(ptr);
}


static void* saved_on_state_changed_callback_context;
static MESSENGER_STATE saved_on_state_changed_callback_previous_state;
static MESSENGER_STATE saved_on_state_changed_callback_new_state;

static void TEST_on_state_changed_callback(void* context, MESSENGER_STATE previous_state, MESSENGER_STATE new_state)
{
	saved_on_state_changed_callback_context = context;
	saved_on_state_changed_callback_previous_state = previous_state;
	saved_on_state_changed_callback_new_state = new_state;
}


static MESSENGER_CONFIG global_messenger_config;

static MESSENGER_CONFIG* get_messenger_config()
{
	global_messenger_config.device_id = TEST_DEVICE_ID;
	global_messenger_config.iothub_host_fqdn = TEST_IOTHUB_HOST_FQDN;
	global_messenger_config.wait_to_send_list = TEST_WAIT_TO_SEND_LIST;
	global_messenger_config.on_state_changed_callback = TEST_on_state_changed_callback;
	global_messenger_config.on_state_changed_context = TEST_ON_STATE_CHANGED_CB_CONTEXT;
	
	return &global_messenger_config;
}


static void set_expected_calls_for_messenger_create(MESSENGER_CONFIG* config)
{
	EXPECTED_CALL(malloc(IGNORED_NUM_ARG));
	STRICT_EXPECTED_CALL(STRING_construct(config->device_id));
	STRICT_EXPECTED_CALL(STRING_construct(config->iothub_host_fqdn));
	EXPECTED_CALL(DList_InitializeListHead(IGNORED_PTR_ARG));
}

BEGIN_TEST_SUITE(iothubtransport_amqp_messenger_ut)

TEST_SUITE_INITIALIZE(TestClassInitialize)
{
    TEST_INITIALIZE_MEMORY_DEBUG(g_dllByDll);
    g_testByTest = TEST_MUTEX_CREATE();
    ASSERT_IS_NOT_NULL(g_testByTest);

    umock_c_init(on_umock_c_error);

	int result = umocktypes_charptr_register_types();
	ASSERT_ARE_EQUAL(int, 0, result);
	result = umocktypes_stdint_register_types();
	ASSERT_ARE_EQUAL(int, 0, result);


	REGISTER_UMOCK_ALIAS_TYPE(STRING_HANDLE, void*);
	REGISTER_UMOCK_ALIAS_TYPE(UNIQUEID_RESULT, int);
	REGISTER_UMOCK_ALIAS_TYPE(SESSION_HANDLE, void*);
	REGISTER_UMOCK_ALIAS_TYPE(PDLIST_ENTRY, void*);


	REGISTER_GLOBAL_MOCK_HOOK(gballoc_malloc, TEST_malloc);
	REGISTER_GLOBAL_MOCK_HOOK(malloc, TEST_malloc);
	REGISTER_GLOBAL_MOCK_HOOK(gballoc_free, TEST_free);
	REGISTER_GLOBAL_MOCK_HOOK(free, TEST_free);


	REGISTER_GLOBAL_MOCK_RETURN(STRING_construct, TEST_STRING_HANDLE);
	REGISTER_GLOBAL_MOCK_FAIL_RETURN(STRING_construct, NULL);

	REGISTER_GLOBAL_MOCK_RETURN(STRING_c_str, TEST_IOTHUB_HOST_FQDN);
	REGISTER_GLOBAL_MOCK_FAIL_RETURN(STRING_c_str, NULL);
}

TEST_SUITE_CLEANUP(TestClassCleanup)
{
    umock_c_deinit();

    TEST_MUTEX_DESTROY(g_testByTest);
    TEST_DEINITIALIZE_MEMORY_DEBUG(g_dllByDll);
}

TEST_FUNCTION_INITIALIZE(TestMethodInitialize)
{
    if (TEST_MUTEX_ACQUIRE(g_testByTest))
    {
        ASSERT_FAIL("our mutex is ABANDONED. Failure in test framework");
    }

    umock_c_reset_all_calls();

	g_STRING_sprintf_call_count = 0;
	g_STRING_sprintf_fail_on_count = -1;
}

TEST_FUNCTION_CLEANUP(TestMethodCleanup)
{
    TEST_MUTEX_RELEASE(g_testByTest);
}

// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_001: [If parameter `messenger_config` is NULL, messenger_create() shall return NULL]
TEST_FUNCTION(messenger_create_NULL_config)
{
	// arrange
	umock_c_reset_all_calls();

	// act
	MESSENGER_HANDLE handle = messenger_create(NULL);

	// assert
	ASSERT_ARE_EQUAL(char_ptr, umock_c_get_expected_calls(), umock_c_get_actual_calls());
	ASSERT_ARE_EQUAL(void_ptr, handle, NULL);

	// cleanup
}

// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_002: [If `messenger_config->device_id` is NULL, messenger_create() shall return NULL]
TEST_FUNCTION(messenger_create_config_NULL_device_id)
{
	// arrange
	MESSENGER_CONFIG* config = get_messenger_config();
	config->device_id = NULL;

	umock_c_reset_all_calls();

	// act
	MESSENGER_HANDLE handle = messenger_create(config);

	// assert
	ASSERT_ARE_EQUAL(char_ptr, umock_c_get_expected_calls(), umock_c_get_actual_calls());
	ASSERT_ARE_EQUAL(void_ptr, handle, NULL);

	// cleanup
}

// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_003: [If `messenger_config->iothub_host_fqdn` is NULL, messenger_create() shall return NULL]
TEST_FUNCTION(messenger_create_config_NULL_iothub_host_fqdn)
{
	// arrange
	MESSENGER_CONFIG* config = get_messenger_config();
	config->iothub_host_fqdn = NULL;

	umock_c_reset_all_calls();

	// act
	MESSENGER_HANDLE handle = messenger_create(config);

	// assert
	ASSERT_ARE_EQUAL(char_ptr, umock_c_get_expected_calls(), umock_c_get_actual_calls());
	ASSERT_ARE_EQUAL(void_ptr, handle, NULL);

	// cleanup
}

// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_004: [If `messenger_config->wait_to_send_list` is NULL, messenger_create() shall return NULL]
TEST_FUNCTION(messenger_create_config_NULL_wait_to_send_list)
{
	// arrange
	MESSENGER_CONFIG* config = get_messenger_config();
	config->wait_to_send_list = NULL;

	umock_c_reset_all_calls();

	// act
	MESSENGER_HANDLE handle = messenger_create(config);

	// assert
	ASSERT_ARE_EQUAL(char_ptr, umock_c_get_expected_calls(), umock_c_get_actual_calls());
	ASSERT_ARE_EQUAL(void_ptr, handle, NULL);

	// cleanup
}

// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_006: [messenger_create() shall allocate memory for the messenger instance structure (aka `instance`)]
// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_008: [messenger_create() shall save a copy of `messenger_config->device_id` into `instance->device_id`]
// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_010: [messenger_create() shall save a copy of `messenger_config->iothub_host_fqdn` into `instance->iothub_host_fqdn`]
// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_012: [The pointer `messenger_config->wait_to_send_list` shall be saved into `instance->wait_to_send_list`]
// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_013: [`messenger_config->on_state_changed_callback` shall be saved into `instance->on_state_changed_callback`]
// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_014: [`messenger_config->on_state_changed_context` shall be saved into `instance->on_state_changed_context`]
// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_015: [If no failures occurr, messenger_create() shall return a handle to `instance`]
TEST_FUNCTION(messenger_create_success)
{
	// arrange
	MESSENGER_CONFIG* config = get_messenger_config();

	umock_c_reset_all_calls();
	set_expected_calls_for_messenger_create(config);

	// act
	MESSENGER_HANDLE handle = messenger_create(config);

	// assert
	ASSERT_ARE_EQUAL(char_ptr, umock_c_get_expected_calls(), umock_c_get_actual_calls());
	ASSERT_ARE_EQUAL(void_ptr, (void*)handle, (void*)saved_malloc_returns[0]);

	// cleanup
	messenger_destroy(handle);
}

// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_007: [If malloc() fails, messenger_create() shall fail and return NULL]
// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_009: [If STRING_construct() fails, messenger_create() shall fail and return NULL]
// Tests_SRS_IOTHUBTRANSPORT_AMQP_MESSENGER_09_011: [If STRING_construct() fails, messenger_create() shall fail and return NULL]
TEST_FUNCTION(messenger_create_failure_checks)
{
	// arrange
	ASSERT_ARE_EQUAL(int, 0, umock_c_negative_tests_init());

	MESSENGER_CONFIG* config = get_messenger_config();

	umock_c_reset_all_calls();
	set_expected_calls_for_messenger_create(config);
	umock_c_negative_tests_snapshot();

	// act
	size_t i;
	for (i = 0; i < umock_c_negative_tests_call_count(); i++)
	{
		// arrange
		char error_msg[64];

		umock_c_negative_tests_reset();
		umock_c_negative_tests_fail_call(i);

		MESSENGER_HANDLE handle = messenger_create(config);

		// assert
		sprintf(error_msg, "On failed call %zu", i);
		ASSERT_IS_NULL_WITH_MSG(handle, error_msg);

		// cleanup
		messenger_destroy(handle);
	}

	// cleanup
	umock_c_negative_tests_reset();
	umock_c_negative_tests_deinit();
}




END_TEST_SUITE(iothubtransport_amqp_messenger_ut)
