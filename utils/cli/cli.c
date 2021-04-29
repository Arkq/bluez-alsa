/*
 * BlueALSA - cli.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "shared/dbus-client.h"
#include "shared/defs.h"
#include "shared/log.h"

/**
 * Helper macros for internal usage. */
#define cli_print_error(M, ...) if (!quiet) { error(M, ##__VA_ARGS__); }
#define cmd_print_error(M, ...) if (!quiet) { error("CMD \"%s\": " M, argv[0], ##__VA_ARGS__); }

static struct ba_dbus_ctx dbus_ctx;
static char dbus_ba_service[32] = BLUEALSA_SERVICE;
static bool quiet = false;

static const char *transport_code_to_string(int transport_code) {
	switch (transport_code) {
	case BA_PCM_TRANSPORT_A2DP_SOURCE:
		return "A2DP-source";
	case BA_PCM_TRANSPORT_A2DP_SINK:
		return"A2DP-sink";
	case BA_PCM_TRANSPORT_HFP_AG:
		return "HFP-AG";
	case BA_PCM_TRANSPORT_HFP_HF:
		return "HFP-HF";
	case BA_PCM_TRANSPORT_HSP_AG:
		return "HSP-AG";
	case BA_PCM_TRANSPORT_HSP_HS:
		return "HSP-HS";
	case BA_PCM_TRANSPORT_MASK_A2DP:
		return "A2DP";
	case BA_PCM_TRANSPORT_MASK_HFP:
		return "HFP";
	case BA_PCM_TRANSPORT_MASK_HSP:
		return "HSP";
	case BA_PCM_TRANSPORT_MASK_SCO:
		return "SCO";
	case BA_PCM_TRANSPORT_MASK_AG:
		return "AG";
	case BA_PCM_TRANSPORT_MASK_HF:
		return "HF";
	default:
		return "Invalid";
	}
}

static const char *pcm_mode_to_string(int pcm_mode) {
	switch (pcm_mode) {
	case BA_PCM_MODE_SINK:
		return "sink";
	case BA_PCM_MODE_SOURCE:
		return "source";
	default:
		return "Invalid";
	}
}

static const char *pcm_format_to_string(int pcm_format) {
	switch (pcm_format) {
	case 0x0108:
		return "U8";
	case 0x8210:
		return "S16_LE";
	case 0x8318:
		return "S24_3LE";
	case 0x8418:
		return "S24_LE";
	case 0x8420:
		return "S32_LE";
	default:
		return "Invalid";
	}
}

static bool get_pcm(const char *path, struct ba_pcm *pcm) {

	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;
	bool found = false;
	size_t i;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &pcms, &pcms_count, &err))
		return false;

	for (i = 0; i < pcms_count; i++)
		if (strcmp(pcms[i].pcm_path, path) == 0) {
			memcpy(pcm, &pcms[i], sizeof(*pcm));
			found = true;
			break;
		}

	free(pcms);
	return found;
}

static bool print_codecs(const char *path, DBusError *err) {

	DBusMessage *msg = NULL, *rep = NULL;
	bool result = false;
	int count = 0;

	printf("Available codecs:");

	if ((msg = dbus_message_new_method_call(dbus_ctx.ba_service, path,
					BLUEALSA_INTERFACE_PCM, "GetCodecs")) == NULL) {
		dbus_set_error(err, DBUS_ERROR_FAILED, "%s", strerror(ENOMEM));
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, err)) == NULL) {
		goto fail;
	}

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(err, DBUS_ERROR_FAILED, "%s", strerror(ENOMEM));
		goto fail;
	}

	DBusMessageIter iter_codecs;
	for (dbus_message_iter_recurse(&iter, &iter_codecs);
			dbus_message_iter_get_arg_type(&iter_codecs) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_codecs)) {

		if (dbus_message_iter_get_arg_type(&iter_codecs) != DBUS_TYPE_DICT_ENTRY) {
			dbus_set_error(err, DBUS_ERROR_FAILED, "Message corrupted");
			goto fail;
		}

		DBusMessageIter iter_codecs_entry;
		dbus_message_iter_recurse(&iter_codecs, &iter_codecs_entry);

		if (dbus_message_iter_get_arg_type(&iter_codecs_entry) != DBUS_TYPE_STRING) {
			dbus_set_error(err, DBUS_ERROR_FAILED, "Message corrupted");
			goto fail;
		}

		const char *codec;
		dbus_message_iter_get_basic(&iter_codecs_entry, &codec);
		printf(" %s", codec);
		++count;

		/* Ignore the properties field, get next codec. */
	}
	result = true;

