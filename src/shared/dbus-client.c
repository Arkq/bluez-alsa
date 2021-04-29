/*
 * BlueALSA - dbus-client.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "shared/dbus-client.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shared/defs.h"

static int path2ba(const char *path, bdaddr_t *ba) {

	unsigned int x[6];
	if ((path = strstr(path, "/dev_")) == NULL ||
			sscanf(&path[5], "%x_%x_%x_%x_%x_%x",
				&x[5], &x[4], &x[3], &x[2], &x[1], &x[0]) != 6)
		return -1;

	size_t i;
	for (i = 0; i < 6; i++)
		ba->b[i] = x[i];

	return 0;
}

static dbus_bool_t ba_dbus_watch_add(DBusWatch *watch, void *data) {
	struct ba_dbus_ctx *ctx = (struct ba_dbus_ctx *)data;
	DBusWatch **tmp = ctx->watches;
	if ((tmp = realloc(tmp, (ctx->watches_len + 1) * sizeof(*tmp))) == NULL)
		return FALSE;
	tmp[ctx->watches_len++] = watch;
	ctx->watches = tmp;
	return TRUE;
}

static void ba_dbus_watch_del(DBusWatch *watch, void *data) {
	struct ba_dbus_ctx *ctx = (struct ba_dbus_ctx *)data;
	size_t i;
	for (i = 0; i < ctx->watches_len; i++)
		if (ctx->watches[i] == watch)
			ctx->watches[i] = ctx->watches[--ctx->watches_len];
}

static void ba_dbus_watch_toggled(DBusWatch *watch, void *data) {
	(void)watch;
	(void)data;
}

dbus_bool_t bluealsa_dbus_connection_ctx_init(
		struct ba_dbus_ctx *ctx,
		const char *ba_service_name,
		DBusError *error) {

	/* Zero-out context structure, so it will be
	 * safe to call *_ctx_free() upon error. */
	memset(ctx, 0, sizeof(*ctx));

	if ((ctx->conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, error)) == NULL)
		return FALSE;

	/* do not terminate in case of D-Bus connection being lost */
	dbus_connection_set_exit_on_disconnect(ctx->conn, FALSE);

	if (!dbus_connection_set_watch_functions(ctx->conn, ba_dbus_watch_add,
				ba_dbus_watch_del, ba_dbus_watch_toggled, ctx, NULL)) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	strncpy(ctx->ba_service, ba_service_name, sizeof(ctx->ba_service) - 1);

	return TRUE;
}

void bluealsa_dbus_connection_ctx_free(
		struct ba_dbus_ctx *ctx) {
	if (ctx->conn != NULL) {
		dbus_connection_close(ctx->conn);
		dbus_connection_unref(ctx->conn);
		ctx->conn = NULL;
	}
	if (ctx->watches != NULL) {
		free(ctx->watches);
		ctx->watches = NULL;
	}
	if (ctx->matches != NULL) {
		size_t i;
		for (i = 0; i < ctx->matches_len; i++)
			free(ctx->matches[i]);
		free(ctx->matches);
		ctx->matches = NULL;
	}
}

dbus_bool_t bluealsa_dbus_connection_signal_match_add(
		struct ba_dbus_ctx *ctx,
		const char *sender,
		const char *path,
		const char *iface,
		const char *member,
		const char *extra) {

	char match[512] = "type='signal'";
	size_t len = 13;

	if (sender != NULL) {
		snprintf(&match[len], sizeof(match) - len, ",sender='%s'", sender);
		len += strlen(&match[len]);
	}
	if (path != NULL) {
		snprintf(&match[len], sizeof(match) - len, ",path='%s'", path);
		len += strlen(&match[len]);
	}
	if (iface != NULL) {
		snprintf(&match[len], sizeof(match) - len, ",interface='%s'", iface);
		len += strlen(&match[len]);
	}
	if (member != NULL) {
		snprintf(&match[len], sizeof(match) - len, ",member='%s'", member);
		len += strlen(&match[len]);
	}
	if (extra != NULL)
		snprintf(&match[len], sizeof(match) - len, ",%s", extra);

	char **tmp = ctx->matches;
	size_t tmp_len = ctx->matches_len;
	if ((tmp = realloc(tmp, (tmp_len + 1) * sizeof(*tmp))) == NULL)
		return FALSE;
	ctx->matches = tmp;
	if ((ctx->matches[tmp_len] = strdup(match)) == NULL)
		return FALSE;
	ctx->matches_len++;

	dbus_bus_add_match(ctx->conn, match, NULL);
	return TRUE;
}

