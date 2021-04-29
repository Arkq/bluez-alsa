/*
 * BlueALSA - bluez.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluez.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "a2dp.h"
#include "a2dp-codecs.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "ba-transport.h"
#include "bluealsa.h"
#include "bluealsa-dbus.h"
#include "bluez-iface.h"
#include "codec-sbc.h"
#include "dbus.h"
#include "hci.h"
#include "sco.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/log.h"

/* Compatibility patch for glib < 2.42. */
#ifndef G_DBUS_ERROR_UNKNOWN_OBJECT
# define G_DBUS_ERROR_UNKNOWN_OBJECT G_DBUS_ERROR_FAILED
#endif

/**
 * Data associated with registered D-Bus object. */
struct bluez_dbus_object_data {
	/* D-Bus object registration ID */
	unsigned int id;
	/* D-Bus object registration path */
	char path[64];
	/* associated adapter */
	int hci_dev_id;
	const struct a2dp_codec *codec;
	struct ba_transport_type ttype;
	/* determine whether object is registered in BlueZ */
	bool registered;
	/* determine whether object is used */
	bool connected;
	/* memory self-management */
	int ref_count;
};

/**
 * BlueALSA copy of BlueZ adapter data. */
struct bluez_adapter {
	/* reference to the adapter structure */
	struct ba_adapter *adapter;
	/* array of end-points for connected devices */
	GHashTable *device_sep_map;
};

static pthread_mutex_t bluez_mutex = PTHREAD_MUTEX_INITIALIZER;
static GHashTable *dbus_object_data_map = NULL;
static struct bluez_adapter bluez_adapters[HCI_MAX_DEV] = { NULL };

#define bluez_adapters_device_lookup(hci_dev_id, addr) \
	g_hash_table_lookup(bluez_adapters[hci_dev_id].device_sep_map, addr)
#define bluez_adapters_device_get_sep(seps, i) \
	g_array_index(seps, struct a2dp_sep, i)

static void bluez_dbus_object_data_unref(
		struct bluez_dbus_object_data *obj) {
	if (--obj->ref_count != 0)
		return;
	free(obj);
}

/**
 * Check whether D-Bus adapter matches our configuration. */
static bool bluez_match_dbus_adapter(
		const char *adapter_path,
		const char *adapter_address) {

	/* if configuration is empty, match everything */
	if (config.hci_filter->len == 0)
		return true;

	/* get the last component of the path */
	if ((adapter_path = strrchr(adapter_path, '/')) == NULL)
		return false;

	adapter_path++;

	size_t i;
	for (i = 0; i < config.hci_filter->len; i++)
		if (strcasecmp(adapter_path, g_array_index(config.hci_filter, char *, i)) == 0 ||
				strcasecmp(adapter_address, g_array_index(config.hci_filter, char *, i)) == 0)
			return true;

	return false;
}

/**
 * Get transport state from BlueZ state string. */
static enum bluez_a2dp_transport_state bluez_a2dp_transport_state_from_string(
		const char *state) {
	if (strcmp(state, BLUEZ_TRANSPORT_STATE_IDLE) == 0)
		return BLUEZ_A2DP_TRANSPORT_STATE_IDLE;
	if (strcmp(state, BLUEZ_TRANSPORT_STATE_PENDING) == 0)
		return BLUEZ_A2DP_TRANSPORT_STATE_PENDING;
	if (strcmp(state, BLUEZ_TRANSPORT_STATE_ACTIVE) == 0)
		return BLUEZ_A2DP_TRANSPORT_STATE_ACTIVE;
	warn("Invalid A2DP transport state: %s", state);
	return BLUEZ_A2DP_TRANSPORT_STATE_IDLE;
}

static void bluez_endpoint_select_configuration(GDBusMethodInvocation *inv) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;
	const struct a2dp_codec *codec = dbus_obj->codec;

	const void *data;
	void *capabilities;
	size_t size = 0;

	params = g_variant_get_child_value(params, 0);
	data = g_variant_get_fixed_array(params, &size, sizeof(char));
	capabilities = g_memdup(data, size);
	g_variant_unref(params);

	if (a2dp_select_configuration(codec, capabilities, size) == -1)
		goto fail;

	GVariantBuilder caps;
	size_t i;

	g_variant_builder_init(&caps, G_VARIANT_TYPE("ay"));
	for (i = 0; i < size; i++)
		g_variant_builder_add(&caps, "y", ((char *)capabilities)[i]);

	g_dbus_method_invocation_return_value(inv, g_variant_new("(ay)", &caps));
	g_variant_builder_clear(&caps);

	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Invalid capabilities");

final:
	g_free(capabilities);
}

static void bluez_register_a2dp_all(struct ba_adapter *adapter);

