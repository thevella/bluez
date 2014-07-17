/*
 * Copyright (C) 2014 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "tester-main.h"

static char exec_dir[PATH_MAX + 1];

static gint scheduled_cbacks_num;

#define EMULATOR_SIGNAL_TIMEOUT 2 /* in seconds */
#define EMULATOR_SIGNAL "emulator_started"

static gboolean check_callbacks_called(gpointer user_data)
{
	/*
	 * Wait for all callbacks scheduled in current test context to execute
	 * in main loop. This will avoid late callback calls after test case has
	 * already failed or timed out.
	 */

	if (g_atomic_int_get(&scheduled_cbacks_num) == 0) {
		tester_teardown_complete();
		return FALSE;
	} else if (scheduled_cbacks_num < 0) {
		tester_warn("Unscheduled callback called!");
		return FALSE;
	}

	return TRUE;
}

static void check_daemon_term(void)
{
	int status;
	pid_t pid;
	struct test_data *data = tester_get_data();

	if (!data)
		return;

	pid = waitpid(data->bluetoothd_pid, &status, WNOHANG);
	if (pid != data->bluetoothd_pid)
		return;

	data->bluetoothd_pid = 0;

	if (WIFEXITED(status) && (WEXITSTATUS(status) == EXIT_SUCCESS)) {
		g_idle_add(check_callbacks_called, NULL);
		return;
	}

	tester_warn("Unexpected Daemon shutdown with status %d", status);
}

static gboolean signal_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct signalfd_siginfo si;
	ssize_t result;
	int fd;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP))
		return FALSE;

	fd = g_io_channel_unix_get_fd(channel);

	result = read(fd, &si, sizeof(si));
	if (result != sizeof(si))
		return FALSE;

	switch (si.ssi_signo) {
	case SIGCHLD:
		check_daemon_term();
		break;
	}

	return TRUE;
}

static guint setup_signalfd(void)
{
	GIOChannel *channel;
	guint source;
	sigset_t mask;
	int fd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
		return 0;

	fd = signalfd(-1, &mask, 0);
	if (fd < 0)
		return 0;

	channel = g_io_channel_unix_new(fd);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				signal_handler, NULL);

	g_io_channel_unref(channel);

	return source;
}

static void test_post_teardown(const void *test_data)
{
	struct test_data *data = tester_get_data();

	hciemu_unref(data->hciemu);
	data->hciemu = NULL;

	g_source_remove(data->signalfd);
	data->signalfd = 0;
}

static void bluetoothd_start(int hci_index)
{
	char prg_name[PATH_MAX + 1];
	char index[8];
	char *prg_argv[5];

	snprintf(prg_name, sizeof(prg_name), "%s/%s", exec_dir, "bluetoothd");
	snprintf(index, sizeof(index), "%d", hci_index);

	prg_argv[0] = prg_name;
	prg_argv[1] = "-i";
	prg_argv[2] = index;
	prg_argv[3] = "-d";
	prg_argv[4] = NULL;

	if (!tester_use_debug())
		fclose(stderr);

	execve(prg_argv[0], prg_argv, NULL);
}

static void emulator(int pipe, int hci_index)
{
	static const char SYSTEM_SOCKET_PATH[] = "\0android_system";
	char buf[1024];
	struct sockaddr_un addr;
	struct timeval tv;
	int fd;
	ssize_t len;

	fd = socket(PF_LOCAL, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		goto failed;

	tv.tv_sec = EMULATOR_SIGNAL_TIMEOUT;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, SYSTEM_SOCKET_PATH, sizeof(SYSTEM_SOCKET_PATH));

	if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		perror("Failed to bind system socket");
		goto failed;
	}

	len = write(pipe, EMULATOR_SIGNAL, sizeof(EMULATOR_SIGNAL));
	if (len != sizeof(EMULATOR_SIGNAL))
		goto failed;

	memset(buf, 0, sizeof(buf));

	len = read(fd, buf, sizeof(buf));
	if (len <= 0 || strcmp(buf, "bluetooth.start=daemon"))
		goto failed;

	close(pipe);
	close(fd);
	return bluetoothd_start(hci_index);

failed:
	close(pipe);

	if (fd >= 0)
		close(fd);
}

static void mgmt_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	tester_print("%s%s", prefix, str);
}