dbus_bool_t bluealsa_dbus_connection_signal_match_clean(
		struct ba_dbus_ctx *ctx) {

	size_t i;
	for (i = 0; i < ctx->matches_len; i++) {
		dbus_bus_remove_match(ctx->conn, ctx->matches[i], NULL);
		free(ctx->matches[i]);
	}

	ctx->matches_len = 0;
	return TRUE;
}

/**
 * Dispatch D-Bus messages synchronously. */
dbus_bool_t bluealsa_dbus_connection_dispatch(
		struct ba_dbus_ctx *ctx) {

	struct pollfd fds[8];
	nfds_t nfds = ARRAYSIZE(fds);

	bluealsa_dbus_connection_poll_fds(ctx, fds, &nfds);
	if (poll(fds, nfds, 0) > 0)
		bluealsa_dbus_connection_poll_dispatch(ctx, fds, nfds);

	/* Dispatch incoming D-Bus messages/signals. The actual dispatching is
	 * done in a function registered with dbus_connection_add_filter(). */
	while (dbus_connection_dispatch(ctx->conn) == DBUS_DISPATCH_DATA_REMAINS)
		continue;

	return TRUE;
}

dbus_bool_t bluealsa_dbus_connection_poll_fds(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t *nfds) {

	if (*nfds < ctx->watches_len) {
		*nfds = ctx->watches_len;
		return FALSE;
	}

	size_t i;
	for (i = 0; i < ctx->watches_len; i++) {
		DBusWatch *watch = ctx->watches[i];

		fds[i].fd = -1;
		fds[i].events = 0;

		if (dbus_watch_get_enabled(watch))
			fds[i].fd = dbus_watch_get_unix_fd(watch);
		if (dbus_watch_get_flags(watch) & DBUS_WATCH_READABLE)
			fds[i].events = POLLIN;

	}

	*nfds = ctx->watches_len;
	return TRUE;
}

dbus_bool_t bluealsa_dbus_connection_poll_dispatch(
		struct ba_dbus_ctx *ctx,
		struct pollfd *fds,
		nfds_t nfds) {

	dbus_bool_t rv = FALSE;
	size_t i;

	if (nfds > ctx->watches_len)
		nfds = ctx->watches_len;

	for (i = 0; i < nfds; i++)
		if (fds[i].revents) {
			unsigned int flags = 0;
			if (fds[i].revents & POLLIN)
				flags |= DBUS_WATCH_READABLE;
			if (fds[i].revents & POLLOUT)
				flags |= DBUS_WATCH_WRITABLE;
			if (fds[i].revents & POLLERR)
				flags |= DBUS_WATCH_ERROR;
			if (fds[i].revents & POLLHUP)
				flags |= DBUS_WATCH_HANGUP;
			dbus_watch_handle(ctx->watches[i], flags);
			rv = TRUE;
		}

	return rv;
}