static void bluez_endpoint_set_configuration(GDBusMethodInvocation *inv) {

	const char *sender = g_dbus_method_invocation_get_sender(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;
	const struct a2dp_codec *codec = dbus_obj->codec;
	const uint16_t codec_id = codec->codec_id;

	struct ba_adapter *a = NULL;
	struct ba_transport *t = NULL;
	struct ba_device *d = NULL;

	enum bluez_a2dp_transport_state state = 0xFFFF;
	char *device_path = NULL;
	void *configuration = NULL;
	uint16_t volume = 127;
	uint16_t delay = 150;

	const char *transport_path;
	GVariantIter *properties;
	GVariant *value = NULL;
	const char *property;

	g_variant_get(params, "(&oa{sv})", &transport_path, &properties);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {

		if (strcmp(property, "Device") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_OBJECT_PATH, property)) {
			g_free(device_path);
			device_path = g_variant_dup_string(value, NULL);
		}
		else if (strcmp(property, "UUID") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
		}
		else if (strcmp(property, "Codec") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTE, property)) {

			if ((codec_id & 0xFF) != g_variant_get_byte(value)) {
				error("Invalid configuration: %s", "Codec mismatch");
				goto fail;
			}

		}
		else if (strcmp(property, "Configuration") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTESTRING, property)) {

			size_t size = 0;
			const void *data = g_variant_get_fixed_array(value, &size, sizeof(char));

			uint32_t rv;
			if ((rv = a2dp_check_configuration(codec, data, size)) != A2DP_CHECK_OK) {
				error("Invalid configuration: %s: %#x", "Invalid configuration blob", rv);
				goto fail;
			}

			g_free(configuration);
			configuration = g_memdup(data, size);

		}
		else if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
			state = bluez_a2dp_transport_state_from_string(g_variant_get_string(value, NULL));
		}
		else if (strcmp(property, "Delay") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			delay = g_variant_get_uint16(value);
		}
		else if (strcmp(property, "Volume") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			/* received volume is in range [0, 127] */
			volume = g_variant_get_uint16(value);
		}

		g_variant_unref(value);
		value = NULL;
	}

	if (state == 0xFFFF) {
		error("Invalid configuration: %s", "Missing state");
		goto fail;
	}

	if ((a = ba_adapter_lookup(dbus_obj->hci_dev_id)) == NULL) {
		error("Couldn't lookup adapter: hci%d: %s", dbus_obj->hci_dev_id, strerror(errno));
		goto fail;
	}

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL &&
			(d = ba_device_new(a, &addr)) == NULL) {
		error("Couldn't create new device: %s", device_path);
		goto fail;
	}

	if (d->seps == NULL)
		d->seps = bluez_adapters_device_lookup(a->hci.dev_id, &addr);

	if (ba_transport_lookup(d, transport_path) != NULL) {
		error("Transport already configured: %s", transport_path);
		goto fail;
	}

	if ((t = ba_transport_new_a2dp(d, dbus_obj->ttype,
					sender, transport_path, codec, configuration)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	/* Skip volume level initialization in case of A2DP Source
	 * profile and software volume control. */
	if (!(t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE &&
				t->a2dp.pcm.soft_volume)) {
		int level = ba_transport_pcm_volume_bt_to_level(&t->a2dp.pcm, volume);
		t->a2dp.pcm.volume[0].level = level;
		t->a2dp.pcm.volume[1].level = level;
	}

	t->a2dp.bluez_dbus_sep_path = dbus_obj->path;
	t->a2dp.delay = delay;

	debug("%s configured for device %s",
			ba_transport_type_to_string(t->type),
			batostr_(&d->addr));
	debug("Configuration: channels: %u, sampling: %u",
			t->a2dp.pcm.channels, t->a2dp.pcm.sampling);

	ba_transport_set_a2dp_state(t, state);
	dbus_obj->connected = true;

	g_dbus_method_invocation_return_value(inv, NULL);
	bluez_register_a2dp_all(a);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to set configuration");

final:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
	g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
	g_free(device_path);
	g_free(configuration);
}

static void bluez_endpoint_clear_configuration(GDBusMethodInvocation *inv) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	debug("Disconnecting media endpoint: %s", dbus_obj->path);
	dbus_obj->connected = false;

	const char *transport_path;
	g_variant_get(params, "(&o)", &transport_path);

	if ((a = ba_adapter_lookup(dbus_obj->hci_dev_id)) == NULL)
		goto fail;

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(transport_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL)
		goto fail;

	if ((t = ba_transport_lookup(d, transport_path)) != NULL)
		ba_transport_destroy(t);

fail:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	g_object_unref(inv);
}

static void bluez_endpoint_release(GDBusMethodInvocation *inv) {

	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;

	debug("Releasing media endpoint: %s", dbus_obj->path);
	dbus_obj->connected = false;
	dbus_obj->registered = false;

	g_object_unref(inv);
}