static void read_info_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();
	const struct mgmt_rp_read_info *rp = param;
	char addr[18];
	uint16_t manufacturer;
	uint32_t supported_settings, current_settings;

	tester_print("Read Info callback");
	tester_print("  Status: 0x%02x", status);

	if (status || !param) {
		tester_pre_setup_failed();
		return;
	}

	ba2str(&rp->bdaddr, addr);
	manufacturer = btohs(rp->manufacturer);
	supported_settings = btohl(rp->supported_settings);
	current_settings = btohl(rp->current_settings);

	tester_print("  Address: %s", addr);
	tester_print("  Version: 0x%02x", rp->version);
	tester_print("  Manufacturer: 0x%04x", manufacturer);
	tester_print("  Supported settings: 0x%08x", supported_settings);
	tester_print("  Current settings: 0x%08x", current_settings);
	tester_print("  Class: 0x%02x%02x%02x",
			rp->dev_class[2], rp->dev_class[1], rp->dev_class[0]);
	tester_print("  Name: %s", rp->name);
	tester_print("  Short name: %s", rp->short_name);

	if (strcmp(hciemu_get_address(data->hciemu), addr)) {
		tester_pre_setup_failed();
		return;
	}

	tester_pre_setup_complete();
}

static void index_added_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Index Added callback");
	tester_print("  Index: 0x%04x", index);

	data->mgmt_index = index;

	mgmt_send(data->mgmt, MGMT_OP_READ_INFO, data->mgmt_index, 0, NULL,
					read_info_callback, NULL, NULL);
}

static void index_removed_callback(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Index Removed callback");
	tester_print("  Index: 0x%04x", index);

	if (index != data->mgmt_index)
		return;

	mgmt_unregister_index(data->mgmt, data->mgmt_index);

	mgmt_unref(data->mgmt);
	data->mgmt = NULL;

	tester_post_teardown_complete();
}

static void read_index_list_callback(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	struct test_data *data = tester_get_data();

	tester_print("Read Index List callback");
	tester_print("  Status: 0x%02x", status);

	if (status || !param) {
		tester_pre_setup_failed();
		return;
	}

	mgmt_register(data->mgmt, MGMT_EV_INDEX_ADDED, MGMT_INDEX_NONE,
					index_added_callback, NULL, NULL);

	mgmt_register(data->mgmt, MGMT_EV_INDEX_REMOVED, MGMT_INDEX_NONE,
					index_removed_callback, NULL, NULL);

	data->hciemu = hciemu_new(data->hciemu_type);
	if (!data->hciemu) {
		tester_warn("Failed to setup HCI emulation");
		tester_pre_setup_failed();
		return;
	}

	tester_print("New hciemu instance created");
}

static void test_pre_setup(const void *test_data)
{
	struct test_data *data = tester_get_data();

	data->signalfd = setup_signalfd();
	if (!data->signalfd) {
		tester_warn("Failed to setup signalfd");
		tester_pre_setup_failed();
		return;
	}

	data->mgmt = mgmt_new_default();
	if (!data->mgmt) {
		tester_warn("Failed to setup management interface");
		tester_pre_setup_failed();
		return;
	}

	if (!tester_use_debug())
		fclose(stderr);
	else
		mgmt_set_debug(data->mgmt, mgmt_debug, "mgmt: ", NULL);

	mgmt_send(data->mgmt, MGMT_OP_READ_INDEX_LIST, MGMT_INDEX_NONE, 0,
				NULL, read_index_list_callback, NULL, NULL);
}

static bool match_property(bt_property_t *exp_prop, bt_property_t *rec_prop,
								int prop_num)
{
	if (exp_prop->type && (exp_prop->type != rec_prop->type))
		return 0;

	if (exp_prop->len && (exp_prop->len != rec_prop->len)) {
		tester_debug("Property [%d] len don't match! received=%d, "
					"expected=%d", prop_num, rec_prop->len,
					exp_prop->len);
		return 0;
	}

	if (exp_prop->val && memcmp(exp_prop->val, rec_prop->val,
							exp_prop->len)) {
		tester_debug("Property [%d] value don't match!", prop_num);
		return 0;
	}

	return 1;
}