dbus_bool_t bluealsa_dbus_get_pcms(
		struct ba_dbus_ctx *ctx,
		struct ba_pcm **pcms,
		size_t *length,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, "/org/bluealsa",
					BLUEALSA_INTERFACE_MANAGER, "GetPCMs")) == NULL) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	dbus_bool_t rv = TRUE;
	struct ba_pcm *_pcms = NULL;
	char *signature;
	size_t i;

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE, "Empty response message");
		goto fail;
	}

	DBusMessageIter iter_pcms;
	for (dbus_message_iter_recurse(&iter, &iter_pcms), i = 0;
			dbus_message_iter_get_arg_type(&iter_pcms) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_pcms), i++) {

		if (dbus_message_iter_get_arg_type(&iter_pcms) != DBUS_TYPE_DICT_ENTRY)
			goto fail_signature;

		struct ba_pcm *tmp = _pcms;
		if ((tmp = realloc(tmp, (i + 1) * sizeof(*tmp))) == NULL) {
			dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
			goto fail;
		}

		_pcms = tmp;

		DBusMessageIter iter_pcms_entry;
		dbus_message_iter_recurse(&iter_pcms, &iter_pcms_entry);

		DBusError err = DBUS_ERROR_INIT;
		if (!bluealsa_dbus_message_iter_get_pcm(&iter_pcms_entry, &err, &_pcms[i])) {
			dbus_set_error(error, err.name, "Get PCM: %s", err.message);
			dbus_error_free(&err);
			goto fail;
		}

	}

	*pcms = _pcms;
	*length = i;

	goto success;

fail_signature:
	signature = dbus_message_iter_get_signature(&iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != a{oa{sv}}", signature);

fail:
	if (_pcms != NULL)
		free(_pcms);
	rv = FALSE;

success:
	if (rep != NULL)
		dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}

dbus_bool_t bluealsa_dbus_get_pcm(
		struct ba_dbus_ctx *ctx,
		const bdaddr_t *addr,
		unsigned int transports,
		unsigned int mode,
		struct ba_pcm *pcm,
		DBusError *error) {

	const bool get_last = bacmp(addr, BDADDR_ANY) == 0;
	struct ba_pcm *pcms = NULL;
	struct ba_pcm *match = NULL;
	dbus_bool_t rv = TRUE;
	size_t length = 0;
	uint32_t seq = 0;
	size_t i;

	if (!bluealsa_dbus_get_pcms(ctx, &pcms, &length, error))
		return FALSE;

	for (i = 0; i < length; i++) {
		if (get_last) {
			if (pcms[i].sequence >= seq &&
					pcms[i].transport & transports &&
					pcms[i].mode == mode) {
				seq = pcms[i].sequence;
				match = &pcms[i];
			}
		}
		else if (bacmp(&pcms[i].addr, addr) == 0 &&
				pcms[i].transport & transports &&
				pcms[i].mode == mode) {
			match = &pcms[i];
			break;
		}
	}

	if (match != NULL)
		memcpy(pcm, match, sizeof(*pcm));
	else {
		dbus_set_error(error, DBUS_ERROR_FILE_NOT_FOUND, "PCM not found");
		rv = FALSE;
	}

	free(pcms);
	return rv;
}

/**
 * Open BlueALSA PCM stream. */
dbus_bool_t bluealsa_dbus_open_pcm(
		struct ba_dbus_ctx *ctx,
		const char *pcm_path,
		int *fd_pcm,
		int *fd_pcm_ctrl,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, pcm_path,
					BLUEALSA_INTERFACE_PCM, "Open")) == NULL) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL) {
		dbus_message_unref(msg);
		return FALSE;
	}

	dbus_bool_t rv;
	rv = dbus_message_get_args(rep, error,
			DBUS_TYPE_UNIX_FD, fd_pcm,
			DBUS_TYPE_UNIX_FD, fd_pcm_ctrl,
			DBUS_TYPE_INVALID);

	dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}

/**
 * Open BlueALSA RFCOMM socket for dispatching AT commands. */
dbus_bool_t bluealsa_dbus_open_rfcomm(
		struct ba_dbus_ctx *ctx,
		const char *rfcomm_path,
		int *fd_rfcomm,
		DBusError *error) {

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, rfcomm_path,
					BLUEALSA_INTERFACE_RFCOMM, "Open")) == NULL) {
		dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
		return FALSE;
	}

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL) {
		dbus_message_unref(msg);
		return FALSE;
	}

	dbus_bool_t rv;
	rv = dbus_message_get_args(rep, error,
			DBUS_TYPE_UNIX_FD, fd_rfcomm,
			DBUS_TYPE_INVALID);

	dbus_message_unref(rep);
	dbus_message_unref(msg);
	return rv;
}