fail:
	if (count == 0)
		printf(" [ Unknown ] ");
	printf("\n");

	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return result;
}

static void print_volume(struct ba_pcm *pcm) {
	if (pcm->channels == 2)
		printf("Volume: L: %u R: %u\n", pcm->volume.ch1_volume, pcm->volume.ch2_volume);
	else
		printf("Volume: %u\n", pcm->volume.ch1_volume);
}

static void print_mute(struct ba_pcm *pcm) {
	if (pcm->channels == 2)
		printf("Muted: L: %c R: %c\n",
				pcm->volume.ch1_muted ? 'Y' : 'N', pcm->volume.ch2_muted ? 'Y' : 'N');
	else
		printf("Muted: %c\n", pcm->volume.ch1_muted ? 'Y' : 'N');
}

static int cmd_list_pcms(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	struct ba_pcm *pcms = NULL;
	size_t pcms_count = 0;

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_get_pcms(&dbus_ctx, &pcms, &pcms_count, &err)) {
		cmd_print_error("Couldn't get BlueALSA PCM list: %s", err.message);
		return EXIT_FAILURE;
	}

	size_t i;
	for (i = 0; i < pcms_count; i++)
		printf("%s\n", pcms[i].pcm_path);

	free(pcms);
	return EXIT_SUCCESS;
}

static void print_string(const char *key, const char *val) {
	printf("%s: %s\n", key, val ? val : "[ Unknown ]");
}

static void print_string_array(const char *key, const void *data, int size, int strlen) {
	const char *array = data;
	int count;

	printf("%s:", key);
	for (count = 0; count < size && array[count * strlen]; count++)
		printf(" %s", &array[count*strlen]);
	printf("\n");
}

static void	print_boolean(const char *key, bool val) {
	printf("%s: %s\n", key, val ?  "Yes" : "No");
}

static bool profile_enabled(const char *profile, const struct ba_status *status) {
	return memmem(status->profiles, sizeof(status->profiles),
			profile, strlen(profile)) != NULL;
}