static int verify_property(bt_property_t *exp_props, int exp_num_props,
				bt_property_t *rec_props, int rec_num_props)
{
	int i, j;
	int exp_prop_to_find = exp_num_props;

	/* Get first exp prop to match and search for it */
	for (i = 0; i < exp_num_props; i++) {
		for (j = 0; j < rec_num_props; j++) {
			if (match_property(&exp_props[i], &rec_props[j], i)) {
				exp_prop_to_find--;
				break;
			}
		}
	}

	if ((i == 0) && exp_props) {
		tester_warn("No property was verified: %s", exp_num_props ?
				"unknown error!" :
				"wrong \'.callback_result.num_properties\'?");

		return 1;
	}

	return exp_prop_to_find;
}

/*
 * Check each test case step if test case expected
 * data is set and match it with expected result.
 */
static bool match_data(struct step *step)
{
	struct test_data *data = tester_get_data();
	const struct step *exp;

	exp = queue_peek_head(data->steps);

	if (!exp) {
		/* Can occure while test passed already */
		tester_debug("Cannot get step to match");
		return false;
	}

	if (exp->action_result.status != step->action_result.status) {
		tester_debug("Action status don't match");
		return false;
	}

	if (exp->callback) {
		if (exp->callback != step->callback) {
			tester_debug("Callback type don't match");
			return false;
		}

		if (exp->callback_result.state !=
						step->callback_result.state) {
			tester_debug("Callback state don't match");
			return false;
		}

		if (exp->callback_result.properties &&
				verify_property(exp->callback_result.properties,
				exp->callback_result.num_properties,
				step->callback_result.properties,
				step->callback_result.num_properties)) {
			tester_debug("Gatt properties don't match");
			return false;
		}
	}

	return true;
}

static void init_test_steps(struct test_data *data)
{
	const struct test_case *test_steps = data->test_data;
	int i = 0;

	for (i = 0; i < test_steps->step_num; i++)
		queue_push_tail(data->steps, (void *) &(test_steps->step[i]));

	tester_print("tester: Number of test steps=%d",
						queue_length(data->steps));
}

/*
 * Each test case step should be verified, if match with
 * expected result tester should go to next test step.
 */
static void verify_step(struct step *step, queue_destroy_func_t cleanup_cb)
{
	struct test_data *data = tester_get_data();
	const struct test_case *test_steps = data->test_data;
	struct step *next_step;

	tester_debug("tester: STEP[%d] check",
			test_steps->step_num-queue_length(data->steps) + 1);

	if (step && !match_data(step)) {
		if (cleanup_cb)
			cleanup_cb(step);

		return;
	}

	queue_pop_head(data->steps);

	if (cleanup_cb)
		cleanup_cb(step);

	tester_debug("tester: STEP[%d] pass",
			test_steps->step_num-queue_length(data->steps));

	if (queue_isempty(data->steps)) {
		tester_print("tester: All steps done, passing");
		tester_test_passed();

		return;
	}

	/* goto next step action if declared in step */
	next_step = queue_peek_head(data->steps);

	if (next_step->action)
		next_step->action();
}

/*
 * NOTICE:
 * Its mandatory for callback to set proper step.callback value so that
 * step verification could pass and move to next test step
 */

static void free_properties(struct step *step)
{
	bt_property_t *properties = step->callback_result.properties;
	int num_properties = step->callback_result.num_properties;
	int i;

	for (i = 0; i < num_properties; i++)
		g_free(properties[i].val);

	g_free(properties);
}

static void destroy_callback_step(void *data)
{
	struct step *step = data;

	if (step->callback_result.properties)
		free_properties(step);

	g_free(step);
	g_atomic_int_dec_and_test(&scheduled_cbacks_num);
}

static gboolean verify_callback(gpointer user_data)
{
	struct step *step = user_data;

	/*
	 * TODO: This may call action from next step before callback data
	 * from previous step was freed.
	 */
	verify_step(step, destroy_callback_step);

	return FALSE;
}

static void schedule_callback_call(struct step *step)
{
	g_atomic_int_inc(&scheduled_cbacks_num);
	g_idle_add(verify_callback, step);
}

static void adapter_state_changed_cb(bt_state_t state)
{
	struct step *step = g_new0(struct step, 1);

	step->callback_result.state = state;
	step->callback = CB_BT_ADAPTER_STATE_CHANGED;

	schedule_callback_call(step);
}

static bt_property_t *copy_properties(int num_properties,
						bt_property_t *properties)
{
	int i;
	bt_property_t *props = g_new0(bt_property_t, num_properties);

	for (i = 0; i < num_properties; i++) {
		props[i].type = properties[i].type;
		props[i].len = properties[i].len;
		props[i].val = g_memdup(properties[i].val, properties[i].len);
	}

	return props;
}