/**
 * Update BlueALSA PCM property. */
dbus_bool_t bluealsa_dbus_pcm_update(
		struct ba_dbus_ctx *ctx,
		const struct ba_pcm *pcm,
		enum ba_pcm_property property,
		DBusError *error) {

	static const char *interface = BLUEALSA_INTERFACE_PCM;
	const char *_property = NULL;
	const char *variant = NULL;
	const void *value = NULL;
	int type = -1;

	switch (property) {
	case BLUEALSA_PCM_SOFT_VOLUME:
		_property = "SoftVolume";
		variant = DBUS_TYPE_BOOLEAN_AS_STRING;
		value = &pcm->soft_volume;
		type = DBUS_TYPE_BOOLEAN;
		break;
	case BLUEALSA_PCM_VOLUME:
		_property = "Volume";
		variant = DBUS_TYPE_UINT16_AS_STRING;
		value = &pcm->volume.raw;
		type = DBUS_TYPE_UINT16;
		break;
	}

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service, pcm->pcm_path,
					DBUS_INTERFACE_PROPERTIES, "Set")) == NULL)
		goto fail;

	DBusMessageIter iter;
	DBusMessageIter iter_val;

	dbus_message_iter_init_append(msg, &iter);
	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface) ||
			!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &_property) ||
			!dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, variant, &iter_val) ||
			!dbus_message_iter_append_basic(&iter_val, type, value) ||
			!dbus_message_iter_close_container(&iter, &iter_val))
		goto fail;

	if (!dbus_connection_send(ctx->conn, msg, NULL))
		goto fail;

	dbus_message_unref(msg);
	return TRUE;

fail:
	if (msg != NULL)
		dbus_message_unref(msg);
	dbus_set_error(error, DBUS_ERROR_NO_MEMORY, NULL);
	return FALSE;
}

/**
 * Send command to the BlueALSA PCM controller socket. */
dbus_bool_t bluealsa_dbus_pcm_ctrl_send(
		int fd_pcm_ctrl,
		const char *command,
		DBusError *error) {

	ssize_t len = strlen(command);
	if (write(fd_pcm_ctrl, command, len) == -1) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Write: %s", strerror(errno));
		return FALSE;
	}

	/* PCM controller socket is created in the non-blocking
	 * mode, so we have to poll for reading by ourself. */
	struct pollfd pfd = { fd_pcm_ctrl, POLLIN, 0 };
	poll(&pfd, 1, -1);

	char rep[32];
	if ((len = read(fd_pcm_ctrl, rep, sizeof(rep))) == -1) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Read: %s", strerror(errno));
		return FALSE;
	}

	if (strncmp(rep, "OK", len) != 0) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "Response: %s", rep);
		errno = ENOMSG;
		return FALSE;
	}

	return TRUE;
}

/**
 * Call the given function for each key/value pairs. */
dbus_bool_t bluealsa_dbus_message_iter_dict(
		DBusMessageIter *iter,
		DBusError *error,
		dbus_bool_t (*cb)(const char *key, DBusMessageIter *val, void *data, DBusError *err),
		void *userdata) {

	char *signature;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY)
		goto fail;

	DBusMessageIter iter_dict;
	for (dbus_message_iter_recurse(iter, &iter_dict);
			dbus_message_iter_get_arg_type(&iter_dict) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_dict)) {

		DBusMessageIter iter_entry;
		DBusMessageIter iter_entry_val;
		const char *key;

		if (dbus_message_iter_get_arg_type(&iter_dict) != DBUS_TYPE_DICT_ENTRY)
			goto fail;
		dbus_message_iter_recurse(&iter_dict, &iter_entry);
		if (dbus_message_iter_get_arg_type(&iter_entry) != DBUS_TYPE_STRING)
			goto fail;
		dbus_message_iter_get_basic(&iter_entry, &key);
		if (!dbus_message_iter_next(&iter_entry) ||
				dbus_message_iter_get_arg_type(&iter_entry) != DBUS_TYPE_VARIANT)
			goto fail;
		dbus_message_iter_recurse(&iter_entry, &iter_entry_val);

		if (!cb(key, &iter_entry_val, userdata, error))
			return FALSE;

	}

	return TRUE;