static int cmd_status(int argc, char *argv[]) {
	DBusError error = DBUS_ERROR_INIT;

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	struct ba_status status = { 0 };
	if (!bluealsa_dbus_get_status(&dbus_ctx, &status, &error)) {
		if (dbus_error_is_set(&error))
			cmd_print_error("DBus error: %s\n", error.message);
		return EXIT_FAILURE;
	}

	print_string("Version", status.version);

	print_string_array("Profiles", &status.profiles,
		ARRAYSIZE(status.profiles), sizeof(status.profiles[0]));

	print_string_array("Adapters", &status.adapters,
		ARRAYSIZE(status.adapters), sizeof(status.adapters[0]));

	print_string_array("Adapter Filter", status.adapter_filter,
		 ARRAYSIZE(status.adapter_filter), sizeof(status.adapter_filter[0]));

	bool internal_hfp_enabled = false;
	if (profile_enabled("HFP-AG", &status)) {
		internal_hfp_enabled = true;
		print_string_array("HFP AG SDP Features",
			status.hfp.sdp_features_ag,
			ARRAYSIZE(status.hfp.sdp_features_ag),
			sizeof(status.hfp.sdp_features_ag[0]));
		print_string_array("HFP AG RFCOMM Features",
			status.hfp.rfcomm_features_ag,
			ARRAYSIZE(status.hfp.rfcomm_features_ag),
			sizeof(status.hfp.rfcomm_features_ag[0]));
	}

	if (profile_enabled("HFP-HF", &status)) {
		internal_hfp_enabled = true;
		print_string_array("HFP HF SDP Features",
			status.hfp.sdp_features_hf,
			ARRAYSIZE(status.hfp.sdp_features_hf),
			sizeof(status.hfp.sdp_features_hf[0]));
		print_string_array("HFP HF RFCOMM Features",
			status.hfp.rfcomm_features_hf,
			ARRAYSIZE(status.hfp.rfcomm_features_hf),
			sizeof(status.hfp.rfcomm_features_hf[0]));
	}

	if (internal_hfp_enabled) {
		printf("HFP XAPL Vendor ID: 0x%X\n", status.hfp.xapl_vendor_id);
		printf("HFP XAPL Product ID: 0x%X\n", status.hfp.xapl_product_id);
		print_string("HFP XAPL Software Version", status.hfp.xapl_software_version);
		print_string("HFP XAPL Product Name", status.hfp.xapl_product_name);
		print_string_array("HFP XAPL Features",
			status.hfp.xapl_features,
			ARRAYSIZE(status.hfp.xapl_features),
			sizeof(status.hfp.xapl_features[0]));
	}

	if (profile_enabled("HFP", &status)) {
		print_boolean("HFP mSBC Available", status.msbc_available);
	}

	if (profile_enabled("A2DP-source", &status)) {
		print_boolean("A2DP Native Volume", status.a2dp.native_volume);
		print_boolean("A2DP Force Mono", status.a2dp.force_mono);
		print_boolean("A2DP Force 44100", status.a2dp.force_44100);
		if (status.a2dp.keep_alive >= 0)
			printf("A2DP Keep Alive: %d seconds\n", status.a2dp.keep_alive);
		else
			printf("A2DP Keep Alive: %s\n", "Unlimited");
		print_string("SBC Quality", status.sbc_quality);
	}

	if (profile_enabled("A2DP", &status)) {
		print_boolean("AAC Available", status.aac.available);
		if (status.aac.available && profile_enabled("A2DP-source", &status)) {
			print_boolean("AAC Afterburner", status.aac.afterburner);
			printf("AAC LATM Version: %u\n", status.aac.latm_version);
			printf("AAC VBR Mode: %u\n", status.aac.vbr_mode);
		}

		print_boolean("MPEG Available", status.mpeg.available);
		if (status.mpeg.available && profile_enabled("A2DP-source", &status)) {
			printf("MPEG Quality: %u\n", status.mpeg.quality);
			printf("MPEG VBR Quality: %u\n", status.mpeg.vbr_quality);
		}

		print_boolean("APTX Available", status.aptx_available);
		print_boolean("APTX-HD Available", status.aptx_hd_available);

		print_boolean("LDAC Available", status.ldac.available);
		if (status.ldac.available && profile_enabled("A2DP-source", &status)) {
			print_boolean("LDAC ABR", status.ldac.abr);
			printf("LDAC Eqmid: %u\n", status.ldac.eqmid);
		}
	}

	print_boolean("Battery Available", status.battery.available);
	if (status.battery.available)
		printf("Battery Level: %d%%\n", status.battery.level);

	return EXIT_SUCCESS;
}

static int cmd_list_services(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusMessage *msg = NULL, *rep = NULL;
	DBusError err = DBUS_ERROR_INIT;
	int result = EXIT_FAILURE;

	if ((msg = dbus_message_new_method_call("org.freedesktop.DBus",
			"/org/freedesktop/DBus",
			"org.freedesktop.DBus", "ListNames")) == NULL) {
		dbus_set_error(&err, DBUS_ERROR_FAILED, "%s", strerror(ENOMEM));
		goto fail;
	}

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn,
				msg, DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		goto fail;
	}

	DBusMessageIter iter;
	if (!dbus_message_iter_init(rep, &iter)) {
		dbus_set_error(&err, DBUS_ERROR_FAILED, "%s", strerror(ENOMEM));
		goto fail;
	}

	DBusMessageIter iter_names;
	for (dbus_message_iter_recurse(&iter, &iter_names);
			dbus_message_iter_get_arg_type(&iter_names) != DBUS_TYPE_INVALID;
			dbus_message_iter_next(&iter_names)) {

		if (dbus_message_iter_get_arg_type(&iter_names) != DBUS_TYPE_STRING) {
			dbus_set_error(&err, DBUS_ERROR_FAILED, "DBus message corrupted");
			goto fail;
		}

		const char *name;
		dbus_message_iter_get_basic(&iter_names, &name);
		if (strncmp(name, "org.bluealsa", strlen("org.bluealsa")) == 0)
			printf("%s\n", name);

	}
	result = EXIT_SUCCESS;