static void adapter_properties_cb(bt_status_t status, int num_properties,
						bt_property_t *properties)
{
	struct step *step = g_new0(struct step, 1);

	step->callback_result.status = status;
	step->callback_result.num_properties = num_properties;
	step->callback_result.properties = copy_properties(num_properties,
								properties);
	step->callback = CB_BT_ADAPTER_PROPERTIES;

	schedule_callback_call(step);
}

static void discovery_state_changed_cb(bt_discovery_state_t state)
{
	struct step *step = g_new0(struct step, 1);

	step->callback = CB_BT_DISCOVERY_STATE_CHANGED;
	step->callback_result.state = state;

	schedule_callback_call(step);
}

static bt_callbacks_t bt_callbacks = {
	.size = sizeof(bt_callbacks),
	.adapter_state_changed_cb = adapter_state_changed_cb,
	.adapter_properties_cb = adapter_properties_cb,
	.remote_device_properties_cb = NULL,
	.device_found_cb = NULL,
	.discovery_state_changed_cb = discovery_state_changed_cb,
	.pin_request_cb = NULL,
	.ssp_request_cb = NULL,
	.bond_state_changed_cb = NULL,
	.acl_state_changed_cb = NULL,
	.thread_evt_cb = NULL,
	.dut_mode_recv_cb = NULL,
	.le_test_mode_cb = NULL
};

static bthh_callbacks_t bthh_callbacks = {
	.size = sizeof(bthh_callbacks),
	.connection_state_cb = NULL,
	.hid_info_cb = NULL,
	.protocol_mode_cb = NULL,
	.idle_time_cb = NULL,
	.get_report_cb = NULL,
	.virtual_unplug_cb = NULL
};

static const btgatt_client_callbacks_t btgatt_client_callbacks = {
	.register_client_cb = NULL,
	.scan_result_cb = NULL,
	.open_cb = NULL,
	.close_cb = NULL,
	.search_complete_cb = NULL,
	.search_result_cb = NULL,
	.get_characteristic_cb = NULL,
	.get_descriptor_cb = NULL,
	.get_included_service_cb = NULL,
	.register_for_notification_cb = NULL,
	.notify_cb = NULL,
	.read_characteristic_cb = NULL,
	.write_characteristic_cb = NULL,
	.read_descriptor_cb = NULL,
	.write_descriptor_cb = NULL,
	.execute_write_cb = NULL,
	.read_remote_rssi_cb = NULL,
	.listen_cb = NULL
};

static const btgatt_server_callbacks_t btgatt_server_callbacks = {
	.register_server_cb = NULL,
	.connection_cb = NULL,
	.service_added_cb = NULL,
	.included_service_added_cb = NULL,
	.characteristic_added_cb = NULL,
	.descriptor_added_cb = NULL,
	.service_started_cb = NULL,
	.service_stopped_cb = NULL,
	.service_deleted_cb = NULL,
	.request_read_cb = NULL,
	.request_write_cb = NULL,
	.request_exec_write_cb = NULL,
	.response_confirmation_cb = NULL
};

static const btgatt_callbacks_t btgatt_callbacks = {
	.size = sizeof(btgatt_callbacks),
	.client = &btgatt_client_callbacks,
	.server = &btgatt_server_callbacks
};

static bool setup_base(struct test_data *data)
{
	const hw_module_t *module;
	hw_device_t *device;
	int signal_fd[2];
	char buf[1024];
	pid_t pid;
	int len;
	int err;

	if (pipe(signal_fd))
		return false;

	pid = fork();

	if (pid < 0) {
		close(signal_fd[0]);
		close(signal_fd[1]);
		return false;
	}

	if (pid == 0) {
		if (!tester_use_debug())
			fclose(stderr);

		close(signal_fd[0]);
		emulator(signal_fd[1], data->mgmt_index);
		exit(0);
	}

	close(signal_fd[1]);
	data->bluetoothd_pid = pid;

	len = read(signal_fd[0], buf, sizeof(buf));
	if (len <= 0 || strcmp(buf, EMULATOR_SIGNAL)) {
		close(signal_fd[0]);
		return false;
	}

	close(signal_fd[0]);

	err = hw_get_module(BT_HARDWARE_MODULE_ID, &module);
	if (err)
		return false;

	err = module->methods->open(module, BT_HARDWARE_MODULE_ID, &device);
	if (err)
		return false;

	data->device = device;

	data->if_bluetooth = ((bluetooth_device_t *)
					device)->get_bluetooth_interface();
	if (!data->if_bluetooth)
		return false;

	if (!(data->steps = queue_new()))
		return false;


	return true;
}