static void bluez_endpoint_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)params;
	(void)userdata;

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "SelectConfiguration",
			.handler = bluez_endpoint_select_configuration },
		{ .method = "SetConfiguration",
			.handler = bluez_endpoint_set_configuration },
		{ .method = "ClearConfiguration",
			.handler = bluez_endpoint_clear_configuration },
		{ .method = "Release",
			.handler = bluez_endpoint_release },
		{ NULL },
	};

	if (!g_dbus_dispatch_method_call(dispatchers, sender, path, interface, method, invocation))
		error("Couldn't dispatch D-Bus method call: %s.%s()", interface, method);

}

/**
 * Register media endpoint in BlueZ. */
static int bluez_register_media_endpoint(
		const struct ba_adapter *adapter,
		const struct bluez_dbus_object_data *dbus_obj,
		const char *uuid,
		GError **error) {

	const struct a2dp_codec *codec = dbus_obj->codec;
	GDBusMessage *msg = NULL, *rep = NULL;
	int ret = 0;
	size_t i;

	debug("Registering media endpoint: %s", dbus_obj->path);

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, adapter->bluez_dbus_path,
			BLUEZ_IFACE_MEDIA, "RegisterEndpoint");

	GVariantBuilder caps;
	GVariantBuilder properties;

	g_variant_builder_init(&caps, G_VARIANT_TYPE("ay"));
	g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));

	for (i = 0; i < codec->capabilities_size; i++)
		g_variant_builder_add(&caps, "y", ((char *)codec->capabilities)[i]);

	g_variant_builder_add(&properties, "{sv}", "UUID", g_variant_new_string(uuid));
	g_variant_builder_add(&properties, "{sv}", "DelayReporting", g_variant_new_boolean(TRUE));
	g_variant_builder_add(&properties, "{sv}", "Codec", g_variant_new_byte(codec->codec_id));
	g_variant_builder_add(&properties, "{sv}", "Capabilities", g_variant_builder_end(&caps));

	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", dbus_obj->path, &properties));
	g_variant_builder_clear(&properties);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return ret;
}

/**
 * Register A2DP endpoint. */
static void bluez_register_a2dp(
		const struct ba_adapter *adapter,
		const struct a2dp_codec *codec,
		const char *uuid) {

	static const GDBusInterfaceVTable vtable = {
		.method_call = bluez_endpoint_method_call,
	};

	struct ba_transport_type ttype = {
		.profile = codec->dir == A2DP_SOURCE ?
			BA_TRANSPORT_PROFILE_A2DP_SOURCE : BA_TRANSPORT_PROFILE_A2DP_SINK,
		.codec = codec->codec_id,
	};

	int registered = 0;
	int connected = 0;

	pthread_mutex_lock(&bluez_mutex);

	for (;;) {

		struct bluez_dbus_object_data *dbus_obj;
		GError *err = NULL;

		char path[sizeof(dbus_obj->path)];
		snprintf(path, sizeof(path), "/org/bluez/%s%s/%d", adapter->hci.name,
				g_dbus_transport_type_to_bluez_object_path(ttype), ++registered);

		if ((dbus_obj = g_hash_table_lookup(dbus_object_data_map, path)) == NULL) {

			/* End the registration loop if all previously created media
			 * endpoints are registered in BlueZ and we've got at least N
			 * not connected endpoints. */
			if (registered > connected + 2)
				break;

			debug("Creating media endpoint object: %s", path);

			if ((dbus_obj = calloc(1, sizeof(*dbus_obj))) == NULL) {
				warn("Couldn't register media endpoint: %s", strerror(errno));
				goto fail;
			}

			strncpy(dbus_obj->path, path, sizeof(dbus_obj->path));
			dbus_obj->hci_dev_id = adapter->hci.dev_id;
			dbus_obj->codec = codec;
			dbus_obj->ttype = ttype;
			dbus_obj->ref_count = 2;

			if ((dbus_obj->id = g_dbus_connection_register_object(config.dbus,
							path, (GDBusInterfaceInfo *)&bluez_iface_endpoint, &vtable,
							dbus_obj, (GDestroyNotify)bluez_dbus_object_data_unref, &err)) == 0) {
				free(dbus_obj);
				goto fail;
			}

			g_hash_table_insert(dbus_object_data_map, dbus_obj->path, dbus_obj);

		}

		if (!dbus_obj->registered) {
			if (bluez_register_media_endpoint(adapter, dbus_obj, uuid, &err) == -1)
				goto fail;
			dbus_obj->registered = true;
		}

		if (dbus_obj->connected)
			connected++;

		continue;

fail:
		if (err != NULL) {
			warn("Couldn't register media endpoint: %s", err->message);
			g_error_free(err);
			if (ttype.profile == BA_TRANSPORT_PROFILE_A2DP_SOURCE)
				config.enable.a2dp_source = false;
			else
				config.enable.a2dp_sink = false;
		}
	}

	pthread_mutex_unlock(&bluez_mutex);

}

/**
 * Register A2DP endpoints. */