fail:
	signature = dbus_message_iter_get_signature(iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != a{sv}", signature);
	dbus_free(signature);
	return FALSE;
}

static dbus_bool_t parse_dbus_string_array(DBusMessageIter *iter, void *out, int size, int strlen, DBusError *err) {
	DBusMessageIter iter2;
	char *array = out;
	int count;
	for (dbus_message_iter_recurse(iter, &iter2), count = 0;
			dbus_message_iter_get_arg_type(&iter2) != DBUS_TYPE_INVALID &&
				count < size;
			dbus_message_iter_next(&iter2), count++) {
		if (dbus_message_iter_get_arg_type(&iter2) != DBUS_TYPE_STRING) {
			dbus_set_error(err, DBUS_ERROR_FAILED, "DBus message corrupted");
			return FALSE;
		}

		const char *tmp;
		dbus_message_iter_get_basic(&iter2, &tmp);
		strncpy(&array[count * strlen], tmp, strlen);
	}
	array[count * strlen] = 0;
	return TRUE;
}

static dbus_bool_t parse_hfp_config(const char *key, DBusMessageIter *val, void *data, DBusError *error) {
	struct ba_status *status = (struct ba_status *)data;
	char type = dbus_message_iter_get_arg_type(val);
	if (strcmp(key, "FeaturesSDPHF") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!parse_dbus_string_array(val, &status->hfp.sdp_features_hf, ARRAYSIZE(status->hfp.sdp_features_hf), sizeof(status->hfp.sdp_features_hf[0]), error))
			return FALSE;
	}
	else if (strcmp(key, "FeaturesSDPAG") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!parse_dbus_string_array(val, &status->hfp.sdp_features_ag, ARRAYSIZE(status->hfp.sdp_features_ag), sizeof(status->hfp.sdp_features_ag[0]), error))
			return FALSE;
	}
	else if (strcmp(key, "FeaturesRFCOMMHF") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!parse_dbus_string_array(val, &status->hfp.rfcomm_features_hf, ARRAYSIZE(status->hfp.rfcomm_features_hf), sizeof(status->hfp.rfcomm_features_hf[0]), error))
			return FALSE;
	}
	else if (strcmp(key, "FeaturesRFCOMMAG") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!parse_dbus_string_array(val, &status->hfp.rfcomm_features_ag, ARRAYSIZE(status->hfp.rfcomm_features_ag), sizeof(status->hfp.rfcomm_features_ag[0]), error))
			return FALSE;
	}
	else if (strcmp(key, "XAPLVendorID") == 0) {
		if (type != DBUS_TYPE_UINT32)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->hfp.xapl_vendor_id);
	}
	else if (strcmp(key, "XAPLProductID") == 0) {
		if (type != DBUS_TYPE_UINT32)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->hfp.xapl_product_id);
	}
	else if (strcmp(key, "XAPLSoftwareVersion") == 0) {
		if (type != DBUS_TYPE_STRING)
			return FALSE;
		const char *tmp;
		dbus_message_iter_get_basic(val, &tmp);
		strncpy(status->hfp.xapl_software_version, tmp, sizeof(status->hfp.xapl_software_version) - 1);
	}
	else if (strcmp(key, "XAPLProductName") == 0) {
		if (type != DBUS_TYPE_STRING)
			return FALSE;
		const char *tmp;
		dbus_message_iter_get_basic(val, &tmp);
		strncpy(status->hfp.xapl_product_name, tmp, sizeof(status->hfp.xapl_product_name) - 1);
	}
	else if (strcmp(key, "XAPLFeatures") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!parse_dbus_string_array(val, &status->hfp.xapl_features, ARRAYSIZE(status->hfp.xapl_features), sizeof(status->hfp.xapl_features[0]), error))
			return FALSE;
	}
	return TRUE;
}