static void setup(const void *test_data)
{
	struct test_data *data = tester_get_data();
	bt_status_t status;

	if (!setup_base(data)) {
		tester_setup_failed();
		return;
	}

	status = data->if_bluetooth->init(&bt_callbacks);
	if (status != BT_STATUS_SUCCESS) {
		data->if_bluetooth = NULL;
		tester_setup_failed();
		return;
	}

	tester_setup_complete();
}

static void setup_socket(const void *test_data)
{
	struct test_data *data = tester_get_data();
	bt_status_t status;
	const void *sock;

	if (!setup_base(data)) {
		tester_setup_failed();
		return;
	}

	status = data->if_bluetooth->init(&bt_callbacks);
	if (status != BT_STATUS_SUCCESS) {
		data->if_bluetooth = NULL;
		tester_setup_failed();
		return;
	}

	sock = data->if_bluetooth->get_profile_interface(BT_PROFILE_SOCKETS_ID);
	if (!sock) {
		tester_setup_failed();
		return;
	}

	data->if_sock = sock;

	tester_setup_complete();
}

static void setup_hidhost(const void *test_data)
{
	struct test_data *data = tester_get_data();
	bt_status_t status;
	const void *hid;

	if (!setup_base(data)) {
		tester_setup_failed();
		return;
	}

	status = data->if_bluetooth->init(&bt_callbacks);
	if (status != BT_STATUS_SUCCESS) {
		data->if_bluetooth = NULL;
		tester_setup_failed();
		return;
	}

	hid = data->if_bluetooth->get_profile_interface(BT_PROFILE_HIDHOST_ID);
	if (!hid) {
		tester_setup_failed();
		return;
	}

	data->if_hid = hid;

	status = data->if_hid->init(&bthh_callbacks);
	if (status != BT_STATUS_SUCCESS) {
		data->if_hid = NULL;
		tester_setup_failed();
		return;
	}

	tester_setup_complete();
}

static void setup_gatt(const void *test_data)
{
	struct test_data *data = tester_get_data();
	bt_status_t status;
	const void *gatt;

	if (!setup_base(data)) {
		tester_setup_failed();
		return;
	}

	status = data->if_bluetooth->init(&bt_callbacks);
	if (status != BT_STATUS_SUCCESS) {
		data->if_bluetooth = NULL;
		tester_setup_failed();
		return;
	}

	gatt = data->if_bluetooth->get_profile_interface(BT_PROFILE_GATT_ID);
	if (!gatt) {
		tester_setup_failed();
		return;
	}

	data->if_gatt = gatt;

	status = data->if_gatt->init(&btgatt_callbacks);
	if (status != BT_STATUS_SUCCESS) {
		data->if_gatt = NULL;

		tester_setup_failed();
		return;
	}

	tester_setup_complete();
}

static void teardown(const void *test_data)
{
	struct test_data *data = tester_get_data();

	queue_destroy(data->steps, NULL);
	data->steps = NULL;

	if (data->if_gatt) {
		data->if_gatt->cleanup();
		data->if_gatt = NULL;
	}

	if (data->if_hid) {
		data->if_hid->cleanup();
		data->if_hid = NULL;
	}

	if (data->if_bluetooth) {
		data->if_bluetooth->cleanup();
		data->if_bluetooth = NULL;
	}

	data->device->close(data->device);

	if (!data->bluetoothd_pid)
		tester_teardown_complete();
}

void dummy_action(void)
{
	struct step step;

	memset(&step, 0, sizeof(step));
	step.action = dummy_action;

	verify_step(&step, NULL);
}

void bluetooth_enable_action(void)
{
	struct test_data *data = tester_get_data();
	struct step step;

	memset(&step, 0, sizeof(step));
	step.action_result.status = data->if_bluetooth->enable();

	verify_step(&step, NULL);
}

void bluetooth_disable_action(void)
{
	struct test_data *data = tester_get_data();
	struct step step;

	memset(&step, 0, sizeof(step));
	step.action_result.status = data->if_bluetooth->disable();

	verify_step(&step, NULL);
}