static void bluez_register_a2dp_all(struct ba_adapter *adapter) {

	const struct a2dp_codec **cc = a2dp_codecs;

	while (*cc != NULL) {
		const struct a2dp_codec *c = *cc++;
		switch (c->dir) {
		case A2DP_SOURCE:
			if (config.enable.a2dp_source)
				bluez_register_a2dp(adapter, c, BLUETOOTH_UUID_A2DP_SOURCE);
			break;
		case A2DP_SINK:
			if (config.enable.a2dp_sink)
				bluez_register_a2dp(adapter, c, BLUETOOTH_UUID_A2DP_SINK);
			break;
		}
	}

}

static void bluez_profile_new_connection(GDBusMethodInvocation *inv) {

	GDBusMessage *msg = g_dbus_method_invocation_get_message(inv);
	const char *sender = g_dbus_method_invocation_get_sender(inv);
	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *device_path;
	GVariantIter *properties;
	GUnixFDList *fd_list;
	GError *err = NULL;
	int fd = -1;

	g_variant_get(params, "(&oha{sv})", &device_path, &fd, &properties);

	fd_list = g_dbus_message_get_unix_fd_list(msg);
	if ((fd = g_unix_fd_list_get(fd_list, 0, &err)) == -1) {
		error("Couldn't obtain RFCOMM socket: %s", err->message);
		goto fail;
	}

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(device_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL) {
		error("Couldn't lookup adapter: hci%d: %s", hci_dev_id, strerror(errno));
		goto fail;
	}

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL &&
			(d = ba_device_new(a, &addr)) == NULL) {
		error("Couldn't create new device: %s", strerror(errno));
		goto fail;
	}

	if ((t = ba_transport_new_sco(d, dbus_obj->ttype,
					sender, device_path, fd)) == NULL) {
		error("Couldn't create new transport: %s", strerror(errno));
		goto fail;
	}

	if (sco_setup_connection_dispatcher(a) == -1) {
		error("Couldn't setup SCO connection dispatcher: %s", strerror(errno));
		goto fail;
	}

	debug("%s configured for device %s",
			ba_transport_type_to_string(t->type),
			batostr_(&d->addr));

	ba_transport_start(t);
	dbus_obj->connected = true;

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_INVALID_ARGS, "Unable to connect profile");
	if (fd != -1)
		close(fd);

final:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
	g_variant_iter_free(properties);
	if (err != NULL)
		g_error_free(err);
}

static void bluez_profile_request_disconnection(GDBusMethodInvocation *inv) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;

	debug("Disconnecting hands-free profile: %s", dbus_obj->path);
	dbus_obj->connected = false;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	const char *device_path;
	g_variant_get(params, "(&o)", &device_path);

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(device_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL)
		goto fail;

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(device_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL)
		goto fail;

	if ((t = ba_transport_lookup(d, device_path)) != NULL)
		ba_transport_destroy(t);

fail:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	g_object_unref(inv);
}

static void bluez_profile_release(GDBusMethodInvocation *inv) {

	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct bluez_dbus_object_data *dbus_obj = userdata;

	debug("Releasing hands-free profile: %s", dbus_obj->path);
	dbus_obj->connected = false;
	dbus_obj->registered = false;

	g_object_unref(inv);
}

static void bluez_profile_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)params;
	(void)userdata;

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "NewConnection",
			.handler = bluez_profile_new_connection },
		{ .method = "RequestDisconnection",
			.handler = bluez_profile_request_disconnection },
		{ .method = "Release",
			.handler = bluez_profile_release },
		{ NULL },
	};

	if (!g_dbus_dispatch_method_call(dispatchers, sender, path, interface, method, invocation))
		error("Couldn't dispatch D-Bus method call: %s.%s()", interface, method);

}

/**
 * Register hands-free profile in BlueZ. */
static int bluez_register_profile(
		const struct bluez_dbus_object_data *dbus_obj,
		const char *uuid,
		uint16_t version,
		uint16_t features,
		GError **error) {

	GDBusMessage *msg = NULL, *rep = NULL;
	int ret = 0;

	debug("Registering hands-free profile: %s", dbus_obj->path);

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_IFACE_PROFILE_MANAGER, "RegisterProfile");

	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
	if (version)
		g_variant_builder_add(&options, "{sv}", "Version", g_variant_new_uint16(version));
	if (features)
		g_variant_builder_add(&options, "{sv}", "Features", g_variant_new_uint16(features));

	g_dbus_message_set_body(msg, g_variant_new("(osa{sv})", dbus_obj->path, uuid, &options));
	g_variant_builder_clear(&options);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	goto final;

fail:
	ret = -1;

final:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);

	return ret;
}

/**
 * Register Bluetooth Hands-Free Audio Profile. */