static dbus_bool_t parse_battery_config(const char *key, DBusMessageIter *val, void *data, DBusError *error) {
	struct ba_status *status = (struct ba_status *)data;
	char type = dbus_message_iter_get_arg_type(val);
	if (strcmp(key, "Available") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->battery.available);
	}
	if (strcmp(key, "Level") == 0) {
		if (type != DBUS_TYPE_UINT32)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->battery.level);
	}

	return TRUE;
}

static dbus_bool_t parse_a2dp_config(const char *key, DBusMessageIter *val, void *data, DBusError *error) {
	struct ba_status *status = (struct ba_status *)data;
	char type = dbus_message_iter_get_arg_type(val);
	if (strcmp(key, "NativeVolume") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->a2dp.native_volume);
	}
	else if (strcmp(key, "ForceMono") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->a2dp.force_mono);
	}
	else if (strcmp(key, "Force44100") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->a2dp.force_44100);
	}
	else if (strcmp(key, "KeepAlive") == 0) {
		if (type != DBUS_TYPE_INT32)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->a2dp.keep_alive);
	}
	return TRUE;
}

static dbus_bool_t parse_aac_config(const char *key, DBusMessageIter *val, void *data, DBusError *error) {
	struct ba_status *status = (struct ba_status *)data;
	char type = dbus_message_iter_get_arg_type(val);
	if (strcmp(key, "Available") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->aac.available);
	}
	else if (strcmp(key, "Afterburner") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->aac.afterburner);
	}
	else if (strcmp(key, "LATMVersion") == 0) {
		if (type != DBUS_TYPE_BYTE)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->aac.latm_version);
	}
	else if (strcmp(key, "VBRMode") == 0) {
		if (type != DBUS_TYPE_BYTE)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->aac.vbr_mode);
	}
	return TRUE;
}

static dbus_bool_t parse_mpeg_config(const char *key, DBusMessageIter *val, void *data, DBusError *error) {
	struct ba_status *status = (struct ba_status *)data;
	char type = dbus_message_iter_get_arg_type(val);
	if (strcmp(key, "Available") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->mpeg.available);
	}
	else if (strcmp(key, "Quality") == 0) {
		if (type != DBUS_TYPE_BYTE)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->mpeg.quality);
	}
	else if (strcmp(key, "VBRQuality") == 0) {
		if (type != DBUS_TYPE_BYTE)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->mpeg.vbr_quality);
	}
	return TRUE;
}

static dbus_bool_t parse_ldac_config(const char *key, DBusMessageIter *val, void *data, DBusError *error) {
	struct ba_status *status = (struct ba_status *)data;
	char type = dbus_message_iter_get_arg_type(val);
	if (strcmp(key, "Available") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->ldac.available);
	}
	else if (strcmp(key, "ABR") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->ldac.abr);
	}
	else if (strcmp(key, "Eqmid") == 0) {
		if (type != DBUS_TYPE_BYTE)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->ldac.eqmid);
	}
	return TRUE;
}