void bt_set_property_action(void)
{
	struct test_data *data = tester_get_data();
	struct step step;
	struct step *current_data_step = queue_peek_head(data->steps);
	bt_property_t *prop;

	if (!current_data_step->set_data) {
		tester_debug("BT property not set for step");
		tester_test_failed();
		return;
	}

	prop = (bt_property_t *)current_data_step->set_data;

	memset(&step, 0, sizeof(step));
	step.action_result.status = data->if_bluetooth->set_adapter_property(
									prop);

	verify_step(&step, NULL);
}

void bt_get_property_action(void)
{
	struct test_data *data = tester_get_data();
	struct step step;
	struct step *current_data_step = queue_peek_head(data->steps);
	bt_property_t *prop;

	if (!current_data_step->set_data) {
		tester_debug("BT property to get not defined");
		tester_test_failed();
		return;
	}

	prop = (bt_property_t *)current_data_step->set_data;

	memset(&step, 0, sizeof(step));
	step.action_result.status = data->if_bluetooth->get_adapter_property(
								prop->type);

	verify_step(&step, NULL);
}

void bt_start_discovery_action(void)
{
	struct test_data *data = tester_get_data();
	struct step step;

	step.action_result.status = data->if_bluetooth->start_discovery();

	verify_step(&step, NULL);
}

void bt_cancel_discovery_action(void)
{
	struct test_data *data = tester_get_data();
	struct step step;

	memset(&step, 0, sizeof(step));
	step.action_result.status = data->if_bluetooth->cancel_discovery();

	verify_step(&step, NULL);
}

static void generic_test_function(const void *test_data)
{
	struct test_data *data = tester_get_data();
	struct step *first_step;

	init_test_steps(data);

	/* first step action */
	first_step = queue_peek_head(data->steps);
	if (!first_step->action) {
		tester_print("tester: No initial action declared");
		tester_test_failed();
		return;
	}
	first_step->action();
}

#define test_bredr(data, test_setup, test, test_teardown) \
	do { \
		struct test_data *user; \
		user = g_malloc0(sizeof(struct test_data)); \
		if (!user) \
			break; \
		user->hciemu_type = HCIEMU_TYPE_BREDR; \
		user->test_data = data; \
		tester_add_full(data->title, data, test_pre_setup, \
					test_setup, test, test_teardown, \
					test_post_teardown, 3, user, g_free); \
	} while (0)

#define test_bredrle(data, test_setup, test, test_teardown) \
	do { \
		struct test_data *user; \
		user = g_malloc0(sizeof(struct test_data)); \
		if (!user) \
			break; \
		user->hciemu_type = HCIEMU_TYPE_BREDRLE; \
		user->test_data = data; \
		tester_add_full(data->title, data, test_pre_setup, \
					test_setup, test, test_teardown, \
					test_post_teardown, 3, user, g_free); \
	} while (0)

static void tester_testcases_cleanup(void)
{
	remove_bluetooth_tests();
	remove_socket_tests();
}

static void add_bluetooth_tests(void *data, void *user_data)
{
	struct test_case *tc = data;

	test_bredrle(tc, setup, generic_test_function, teardown);
}

static void add_socket_tests(void *data, void *user_data)
{
	struct test_case *tc = data;

	test_bredrle(tc, setup_socket, generic_test_function, teardown);
}

static void add_hidhost_tests(void *data, void *user_data)
{
	struct test_case *tc = data;

	test_bredrle(tc, setup_hidhost, generic_test_function, teardown);
}

static void add_gatt_tests(void *data, void *user_data)
{
	struct test_case *tc = data;

	test_bredrle(tc, setup_gatt, generic_test_function, teardown);
}

int main(int argc, char *argv[])
{
	snprintf(exec_dir, sizeof(exec_dir), "%s", dirname(argv[0]));

	tester_init(&argc, &argv);

	queue_foreach(get_bluetooth_tests(), add_bluetooth_tests, NULL);
	queue_foreach(get_socket_tests(), add_socket_tests, NULL);
	queue_foreach(get_hidhost_tests(), add_hidhost_tests, NULL);
	queue_foreach(get_gatt_tests(), add_gatt_tests, NULL);

	if (tester_run())
		return 1;

	tester_testcases_cleanup();

	return 0;
}