static void bluez_register_hfp(
		const char *uuid,
		uint32_t profile,
		uint16_t version,
		uint16_t features) {

	static const GDBusInterfaceVTable vtable = {
		.method_call = bluez_profile_method_call,
	};

	struct ba_transport_type ttype = {
		.profile = profile,
	};

	pthread_mutex_lock(&bluez_mutex);

	struct bluez_dbus_object_data *dbus_obj;
	GError *err = NULL;

	char path[sizeof(dbus_obj->path)];
	snprintf(path, sizeof(path), "/org/bluez%s",
			g_dbus_transport_type_to_bluez_object_path(ttype));

	if ((dbus_obj = g_hash_table_lookup(dbus_object_data_map, path)) == NULL) {

		debug("Creating hands-free profile object: %s", path);

		if ((dbus_obj = calloc(1, sizeof(*dbus_obj))) == NULL) {
			warn("Couldn't register hands-free profile: %s", strerror(errno));
			goto fail;
		}

		strncpy(dbus_obj->path, path, sizeof(dbus_obj->path));
		dbus_obj->hci_dev_id = -1;
		dbus_obj->ttype = ttype;
		dbus_obj->ref_count = 2;

		if ((dbus_obj->id = g_dbus_connection_register_object(config.dbus,
						path, (GDBusInterfaceInfo *)&bluez_iface_profile, &vtable,
						dbus_obj, (GDestroyNotify)bluez_dbus_object_data_unref, &err)) == 0) {
			free(dbus_obj);
			goto fail;
		}

		g_hash_table_insert(dbus_object_data_map, dbus_obj->path, dbus_obj);

	}

	if (!dbus_obj->registered) {
		if (bluez_register_profile(dbus_obj, uuid, version, features, &err) == -1)
			goto fail;
		dbus_obj->registered = true;
	}

fail:

	if (err != NULL) {
		warn("Couldn't register hands-free profile: %s", err->message);
		g_error_free(err);
		switch (ttype.profile) {
		case BA_TRANSPORT_PROFILE_HSP_HS:
			config.enable.hsp_hs = false;
			break;
		case BA_TRANSPORT_PROFILE_HSP_AG:
			config.enable.hsp_ag = false;
			break;
		case BA_TRANSPORT_PROFILE_HFP_HF:
			config.enable.hfp_hf = false;
			break;
		case BA_TRANSPORT_PROFILE_HFP_AG:
			config.enable.hfp_ag = false;
			break;
		}
	}

	pthread_mutex_unlock(&bluez_mutex);

}

/**
 * Register Bluetooth Hands-Free Audio Profiles.
 *
 * This function also registers deprecated HSP profile. Profiles registration
 * is controlled by the global configuration structure - if none is enabled,
 * this function will do nothing. */
static void bluez_register_hfp_all(void) {
	if (config.enable.hsp_hs)
		bluez_register_hfp(BLUETOOTH_UUID_HSP_HS, BA_TRANSPORT_PROFILE_HSP_HS,
				0x0102 /* HSP 1.2 */, 0x1 /* remote audio volume control */);
	if (config.enable.hsp_ag)
		bluez_register_hfp(BLUETOOTH_UUID_HSP_AG, BA_TRANSPORT_PROFILE_HSP_AG,
				0x0102 /* HSP 1.2 */, 0x0);
	if (config.enable.hfp_hf)
		bluez_register_hfp(BLUETOOTH_UUID_HFP_HF, BA_TRANSPORT_PROFILE_HFP_HF,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_hf);
	if (config.enable.hfp_ag)
		bluez_register_hfp(BLUETOOTH_UUID_HFP_AG, BA_TRANSPORT_PROFILE_HFP_AG,
				0x0107 /* HFP 1.7 */, config.hfp.features_sdp_ag);
}

static void bluez_sep_array_free(GArray *seps) {
	size_t i;
	for (i = 0; i < seps->len; i++) {
		g_free(bluez_adapters_device_get_sep(seps, i).capabilities);
		g_free(bluez_adapters_device_get_sep(seps, i).configuration);
	}
	g_array_unref(seps);
}

/**
 * Register to the BlueZ service. */
void bluez_register(void) {

	if (dbus_object_data_map == NULL)
		dbus_object_data_map = g_hash_table_new_full(g_str_hash, g_str_equal,
				NULL, (GDestroyNotify)bluez_dbus_object_data_unref);

	GError *err = NULL;
	GVariantIter *objects = NULL;
	if ((objects = g_dbus_get_managed_objects(config.dbus, BLUEZ_SERVICE, "/", &err)) == NULL) {
		warn("Couldn't get managed objects: %s", err->message);
		g_error_free(err);
		return;
	}

	bool adapters[HCI_MAX_DEV] = { 0 };

	GVariantIter *interfaces;
	GVariantIter *properties;
	GVariant *value;
	const char *object_path;
	const char *interface;
	const char *property;

	while (g_variant_iter_next(objects, "{&oa{sa{sv}}}", &object_path, &interfaces)) {
		while (g_variant_iter_next(interfaces, "{&sa{sv}}", &interface, &properties)) {
			if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0)
				while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
					if (strcmp(property, "Address") == 0 &&
							bluez_match_dbus_adapter(object_path, g_variant_get_string(value, NULL)))
						/* mark adapter as valid for registration */
						adapters[g_dbus_bluez_object_path_to_hci_dev_id(object_path)] = true;
					g_variant_unref(value);
				}
			g_variant_iter_free(properties);
		}
		g_variant_iter_free(interfaces);
	}
	g_variant_iter_free(objects);

	size_t i;
	struct ba_adapter *a;
	for (i = 0; i < ARRAYSIZE(adapters); i++)
		if (adapters[i] &&
				(a = ba_adapter_new(i)) != NULL) {
			bluez_adapters[a->hci.dev_id].adapter = a;
			bluez_adapters[a->hci.dev_id].device_sep_map = g_hash_table_new_full(
					g_bdaddr_hash, g_bdaddr_equal, g_free, (GDestroyNotify)bluez_sep_array_free);
			bluez_register_a2dp_all(a);
		}

	/* HFP has to be registered globally */
	bluez_register_hfp_all();

}