/* Callback function for BlueALSA Service status parser. */
static dbus_bool_t bluealsa_dbus_message_iter_get_status_cb(const char *key,
		DBusMessageIter *val, void *userdata, DBusError *error) {

	struct ba_status *status = (struct ba_status *)userdata;
	char type = dbus_message_iter_get_arg_type(val);

	if (strcmp(key, "Version") == 0) {
		if (type != DBUS_TYPE_STRING)
			return FALSE;
		const char *tmp;
		dbus_message_iter_get_basic(val, &tmp);
		strncpy(status->version, tmp, sizeof(status->version) - 1);
	}
	else if (strcmp(key, "Profiles") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (! parse_dbus_string_array(val, &status->profiles, ARRAYSIZE(status->profiles), sizeof(status->profiles[0]), error))
			return FALSE;
	}
	else if (strcmp(key, "Adapters") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (! parse_dbus_string_array(val, &status->adapters, ARRAYSIZE(status->adapters), sizeof(status->adapters[0]), error))
			return FALSE;
	}
	else if (strcmp(key, "AdapterFilter") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (! parse_dbus_string_array(val, &status->adapter_filter, ARRAYSIZE(status->adapter_filter), sizeof(status->adapter_filter[0]), error))
			return FALSE;
	}
	else if (strcmp(key, "HFP") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!bluealsa_dbus_message_iter_dict(val, error, parse_hfp_config, status))
			return FALSE;
	}
	else if (strcmp(key, "MSBC") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->msbc_available);
	}
	else if (strcmp(key, "A2DP") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!bluealsa_dbus_message_iter_dict(val, error, parse_a2dp_config, status))
			return FALSE;
	}
	else if (strcmp(key, "SBCQuality") == 0) {
		if (type != DBUS_TYPE_STRING)
			return FALSE;
		const char *tmp;
		dbus_message_iter_get_basic(val, &tmp);
		strncpy(status->sbc_quality, tmp, sizeof(status->sbc_quality) - 1);
	}
	else if (strcmp(key, "AAC") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!bluealsa_dbus_message_iter_dict(val, error, parse_aac_config, status))
			return FALSE;
	}
	else if (strcmp(key, "MPEG") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!bluealsa_dbus_message_iter_dict(val, error, parse_mpeg_config, status))
			return FALSE;
	}
	else if (strcmp(key, "APTX") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->aptx_available);
	}
	else if (strcmp(key, "APTX-HD") == 0) {
		if (type != DBUS_TYPE_BOOLEAN)
			return FALSE;
		dbus_message_iter_get_basic(val, &status->aptx_hd_available);
	}
	else if (strcmp(key, "LDAC") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!bluealsa_dbus_message_iter_dict(val, error, parse_ldac_config, status))
			return FALSE;
	}
	else if (strcmp(key, "Battery") == 0) {
		if (type != DBUS_TYPE_ARRAY)
			return FALSE;
		if (!bluealsa_dbus_message_iter_dict(val, error, parse_battery_config, status))
			return FALSE;
	}

	return TRUE;

}

/**
 * Get status of BlueALSA service. */
dbus_bool_t bluealsa_dbus_get_status(
		struct ba_dbus_ctx *ctx,
		struct ba_status *status,
		DBusError *error) {

	dbus_bool_t ret = FALSE;

	DBusMessage *msg;
	if ((msg = dbus_message_new_method_call(ctx->ba_service,
			"/org/bluealsa", DBUS_INTERFACE_PROPERTIES, "GetAll")) == NULL) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "%s", strerror(ENOMEM));
		goto fail;
	}

	DBusMessageIter iter;
	dbus_message_iter_init_append(msg, &iter);
	static const char *interface = BLUEALSA_INTERFACE_MANAGER;

	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &interface)) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "%s", strerror(ENOMEM));
		goto fail;
	}

	DBusMessage *rep;
	if ((rep = dbus_connection_send_with_reply_and_block(ctx->conn,
			msg, DBUS_TIMEOUT_USE_DEFAULT, error)) == NULL)
		goto fail;

	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(error, DBUS_ERROR_FAILED, "%s", strerror(ENOMEM));
		goto fail;
	}

	if (!bluealsa_dbus_message_iter_dict(&iter, error, bluealsa_dbus_message_iter_get_status_cb, status)
)
		goto fail;

	ret = TRUE;

fail:
	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return ret;

}

/**
 * Parse BlueALSA PCM. */
dbus_bool_t bluealsa_dbus_message_iter_get_pcm(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm) {

	const char *path;
	char *signature;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_OBJECT_PATH)
		goto fail;

	memset(pcm, 0, sizeof(*pcm));

	dbus_message_iter_get_basic(iter, &path);
	strncpy(pcm->pcm_path, path, sizeof(pcm->pcm_path) - 1);

	if (!dbus_message_iter_next(iter))
		goto fail;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_message_iter_get_pcm_props(iter, &err, pcm)) {
		dbus_set_error(error, err.name, "Get properties: %s", err.message);
		dbus_error_free(&err);
		return FALSE;
	}

	return TRUE;