fail:
	if (dbus_error_is_set(&err))
		cmd_print_error("DBus error: %s\n", err.message);

	if (msg != NULL)
		dbus_message_unref(msg);
	if (rep != NULL)
		dbus_message_unref(rep);
	return result;
}

static int cmd_info(int argc, char *argv[]) {

	if (argc != 2) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	printf("Device: %s\n", pcm.device_path);
	printf("Sequence: %u\n", pcm.sequence);
	printf("Transport: %s\n", transport_code_to_string(pcm.transport));
	printf("Mode: %s\n", pcm_mode_to_string(pcm.mode));
	printf("Format: %s\n", pcm_format_to_string(pcm.format));
	printf("Channels: %d\n", pcm.channels);
	printf("Sampling: %d Hz\n", pcm.sampling);
	print_codecs(path, &err);
	printf("Selected codec: %s\n", pcm.codec);
	printf("Delay: %#.1f ms\n", (double)pcm.delay / 10);
	printf("SoftVolume: %s\n", pcm.soft_volume ? "Y" : "N");
	print_volume(&pcm);
	print_mute(&pcm);

	if (dbus_error_is_set(&err))
		warn("Unable to read available codecs: %s", err.message);

	return EXIT_SUCCESS;
}

static int cmd_codec(int argc, char *argv[]) {

	if (argc < 2 || argc > 3) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		print_codecs(path, &err);
		printf("Selected codec: %s\n", pcm.codec);
		return EXIT_SUCCESS;
	}

	DBusMessage *msg = NULL, *rep = NULL;
	const char *codec = argv[2];

	if ((msg = dbus_message_new_method_call(dbus_ctx.ba_service, path,
					BLUEALSA_INTERFACE_PCM, "SelectCodec")) == NULL) {
		cmd_print_error("%s", strerror(ENOMEM));
		return EXIT_FAILURE;
	}

	DBusMessageIter iter;
	dbus_message_iter_init_append(msg, &iter);

	if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &codec)) {
		cmd_print_error("%s", strerror(ENOMEM));
		dbus_message_unref(msg);
		return EXIT_FAILURE;
	}

	DBusMessageIter props;
	if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &props)) {
		cmd_print_error("%s", strerror(ENOMEM));
		dbus_message_unref(msg);
		return EXIT_FAILURE;
	}

	dbus_message_iter_close_container(&iter, &props);

	if ((rep = dbus_connection_send_with_reply_and_block(dbus_ctx.conn,
					msg, DBUS_TIMEOUT_USE_DEFAULT, &err)) == NULL) {
		cmd_print_error("Couldn't select BlueALSA PCM Codec: %s", err.message);
		dbus_message_unref(msg);
		return EXIT_FAILURE;
	}

	dbus_message_unref(rep);
	dbus_message_unref(msg);
	return EXIT_SUCCESS;
}