static void bluez_signal_interfaces_added(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface_, signal);
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface_;
	(void)signal;
	(void)userdata;

	GVariantIter *interfaces;
	GVariantIter *properties;
	GVariant *value;
	const char *object_path;
	const char *interface;
	const char *property;

	int hci_dev_id = -1;
	struct a2dp_sep sep = {
		.dir = A2DP_SOURCE,
		.codec_id = 0xFFFF,
	};

	g_variant_get(params, "(&oa{sa{sv}})", &object_path, &interfaces);
	while (g_variant_iter_next(interfaces, "{&sa{sv}}", &interface, &properties)) {
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0)
			while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
				if (strcmp(property, "Address") == 0 &&
						bluez_match_dbus_adapter(object_path, g_variant_get_string(value, NULL)))
					hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);
				g_variant_unref(value);
			}
		else if (strcmp(interface, BLUEZ_IFACE_MEDIA_ENDPOINT) == 0)
			while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
				if (strcmp(property, "UUID") == 0) {
					const char *uuid = g_variant_get_string(value, NULL);
					if (strcasecmp(uuid, BLUETOOTH_UUID_A2DP_SINK) == 0)
						sep.dir = A2DP_SINK;
				}
				else if (strcmp(property, "Codec") == 0)
					sep.codec_id = g_variant_get_byte(value);
				else if (strcmp(property, "Capabilities") == 0) {

					const void *data = g_variant_get_fixed_array(value,
							&sep.capabilities_size, sizeof(char));

					g_free(sep.capabilities);
					sep.capabilities = g_memdup(data, sep.capabilities_size);

				}
				g_variant_unref(value);
			}
		g_variant_iter_free(properties);
	}
	g_variant_iter_free(interfaces);

	struct ba_adapter *a;
	if (hci_dev_id != -1 &&
			(a = ba_adapter_new(hci_dev_id)) != NULL) {
		bluez_adapters[a->hci.dev_id].adapter = a;
		bluez_adapters[a->hci.dev_id].device_sep_map = g_hash_table_new_full(
				g_bdaddr_hash, g_bdaddr_equal, g_free, (GDestroyNotify)bluez_sep_array_free);
		bluez_register_a2dp_all(a);
	}

	/* HFP has to be registered globally */
	if (strcmp(object_path, "/org/bluez") == 0)
		bluez_register_hfp_all();

	if (sep.codec_id != 0xFFFF) {

		bdaddr_t addr;
		g_dbus_bluez_object_path_to_bdaddr(object_path, &addr);
		int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);

		GArray *seps;
		if ((seps = bluez_adapters_device_lookup(hci_dev_id, &addr)) == NULL)
			g_hash_table_insert(bluez_adapters[hci_dev_id].device_sep_map,
					g_memdup(&addr, sizeof(addr)), seps = g_array_new(FALSE, FALSE, sizeof(sep)));

		strncpy(sep.bluez_dbus_path, object_path, sizeof(sep.bluez_dbus_path) - 1);
		if (sep.codec_id == A2DP_CODEC_VENDOR)
			sep.codec_id = a2dp_get_vendor_codec_id(sep.capabilities, sep.capabilities_size);
		sep.configuration = g_malloc(sep.capabilities_size);

		debug("Adding new Stream End-Point: %s: %s", batostr_(&addr),
				ba_transport_codecs_a2dp_to_string(sep.codec_id));
		g_array_append_val(seps, sep);

	}

}