fail:
	signature = dbus_message_iter_get_signature(iter);
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect signature: %s != oa{sv}", signature);
	dbus_free(signature);
	return FALSE;
}

/**
 * Callback function for BlueALSA PCM properties parser. */
static dbus_bool_t bluealsa_dbus_message_iter_get_pcm_props_cb(const char *key,
		DBusMessageIter *variant, void *userdata, DBusError *error) {
	struct ba_pcm *pcm = (struct ba_pcm *)userdata;

	char type = dbus_message_iter_get_arg_type(variant);
	char type_expected;
	const char *tmp;

	if (strcmp(key, "Device") == 0) {
		if (type != (type_expected = DBUS_TYPE_OBJECT_PATH))
			goto fail;
		dbus_message_iter_get_basic(variant, &tmp);
		strncpy(pcm->device_path, tmp, sizeof(pcm->device_path) - 1);
		path2ba(tmp, &pcm->addr);
	}
	else if (strcmp(key, "Sequence") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT32))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->sequence);
	}
	else if (strcmp(key, "Transport") == 0) {
		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;
		dbus_message_iter_get_basic(variant, &tmp);
		if (strstr(tmp, "A2DP-source") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_A2DP_SOURCE;
		else if (strstr(tmp, "A2DP-sink") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_A2DP_SINK;
		else if (strstr(tmp, "HFP-AG") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_HFP_AG;
		else if (strstr(tmp, "HFP-HF") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_HFP_HF;
		else if (strstr(tmp, "HSP-AG") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_HSP_AG;
		else if (strstr(tmp, "HSP-HS") != NULL)
			pcm->transport = BA_PCM_TRANSPORT_HSP_HS;
	}
	else if (strcmp(key, "Mode") == 0) {
		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;
		dbus_message_iter_get_basic(variant, &tmp);
		if (strcmp(tmp, "source") == 0)
			pcm->mode = BA_PCM_MODE_SOURCE;
		else if (strcmp(tmp, "sink") == 0)
			pcm->mode = BA_PCM_MODE_SINK;
	}
	else if (strcmp(key, "Format") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->format);
	}
	else if (strcmp(key, "Channels") == 0) {
		if (type != (type_expected = DBUS_TYPE_BYTE))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->channels);
	}
	else if (strcmp(key, "Sampling") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT32))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->sampling);
	}
	else if (strcmp(key, "Codec") == 0) {
		if (type != (type_expected = DBUS_TYPE_STRING))
			goto fail;
		dbus_message_iter_get_basic(variant, &tmp);
		strncpy(pcm->codec, tmp, sizeof(pcm->codec) - 1);
	}
	else if (strcmp(key, "Delay") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->delay);
	}
	else if (strcmp(key, "SoftVolume") == 0) {
		if (type != (type_expected = DBUS_TYPE_BOOLEAN))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->soft_volume);
	}
	else if (strcmp(key, "Volume") == 0) {
		if (type != (type_expected = DBUS_TYPE_UINT16))
			goto fail;
		dbus_message_iter_get_basic(variant, &pcm->volume.raw);
	}

	return TRUE;

fail:
	dbus_set_error(error, DBUS_ERROR_INVALID_SIGNATURE,
			"Incorrect variant for '%s': %c != %c", key, type, type_expected);
	return FALSE;
}

/**
 * Parse BlueALSA PCM properties. */
dbus_bool_t bluealsa_dbus_message_iter_get_pcm_props(
		DBusMessageIter *iter,
		DBusError *error,
		struct ba_pcm *pcm) {
	return bluealsa_dbus_message_iter_dict(iter, error,
			bluealsa_dbus_message_iter_get_pcm_props_cb, pcm);
}