static int cmd_volume(int argc, char *argv[]) {

	if (argc < 2 || argc > 4) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		print_volume(&pcm);
		return EXIT_SUCCESS;
	}

	int vol1, vol2;
	vol1 = vol2 = atoi(argv[2]);
	if (argc == 4)
		vol2 = atoi(argv[3]);

	if (pcm.transport & BA_PCM_TRANSPORT_MASK_A2DP) {
		if (vol1 < 0 || vol1 > 127) {
			cmd_print_error("Invalid volume [0, 127]: %d", vol1);
			return EXIT_FAILURE;
		}
		pcm.volume.ch1_volume = vol1;
		if (pcm.channels == 2) {
			if (vol2 < 0 || vol2 > 127) {
				cmd_print_error("Invalid volume [0, 127]: %d", vol2);
				return EXIT_FAILURE;
			}
			pcm.volume.ch2_volume = vol2;
		}
	}
	else {
		if (vol1 < 0 || vol1 > 15) {
			cmd_print_error("Invalid volume [0, 15]: %d", vol1);
			return EXIT_FAILURE;
		}
		pcm.volume.ch1_volume = vol1;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume loudness update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_mute(int argc, char *argv[]) {

	if (argc < 2 || argc > 4) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		print_mute(&pcm);
		return EXIT_SUCCESS;
	}

	pcm.volume.ch1_muted = pcm.volume.ch2_muted = false;

	if (strcasecmp(argv[2], "y") == 0)
		pcm.volume.ch1_muted = pcm.volume.ch2_muted = true;
	else if (strcasecmp(argv[2], "n") != 0) {
		cmd_print_error("Invalid argument [y|n]: %s", argv[2]);
		return EXIT_FAILURE;
	}

	if (pcm.channels == 2 && argc == 4) {
		if (strcasecmp(argv[3], "y") == 0)
			pcm.volume.ch2_muted = true;
		else if (strcasecmp(argv[3], "n") != 0) {
			cmd_print_error("Invalid argument [y|n]: %s", argv[3]);
			return EXIT_FAILURE;
		}
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_VOLUME, &err)) {
		cmd_print_error("Volume mute update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_softvol(int argc, char *argv[]) {

	if (argc < 2 || argc > 3) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	struct ba_pcm pcm;
	if (!get_pcm(path, &pcm)) {
		cmd_print_error("Invalid BlueALSA PCM path: %s", path);
		return EXIT_FAILURE;
	}

	if (argc == 2) {
		printf("SoftVolume: %c\n", pcm.soft_volume ? 'Y' : 'N');
		return EXIT_SUCCESS;
	}

	if (strcasecmp(argv[2], "y") == 0)
		pcm.soft_volume = true;
	else if (strcasecmp(argv[2], "n") == 0)
		pcm.soft_volume = false;
	else {
		cmd_print_error("Invalid argument [y|n]: %s", argv[2]);
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_pcm_update(&dbus_ctx, &pcm, BLUEALSA_PCM_SOFT_VOLUME, &err)) {
		cmd_print_error("SoftVolume update failed: %s", err.message);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}

static int cmd_open(int argc, char *argv[]) {

	if (argc != 2) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	const char *path = argv[1];
	if (!dbus_validate_path(path, NULL)) {
		cmd_print_error("Invalid PCM path: %s", path);
		return EXIT_FAILURE;
	}

	int fd_pcm, fd_pcm_ctrl, input, output;
	size_t len = strlen(path);

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_open_pcm(&dbus_ctx, path, &fd_pcm, &fd_pcm_ctrl, &err)) {
		cmd_print_error("Cannot open PCM: %s", err.message);
		return EXIT_FAILURE;
	}

	if (strcmp(path + len - strlen("source"), "source") == 0) {
		input = fd_pcm;
		output = STDOUT_FILENO;
	}
	else {
		input = STDIN_FILENO;
		output = fd_pcm;
	}

	ssize_t count;
	char buffer[4096];
	while ((count = read(input, buffer, sizeof(buffer))) > 0) {
		ssize_t written = 0;
		const char *pos = buffer;
		while (written < count) {
			ssize_t res = write(output, pos, count - written);
			if (res <= 0) {
				/* Cannot write any more, so just terminate */
				goto finish;
			}
			written += res;
			pos += res;
		}
	}

	if (output == fd_pcm)
		bluealsa_dbus_pcm_ctrl_send_drain(fd_pcm_ctrl, &err);

finish:
	close(fd_pcm);
	close(fd_pcm_ctrl);
	return EXIT_SUCCESS;
}

static DBusHandlerResult dbus_signal_handler(DBusConnection *conn, DBusMessage *message, void *data) {
	(void)conn;
	(void)data;

	if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char *interface = dbus_message_get_interface(message);
	const char *signal = dbus_message_get_member(message);

	DBusMessageIter iter;
	const char *path;

	if (strcmp(interface, BLUEALSA_INTERFACE_MANAGER) == 0) {

		if (strcmp(signal, "PCMAdded") == 0)
			if (dbus_message_iter_init(message, &iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
				dbus_message_iter_get_basic(&iter, &path);
				printf("PCMAdded %s\n", path);
				return DBUS_HANDLER_RESULT_HANDLED;
			}

		if (strcmp(signal, "PCMRemoved") == 0)
			if (dbus_message_iter_init(message, &iter) &&
					dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_OBJECT_PATH) {
				dbus_message_iter_get_basic(&iter, &path);
				printf("PCMRemoved %s\n", path);
				return DBUS_HANDLER_RESULT_HANDLED;
			}

	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int cmd_monitor(int argc, char *argv[]) {

	if (argc != 1) {
		cmd_print_error("Invalid number of arguments");
		return EXIT_FAILURE;
	}

	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMAdded", NULL);
	bluealsa_dbus_connection_signal_match_add(&dbus_ctx,
			dbus_ba_service, NULL, BLUEALSA_INTERFACE_MANAGER, "PCMRemoved", NULL);

	if (!dbus_connection_add_filter(dbus_ctx.conn, dbus_signal_handler, NULL, NULL)) {
		cmd_print_error("Couldn't add D-Bus filter");
		return EXIT_FAILURE;
	}

	while (dbus_connection_read_write_dispatch(dbus_ctx.conn, -1))
		continue;

	return EXIT_SUCCESS;
}

static struct command {
	const char *name;
	int (*func)(int argc, char *arg[]);
	const char *args;
	const char *help;
} commands[] = {
	{ "list-pcms", cmd_list_pcms, "", "List all PCM paths" },
	{ "list-services", cmd_list_services, "", "List all bluealsa services" },
	{ "status", cmd_status, "", "Show service runtime configuration" },
	{ "info", cmd_info, "<pcm-path>", "Show PCM properties etc" },
	{ "codec", cmd_codec, "<pcm-path> [<codec>]", "Change codec used by PCM" },
	{ "volume", cmd_volume, "<pcm-path> [<val>] [<val>]", "Set audio volume" },
	{ "mute", cmd_mute, "<pcm-path> [y|n] [y|n]", "Mute/unmute audio" },
	{ "soft-volume", cmd_softvol, "<pcm-path> [y|n]", "Enable/disable SoftVolume property" },
	{ "monitor", cmd_monitor, "", "Display PCMAdded & PCMRemoved signals" },
	{ "open", cmd_open, "<pcm-path>", "Transfer raw PCM via stdin or stdout" },
};

static void usage(const char *progname) {
	printf("%s - Utility to issue BlueALSA API commands\n", progname);
	printf("\nUsage:\n  %s [options] <command> [command-args]\n", progname);
	printf("\nOptions:\n");
	printf("  -h, --help          Show this help\n");
	printf("  -V, --version       Show version\n");
	printf("  -B, --dbus=NAME     BlueALSA service name suffix\n");
	printf("  -q, --quiet         Do not print any error messages\n");
	printf("\nCommands:\n");
	size_t i;
	for (i = 0; i < ARRAYSIZE(commands); i++)
		printf("  %-13s%-27s%s\n", commands[i].name, commands[i].args, commands[i].help);
	printf("\nNotes:\n");
	printf("   1. <pcm-path> must be a valid BlueALSA PCM path as returned by "
	       "the list-pcms command.\n");
	printf("   2. For commands that accept optional arguments, if no such "
	       "argument is given then the current status of the associated "
	       "attribute is printed.\n");
	printf("   3. The codec command requires BlueZ version >= 5.52 "
	       "for SEP support.\n");
}

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "+B:Vhq";
	const struct option longopts[] = {
		{"dbus", required_argument, NULL, 'B'},
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{"quiet", no_argument, NULL, 'q'},
		{ 0 },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			usage(argv[0]);
			return EXIT_SUCCESS;
		case 'V' /* --version */ :
			printf("%s\n", PACKAGE_VERSION);
			return EXIT_SUCCESS;
		case 'B' /* --dbus=NAME */ :
			snprintf(dbus_ba_service, sizeof(dbus_ba_service), BLUEALSA_SERVICE ".%s", optarg);
			break;
		case 'q' /* --quiet */ :
			quiet = true;
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return EXIT_FAILURE;
		}

	log_open(argv[0], false, false);
	dbus_threads_init_default();

	if (argc == optind) {
		cli_print_error("Missing command");
		return EXIT_FAILURE;
	}

	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_connection_ctx_init(&dbus_ctx, dbus_ba_service, &err)) {
		cli_print_error("Couldn't initialize D-Bus context: %s", err.message);
		return EXIT_FAILURE;
	}

	size_t i;
	for (i = 0; i < ARRAYSIZE(commands); i++)
		if (strcmp(argv[optind], commands[i].name) == 0)
			return commands[i].func(argc - optind, &argv[optind]);

	cli_print_error("Invalid command: %s", argv[optind]);
	return EXIT_FAILURE;
}