static void bluez_signal_interfaces_removed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	debug("Signal: %s.%s()", interface_, signal);
	(void)sender;
	(void)path;
	(void)interface_;
	(void)signal;
	(void)userdata;

	GVariantIter *interfaces;
	const char *object_path;
	const char *interface;
	int hci_dev_id;

	g_variant_get(params, "(&oas)", &object_path, &interfaces);
	hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(object_path);

	pthread_mutex_lock(&bluez_mutex);

	while (g_variant_iter_next(interfaces, "&s", &interface))
		if (strcmp(interface, BLUEZ_IFACE_ADAPTER) == 0) {

			GHashTableIter iter;
			struct bluez_dbus_object_data *dbus_obj;
			g_hash_table_iter_init(&iter, dbus_object_data_map);
			while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj))
				if (dbus_obj->hci_dev_id == hci_dev_id) {
					g_dbus_connection_unregister_object(conn, dbus_obj->id);
					g_hash_table_iter_remove(&iter);
				}

			if (bluez_adapters[hci_dev_id].adapter != NULL) {
				ba_adapter_destroy(bluez_adapters[hci_dev_id].adapter);
				bluez_adapters[hci_dev_id].adapter = NULL;
				g_hash_table_destroy(bluez_adapters[hci_dev_id].device_sep_map);
				bluez_adapters[hci_dev_id].device_sep_map = NULL;
			}

		}
		else if (strcmp(interface, BLUEZ_IFACE_MEDIA_ENDPOINT) == 0) {

			GArray *seps;
			bdaddr_t addr;
			size_t i;

			g_dbus_bluez_object_path_to_bdaddr(object_path, &addr);
			if ((seps = bluez_adapters_device_lookup(hci_dev_id, &addr)) != NULL)
				for (i = 0; i < seps->len; i++)
					if (strcmp(bluez_adapters_device_get_sep(seps, i).bluez_dbus_path, object_path) == 0)
						g_array_remove_index_fast(seps, i);

		}

	pthread_mutex_unlock(&bluez_mutex);
	g_variant_iter_free(interfaces);

}

static void bluez_signal_transport_changed(GDBusConnection *conn, const char *sender,
		const char *transport_path, const char *interface_, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)interface_;
	(void)signal;
	(void)userdata;

	struct ba_adapter *a = NULL;
	struct ba_device *d = NULL;
	struct ba_transport *t = NULL;

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(transport_path);
	if ((a = ba_adapter_lookup(hci_dev_id)) == NULL) {
		error("Adapter not available: %s", transport_path);
		return;
	}

	GVariantIter *properties = NULL;
	const char *interface;
	const char *property;
	GVariant *value;

	bdaddr_t addr;
	g_dbus_bluez_object_path_to_bdaddr(transport_path, &addr);
	if ((d = ba_device_lookup(a, &addr)) == NULL) {
		error("Device not available: %s", transport_path);
		goto final;
	}

	if ((t = ba_transport_lookup(d, transport_path)) == NULL) {
		error("Transport not available: %s", transport_path);
		goto final;
	}

	g_variant_get(params, "(&sa{sv}as)", &interface, &properties, NULL);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {
		debug("Signal: %s.%s(): %s: %s", interface_, signal, interface, property);

		if (strcmp(property, "State") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_STRING, property)) {
			const char *state = g_variant_get_string(value, NULL);
			ba_transport_set_a2dp_state(t, bluez_a2dp_transport_state_from_string(state));
		}
		else if (strcmp(property, "Delay") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			t->a2dp.delay = g_variant_get_uint16(value);
			bluealsa_dbus_pcm_update(&t->a2dp.pcm, BA_DBUS_PCM_UPDATE_DELAY);
		}
		else if (strcmp(property, "Volume") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_UINT16, property)) {
			/* received volume is in range [0, 127] */
			const uint16_t volume = g_variant_get_uint16(value);
			if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE &&
					t->a2dp.pcm.soft_volume)
				debug("Skipping A2DP volume update: %u", volume);
			else {
				int level = ba_transport_pcm_volume_bt_to_level(&t->a2dp.pcm, volume);
				debug("Updating A2DP volume: %u [%.2f dB]", volume, 0.01 * level);
				t->a2dp.pcm.volume[0].level = t->a2dp.pcm.volume[1].level = level;
				bluealsa_dbus_pcm_update(&t->a2dp.pcm, BA_DBUS_PCM_UPDATE_VOLUME);
			}
		}

		g_variant_unref(value);
	}
	g_variant_iter_free(properties);

final:
	if (a != NULL)
		ba_adapter_unref(a);
	if (d != NULL)
		ba_device_unref(d);
	if (t != NULL)
		ba_transport_unref(t);
}

/**
 * Monitor BlueZ service availability.
 *
 * When BlueZ is properly shutdown, we are notified about adapter removal via
 * the InterfacesRemoved signal. Here, we get the opportunity to perform some
 * cleanup if BlueZ service was killed. */
static void bluez_signal_name_owner_changed(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *signal, GVariant *params,
		void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)signal;
	(void)userdata;

	const char *name;
	const char *owner_old;
	const char *owner_new;

	g_variant_get(params, "(&s&s&s)", &name, &owner_old, &owner_new);
	if (owner_old != NULL && owner_old[0] != '\0') {

		pthread_mutex_lock(&bluez_mutex);

		GHashTableIter iter;
		struct bluez_dbus_object_data *dbus_obj;
		g_hash_table_iter_init(&iter, dbus_object_data_map);
		while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj)) {
			g_dbus_connection_unregister_object(conn, dbus_obj->id);
			g_hash_table_iter_remove(&iter);
		}

		size_t i;
		for (i = 0; i < ARRAYSIZE(bluez_adapters); i++)
			if (bluez_adapters[i].adapter != NULL) {
				ba_adapter_destroy(bluez_adapters[i].adapter);
				bluez_adapters[i].adapter = NULL;
				g_hash_table_destroy(bluez_adapters[i].device_sep_map);
				bluez_adapters[i].device_sep_map = NULL;
			}

		pthread_mutex_unlock(&bluez_mutex);

	}

}

/**
 * Subscribe to BlueZ signals.
 *
 * @return On success this function returns 0. Otherwise -1 is returned. */
int bluez_subscribe_signals(void) {

	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			DBUS_IFACE_OBJECT_MANAGER, "InterfacesAdded", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_interfaces_added, NULL, NULL);
	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			DBUS_IFACE_OBJECT_MANAGER, "InterfacesRemoved", NULL, NULL,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_interfaces_removed, NULL, NULL);

	g_dbus_connection_signal_subscribe(config.dbus, BLUEZ_SERVICE,
			DBUS_IFACE_PROPERTIES, "PropertiesChanged", NULL, BLUEZ_IFACE_MEDIA_TRANSPORT,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_transport_changed, NULL, NULL);

	g_dbus_connection_signal_subscribe(config.dbus, DBUS_SERVICE,
			DBUS_IFACE_DBUS, "NameOwnerChanged", NULL, BLUEZ_SERVICE,
			G_DBUS_SIGNAL_FLAGS_NONE, bluez_signal_name_owner_changed, NULL, NULL);

	return 0;
}

/**
 * Set new configuration for already connected A2DP endpoint.
 *
 * @param dbus_current_sep_path D-Bus SEP path of current connection.
 * @param sep New SEP to be configured.
 * @param error NULL GError pointer.
 * @return On success this function returns true. */
bool bluez_a2dp_set_configuration(
		const char *dbus_current_sep_path,
		const struct a2dp_sep *sep,
		GError **error) {

	int hci_dev_id = g_dbus_bluez_object_path_to_hci_dev_id(sep->bluez_dbus_path);
	const char *endpoint = NULL;
	GDBusMessage *msg = NULL;
	GDBusMessage *rep = NULL;
	bool rv = false;

	/* When requesting new A2DP configuration, BlueZ will disconnect the
	 * existing connection and will try to establish a new one. So, when
	 * changing configuration for already selected codec we can reuse the
	 * endpoint path - it will be disconnected before new connection is
	 * established. Things are more complicated when we want to select
	 * different codec. In such case we will have to choose one of the
	 * endpoint paths registered in BlueZ but not connected. The issue
	 * is, that we can not avoid race condition when new BT device will
	 * try to set configuration as well - we have to pick endpoint path
	 * currently not-used by BlueZ... */

	pthread_mutex_lock(&bluez_mutex);

	GHashTableIter iter;
	struct bluez_dbus_object_data *dbus_obj;
	g_hash_table_iter_init(&iter, dbus_object_data_map);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer)&dbus_obj))
		if (dbus_obj->hci_dev_id == hci_dev_id &&
				dbus_obj->codec->codec_id == sep->codec_id &&
				dbus_obj->codec->dir == !sep->dir &&
				dbus_obj->registered) {

			/* reuse already selected endpoint path */
			if (strcmp(dbus_obj->path, dbus_current_sep_path) == 0) {
				endpoint = dbus_obj->path;
				break;
			}

			if (!dbus_obj->connected)
				endpoint = dbus_obj->path;

		}

	if (endpoint == NULL) {
		pthread_mutex_unlock(&bluez_mutex);
		*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
				"Extra A2DP endpoint not available");
		goto fail;
	}

	GVariantBuilder caps;
	g_variant_builder_init(&caps, G_VARIANT_TYPE("ay"));

	size_t i;
	for (i = 0; i < sep->capabilities_size; i++)
		g_variant_builder_add(&caps, "y", ((char *)sep->configuration)[i]);

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&props, "{sv}", "Capabilities", g_variant_builder_end(&caps));

	msg = g_dbus_message_new_method_call(BLUEZ_SERVICE,
			sep->bluez_dbus_path, BLUEZ_IFACE_MEDIA_ENDPOINT, "SetConfiguration");
	g_dbus_message_set_body(msg, g_variant_new("(oa{sv})", endpoint, &props));
	g_variant_builder_clear(&props);

	pthread_mutex_unlock(&bluez_mutex);

	if ((rep = g_dbus_connection_send_message_with_reply_sync(config.dbus, msg,
					G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, NULL, error)) == NULL)
		goto fail;

	if (g_dbus_message_get_message_type(rep) == G_DBUS_MESSAGE_TYPE_ERROR) {
		g_dbus_message_to_gerror(rep, error);
		goto fail;
	}

	rv = true;

fail:
	if (msg != NULL)
		g_object_unref(msg);
	if (rep != NULL)
		g_object_unref(rep);
	return rv;
}
