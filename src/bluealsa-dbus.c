/*
 * BlueALSA - bluealsa-dbus.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "bluealsa-dbus.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bluetooth/hci.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-object.h>
#include <glib.h>

#include "a2dp.h"
#include "a2dp-codecs.h"
#include "ba-adapter.h"
#include "ba-device.h"
#include "bluealsa-iface.h"
#include "bluealsa.h"
#include "codec-sbc.h"
#include "dbus.h"
#include "hfp.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/log.h"

static GVariant *ba_variant_new_device_path(const struct ba_device *d) {
	return g_variant_new_object_path(d->bluez_dbus_path);
}

static GVariant *ba_variant_new_device_sequence(const struct ba_device *d) {
	return g_variant_new_uint32(d->seq);
}

static GVariant *ba_variant_new_device_battery(const struct ba_device *d) {
	return g_variant_new_byte(d->battery_level);
}

static GVariant *ba_variant_new_transport_type(const struct ba_transport *t) {
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE)
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_A2DP_SOURCE);
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SINK)
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_A2DP_SINK);
	if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_AG)
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_HFP_AG);
	if (t->type.profile & BA_TRANSPORT_PROFILE_HFP_HF)
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_HFP_HF);
	if (t->type.profile & BA_TRANSPORT_PROFILE_HSP_AG)
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_HSP_AG);
	if (t->type.profile & BA_TRANSPORT_PROFILE_HSP_HS)
		return g_variant_new_string(BLUEALSA_TRANSPORT_TYPE_HSP_HS);
	warn("Unsupported transport type: %#x", t->type.profile);
	return g_variant_new_string("<null>");
}

static GVariant *ba_variant_new_rfcomm_features(const struct ba_rfcomm *r) {
	return g_variant_new_uint32(r->hfp_features);
}

static GVariant *ba_variant_new_pcm_mode(const struct ba_transport_pcm *pcm) {
	if (pcm->mode == BA_TRANSPORT_PCM_MODE_SOURCE)
		return g_variant_new_string(BLUEALSA_PCM_MODE_SOURCE);
	return g_variant_new_string(BLUEALSA_PCM_MODE_SINK);
}

static GVariant *ba_variant_new_pcm_format(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint16(pcm->format);
}

static GVariant *ba_variant_new_pcm_channels(const struct ba_transport_pcm *pcm) {
	return g_variant_new_byte(pcm->channels);
}

static GVariant *ba_variant_new_pcm_sampling(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint32(pcm->sampling);
}

static GVariant *ba_variant_new_pcm_codec(const struct ba_transport_pcm *pcm) {
	const struct ba_transport *t = pcm->t;
	const char *codec = NULL;
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP)
		codec = ba_transport_codecs_a2dp_to_string(t->type.codec);
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO)
		codec = ba_transport_codecs_hfp_to_string(t->type.codec);
	if (codec != NULL)
		return g_variant_new_string(codec);
	return g_variant_new_string("<null>");
}

static GVariant *ba_variant_new_pcm_delay(const struct ba_transport_pcm *pcm) {
	return g_variant_new_uint16(ba_transport_pcm_get_delay(pcm));
}

static GVariant *ba_variant_new_pcm_soft_volume(const struct ba_transport_pcm *pcm) {
	return g_variant_new_boolean(pcm->soft_volume);
}

static uint8_t ba_volume_pack_dbus_volume(bool muted, int value) {
	return (muted << 7) | (((uint8_t)value) & 0x7F);
}

static GVariant *ba_variant_new_pcm_volume(const struct ba_transport_pcm *pcm) {
	uint8_t ch1 = ba_volume_pack_dbus_volume(pcm->volume[0].muted,
			ba_transport_pcm_volume_level_to_bt(pcm, pcm->volume[0].level));
	uint8_t ch2 = ba_volume_pack_dbus_volume(pcm->volume[1].muted,
			ba_transport_pcm_volume_level_to_bt(pcm, pcm->volume[1].level));
	return g_variant_new_uint16((ch1 << 8) | (pcm->channels == 1 ? 0 : ch2));
}

static void ba_variant_populate_pcm(GVariantBuilder *props, const struct ba_transport_pcm *pcm) {
	g_variant_builder_init(props, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(props, "{sv}", "Device", ba_variant_new_device_path(pcm->t->d));
	g_variant_builder_add(props, "{sv}", "Sequence", ba_variant_new_device_sequence(pcm->t->d));
	g_variant_builder_add(props, "{sv}", "Transport", ba_variant_new_transport_type(pcm->t));
	g_variant_builder_add(props, "{sv}", "Mode", ba_variant_new_pcm_mode(pcm));
	g_variant_builder_add(props, "{sv}", "Format", ba_variant_new_pcm_format(pcm));
	g_variant_builder_add(props, "{sv}", "Channels", ba_variant_new_pcm_channels(pcm));
	g_variant_builder_add(props, "{sv}", "Sampling", ba_variant_new_pcm_sampling(pcm));
	g_variant_builder_add(props, "{sv}", "Codec", ba_variant_new_pcm_codec(pcm));
	g_variant_builder_add(props, "{sv}", "Delay", ba_variant_new_pcm_delay(pcm));
	g_variant_builder_add(props, "{sv}", "SoftVolume", ba_variant_new_pcm_soft_volume(pcm));
	g_variant_builder_add(props, "{sv}", "Volume", ba_variant_new_pcm_volume(pcm));
}

static bool ba_variant_populate_sep(GVariantBuilder *props, const struct a2dp_sep *sep) {

	const struct a2dp_codec *codec;
	if ((codec = a2dp_codec_lookup(sep->codec_id, !sep->dir)) == NULL)
		return false;

	uint8_t caps[32];
	size_t size = MIN(sep->capabilities_size, sizeof(caps));
	if (a2dp_filter_capabilities(codec, memcpy(caps, sep->capabilities, size), size) != 0) {
		error("Couldn't filter %s capabilities: %s",
				ba_transport_codecs_a2dp_to_string(sep->codec_id),
				strerror(errno));
		return false;
	}

	g_variant_builder_init(props, G_VARIANT_TYPE("a{sv}"));

	GVariantBuilder vcaps;
	g_variant_builder_init(&vcaps, G_VARIANT_TYPE("ay"));

	size_t i;
	for (i = 0; i < size; i++)
		g_variant_builder_add(&vcaps, "y", caps[i]);

	g_variant_builder_add(props, "{sv}", "Capabilities", g_variant_builder_end(&vcaps));

	switch (codec->codec_id) {
	case A2DP_CODEC_SBC:
		break;
#if ENABLE_MPEG
	case A2DP_CODEC_MPEG12:
		break;
#endif
#if ENABLE_AAC
	case A2DP_CODEC_MPEG24:
		break;
#endif
#if ENABLE_APTX
	case A2DP_CODEC_VENDOR_APTX:
		break;
#endif
#if ENABLE_APTX_HD
	case A2DP_CODEC_VENDOR_APTX_HD:
		break;
#endif
#if ENABLE_FASTSTREAM
	case A2DP_CODEC_VENDOR_FASTSTREAM:
		break;
#endif
#if ENABLE_LDAC
	case A2DP_CODEC_VENDOR_LDAC:
		break;
#endif
	default:
		g_assert_not_reached();
	}

	return true;
}

static void bluealsa_manager_get_pcms(GDBusMethodInvocation *inv) {

	GVariantBuilder pcms;
	g_variant_builder_init(&pcms, G_VARIANT_TYPE("a{oa{sv}}"));

	struct ba_adapter *a;
	size_t i;

	for (i = 0; i < HCI_MAX_DEV; i++) {

		if ((a = ba_adapter_lookup(i)) == NULL)
			continue;

		GHashTableIter iter_d, iter_t;
		GVariantBuilder props;
		struct ba_device *d;
		struct ba_transport *t;

		pthread_mutex_lock(&a->devices_mutex);
		g_hash_table_iter_init(&iter_d, a->devices);
		while (g_hash_table_iter_next(&iter_d, NULL, (gpointer)&d)) {

			pthread_mutex_lock(&d->transports_mutex);
			g_hash_table_iter_init(&iter_t, d->transports);
			while (g_hash_table_iter_next(&iter_t, NULL, (gpointer)&t)) {

				if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

					if (t->a2dp.pcm.ba_dbus_id != 0) {
						ba_variant_populate_pcm(&props, &t->a2dp.pcm);
						g_variant_builder_add(&pcms, "{oa{sv}}", t->a2dp.pcm.ba_dbus_path, &props);
						g_variant_builder_clear(&props);
					}

					if (t->a2dp.pcm_bc.ba_dbus_id != 0) {
						ba_variant_populate_pcm(&props, &t->a2dp.pcm_bc);
						g_variant_builder_add(&pcms, "{oa{sv}}", t->a2dp.pcm_bc.ba_dbus_path, &props);
						g_variant_builder_clear(&props);
					}

				}
				else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {

					ba_variant_populate_pcm(&props, &t->sco.spk_pcm);
					g_variant_builder_add(&pcms, "{oa{sv}}", t->sco.spk_pcm.ba_dbus_path, &props);
					g_variant_builder_clear(&props);

					ba_variant_populate_pcm(&props, &t->sco.mic_pcm);
					g_variant_builder_add(&pcms, "{oa{sv}}", t->sco.mic_pcm.ba_dbus_path, &props);
					g_variant_builder_clear(&props);

				}

			}

			pthread_mutex_unlock(&d->transports_mutex);
		}

		pthread_mutex_unlock(&a->devices_mutex);
		ba_adapter_unref(a);

	}

	g_dbus_method_invocation_return_value(inv, g_variant_new("(a{oa{sv}})", &pcms));
	g_variant_builder_clear(&pcms);
}

static void bluealsa_manager_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)params;
	(void)userdata;

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "GetPCMs",
			.handler = bluealsa_manager_get_pcms },
		{ NULL },
	};

	if (!g_dbus_dispatch_method_call(dispatchers, sender, path, interface, method, invocation))
		error("Couldn't dispatch D-Bus method call: %s.%s()", interface, method);

}

static GVariant *ba_variant_new_version(void) {
	static const char *version = PACKAGE_VERSION;
	return g_variant_new_string(version);
}

static GVariant *ba_variant_new_profiles(void) {
	const gchar *strv[8];
	int n = 0;
	if (config.enable.a2dp_source)
		strv[n++] = "A2DP-source";
	if (config.enable.a2dp_sink)
		strv[n++] = "A2DP-sink";
	if (config.enable.hfp_ofono)
		strv[n++] = "HFP-ofono";
	if (config.enable.hfp_hf)
		strv[n++] = "HFP-HF";
	if (config.enable.hfp_ag)
		strv[n++] = "HFP-AG";
	if (config.enable.hsp_hs)
		strv[n++] = "HSP-HS";
	if (config.enable.hsp_ag)
		strv[n++] = "HSP-AG";
	strv[n] = NULL;

	return g_variant_new_strv(strv, -1);
}

static GVariant *ba_variant_new_adapters(void) {
	const gchar *strv[HCI_MAX_DEV];

	int a, s = 0;
	for (a = 0; a < HCI_MAX_DEV; a++) {
		if (config.adapters[a] && config.adapters[a]->hci.name)
			strv[s++] = config.adapters[a]->hci.name;
	}
	strv[s] = NULL;

	return g_variant_new_strv(strv, -1);
}

static GVariant *ba_variant_new_adapterfilter(void) {
	return g_variant_new_strv((const gchar* const*)config.hci_filter->data, config.hci_filter->len);
}

static GVariant *ba_variant_new_hfp(void) {
	GVariantDict dict;
	/* max number of features is 12, plus NULL terminator */
	const gchar *strv[13];
	int index;

	g_variant_dict_init (&dict, NULL);

	if (config.enable.hfp_ag) {
		index = 0;
		if (config.hfp.features_sdp_ag & SDP_HFP_AG_FEAT_TWC)
			strv[index++] = "TWC";
		if (config.hfp.features_sdp_ag & SDP_HFP_AG_FEAT_ECNR)
			strv[index++] = "ECNR";
		if (config.hfp.features_sdp_ag & SDP_HFP_AG_FEAT_VREC)
			strv[index++] = "VREC";
		if (config.hfp.features_sdp_ag & SDP_HFP_AG_FEAT_RING)
			strv[index++] = "RING";
		if (config.hfp.features_sdp_ag & SDP_HFP_AG_FEAT_VTAG)
			strv[index++] = "VTAG";
		if (config.hfp.features_sdp_ag & SDP_HFP_AG_FEAT_WBAND)
			strv[index++] = "WBAND";
		strv[index] = NULL;
		g_variant_dict_insert_value(&dict, "FeaturesSDPAG",
				 g_variant_new_strv(strv, -1));

		index = 0;
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_3WC)
			strv[index++] = "3WC";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_ECNR)
			strv[index++] = "ECNR";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_VOICE)
			strv[index++] = "VOICE";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_RING)
			strv[index++] = "RING";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_VTAG)
			strv[index++] = "VTAG";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_REJECT)
			strv[index++] = "REJECT";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_ECS)
			strv[index++] = "ECS";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_ECC)
			strv[index++] = "ECC";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_EERC)
			strv[index++] = "EERC";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_CODEC)
			strv[index++] = "CODEC";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_HFIND)
			strv[index++] = "HFIND";
		if (config.hfp.features_rfcomm_ag & HFP_AG_FEAT_ESCO)
			strv[index++] = "ESCO";
		strv[index] = NULL;
		g_variant_dict_insert_value(&dict, "FeaturesRFCOMMAG",
				 g_variant_new_strv(strv, -1));
	}

	if (config.enable.hfp_hf) {
		index = 0;
		if (config.hfp.features_sdp_hf & SDP_HFP_HF_FEAT_ECNR)
			strv[index++] = "ECNR";
		if (config.hfp.features_sdp_hf & SDP_HFP_HF_FEAT_TWC)
			strv[index++] = "TWC";
		if (config.hfp.features_sdp_hf & SDP_HFP_HF_FEAT_CLI)
			strv[index++] = "CLI";
		if (config.hfp.features_sdp_hf & SDP_HFP_HF_FEAT_VREC)
			strv[index++] = "VREC";
		if (config.hfp.features_sdp_hf & SDP_HFP_HF_FEAT_VOLUME)
			strv[index++] = "VOLUME";
		if (config.hfp.features_sdp_hf & SDP_HFP_HF_FEAT_WBAND)
			strv[index++] = "WBAND";
		strv[index] = NULL;
		g_variant_dict_insert_value(&dict, "FeaturesSDPHF",
				 g_variant_new_strv(strv, -1));

		index = 0;
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_ECNR)
			strv[index++] = "ECNR";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_3WC)
			strv[index++] = "3WC";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_CLI)
			strv[index++] = "CLI";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_VOICE)
			strv[index++] = "VOICE";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_VOLUME)
			strv[index++] = "VOLUME";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_ECS)
			strv[index++] = "ECS";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_ECC)
			strv[index++] = "ECC";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_CODEC)
			strv[index++] = "CODEC";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_HFIND)
			strv[index++] = "HFIND";
		if (config.hfp.features_rfcomm_hf & HFP_HF_FEAT_ESCO)
			strv[index++] = "ESCO";
		strv[index] = NULL;
		g_variant_dict_insert_value(&dict, "FeaturesRFCOMMHF",
				g_variant_new_strv(strv, -1));
	}


	g_variant_dict_insert(&dict, "XAPLVendorID", "u", config.hfp.xapl_vendor_id);
	g_variant_dict_insert(&dict, "XAPLProductID", "u", config.hfp.xapl_product_id);
	g_variant_dict_insert(&dict, "XAPLSoftwareVersion", "s", config.hfp.xapl_software_version);
	g_variant_dict_insert(&dict, "XAPLProductName", "s", config.hfp.xapl_product_name);

	index = 0;
	if (config.hfp.xapl_features & XAPL_FEATURE_BATTERY)
		strv[index++] = "BATTERY";
	if (config.hfp.xapl_features & XAPL_FEATURE_DOCKING)
		strv[index++] = "DOCKING";
	if (config.hfp.xapl_features & XAPL_FEATURE_SIRI)
		strv[index++] = "SIRI";
	if (config.hfp.xapl_features & XAPL_FEATURE_DENOISE)
		strv[index++] = "DENOISE";
	strv[index] = NULL;
	g_variant_dict_insert_value(&dict, "XAPLFeatures",
			g_variant_new_strv(strv, -1));

	return g_variant_dict_end(&dict);
}

static GVariant *ba_variant_new_msbc(void) {
#if ENABLE_MSBC
	return g_variant_new_boolean(config.enable.hfp_hf ||
			config.enable.hfp_ag || config.enable.hfp_ofono);
#else
	return g_variant_new_boolean(FALSE);
#endif
}

static GVariant *ba_variant_new_a2dp(void) {
	GVariantDict dict;

	g_variant_dict_init (&dict, NULL);
	g_variant_dict_insert (&dict, "NativeVolume", "b", config.a2dp.volume);
	g_variant_dict_insert (&dict, "ForceMono", "b", config.a2dp.force_mono);
	g_variant_dict_insert (&dict, "Force44100", "b", config.a2dp.force_44100);
	g_variant_dict_insert (&dict, "KeepAlive", "i", config.a2dp.keep_alive);

	return g_variant_dict_end(&dict);
}

static GVariant *ba_variant_new_sbc_quality(void) {
	const char *quality;
	switch (config.sbc_quality) {
		case SBC_QUALITY_LOW:
			quality = "LOW";
			break;
		case SBC_QUALITY_MEDIUM:
			quality = "MEDIUM";
			break;
		case SBC_QUALITY_HIGH:
			quality = "HIGH";
			break;
		case SBC_QUALITY_XQ:
			quality = "XQ";
			break;
		default:
			return NULL;
	}
	return g_variant_new_string(quality);
}

static GVariant *ba_variant_new_aac(void) {
	GVariantDict dict;

	g_variant_dict_init (&dict, NULL);
#if ENABLE_AAC
	g_variant_dict_insert (&dict, "Available", "b", TRUE);
	if (config.enable.a2dp_source) {
		g_variant_dict_insert (&dict, "Afterburner", "b", config.aac_afterburner);
		g_variant_dict_insert (&dict, "LATMVersion", "y", config.aac_latm_version);
		g_variant_dict_insert (&dict, "VBRMode", "y", config.aac_vbr_mode);
	}
#else
	g_variant_dict_insert (&dict, "Available", "b", FALSE);
#endif

	return g_variant_dict_end(&dict);
}

static GVariant *ba_variant_new_mpeg(void) {
	GVariantDict dict;

	g_variant_dict_init (&dict, NULL);
#if ENABLE_MPG123 || ENABLE_MP3LAME
	gboolean mpeg_available = config.enable.a2dp_sink;
#if ENABLE_MP3LAME
	if (config.enable.a2dp_source) {
		mpeg_available = TRUE;
		g_variant_dict_insert (&dict, "Quality", "y", config.lame_quality);
		g_variant_dict_insert (&dict, "VBRQuality", "y", config.lame_vbr_quality);
	}
#endif
	if (mpeg_available) {
		g_variant_dict_insert (&dict, "Available", "b", TRUE);
	}
#else
	g_variant_dict_insert (&dict, "Available", "b", FALSE);
#endif

	return g_variant_dict_end(&dict);
}

static GVariant *ba_variant_new_aptx(void) {
#if ENABLE_APTX
#if HAVE_APTX_DECODE
	return g_variant_new_boolean(config.enable.a2dp_source || config.enable.a2dp_sink);
#else
	return g_variant_new_boolean(config.enable.a2dp_source);
#endif
#else
	return g_variant_new_boolean(FALSE);
#endif
}
static GVariant *ba_variant_new_aptx_hd(void) {
#if ENABLE_APTX_HD
#if HAVE_APTX_HD_DECODE
	return g_variant_new_boolean(config.enable.a2dp_source || config.enable.a2dp_sink);
#else
	return g_variant_new_boolean(config.enable.a2dp_source);
#endif
#else
	return g_variant_new_boolean(FALSE);
#endif
}

static GVariant *ba_variant_new_ldac(void) {
	GVariantDict dict;

	g_variant_dict_init (&dict, NULL);
#if ENABLE_LDAC
#if HAVE_LDAC_DECODE
	g_variant_dict_insert (&dict, "Available", "b", config.enable.a2dp_source || config.enable.a2dp_sink);
#else
	g_variant_dict_insert (&dict, "Available", "b", config.enable.a2dp_source);
#endif
	if (config.enable.a2dp_source) {
		g_variant_dict_insert (&dict, "ABR", "b", config.ldac_abr);
		g_variant_dict_insert (&dict, "Eqmid", "y", config.ldac_eqmid);
	}
#else
	g_variant_dict_insert (&dict, "Available", "b", FALSE);
#endif

	return g_variant_dict_end(&dict);
}

static GVariant *ba_variant_new_battery(void) {
	GVariantDict dict;

	g_variant_dict_init (&dict, NULL);
	g_variant_dict_insert (&dict, "Available", "b", config.battery.available);
	if (config.battery.available)
		g_variant_dict_insert (&dict, "Level", "u", config.battery.level);

	return g_variant_dict_end(&dict);
}

static GVariant *bluealsa_manager_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *interface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;
	(void)userdata;

	if (strcmp(property, "Version") == 0)
		return ba_variant_new_version();

	if (strcmp(property, "Profiles") == 0)
		return ba_variant_new_profiles();

	if (strcmp(property, "Adapters") == 0)
		return ba_variant_new_adapters();

	if (strcmp(property, "AdapterFilter") == 0)
		return ba_variant_new_adapterfilter();

	if (strcmp(property, "HFP") == 0)
		return ba_variant_new_hfp();

	if (strcmp(property, "MSBC") == 0)
		return ba_variant_new_msbc();

	if (strcmp(property, "A2DP") == 0)
		return ba_variant_new_a2dp();

	if (strcmp(property, "SBCQuality") == 0)
		return ba_variant_new_sbc_quality();

	if (strcmp(property, "AAC") == 0)
		return ba_variant_new_aac();

	if (strcmp(property, "MPEG") == 0)
		return ba_variant_new_mpeg();

	if (strcmp(property, "LDAC") == 0)
		return ba_variant_new_ldac();

	if (strcmp(property, "APTX") == 0)
		return ba_variant_new_aptx();

	if (strcmp(property, "APTX-HD") == 0)
		return ba_variant_new_aptx_hd();

	if (strcmp(property, "Battery") == 0)
		return ba_variant_new_battery();

	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"Property not supported '%s'", property);
	return NULL;
}

/**
 * Register BlueALSA D-Bus manager interface. */
unsigned int bluealsa_dbus_manager_register(GError **error) {
	static const GDBusInterfaceVTable vtable = {
		.method_call = bluealsa_manager_method_call,
		.get_property = bluealsa_manager_get_property,
	 };
	return g_dbus_connection_register_object(config.dbus, "/org/bluealsa",
			(GDBusInterfaceInfo *)&bluealsa_iface_manager, &vtable, NULL, NULL, error);
}

static gboolean bluealsa_pcm_controller(GIOChannel *ch, GIOCondition condition,
		void *userdata) {
	(void)condition;

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	char command[32];
	size_t len;

	switch (g_io_channel_read_chars(ch, command, sizeof(command), &len, NULL)) {
	case G_IO_STATUS_ERROR:
		error("Couldn't read controller channel");
		return TRUE;
	case G_IO_STATUS_NORMAL:
		if (strncmp(command, BLUEALSA_PCM_CTRL_DRAIN, len) == 0) {
			if (pcm->mode == BA_TRANSPORT_PCM_MODE_SINK)
				ba_transport_pcm_drain(pcm);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_DROP, len) == 0) {
			if (pcm->mode == BA_TRANSPORT_PCM_MODE_SINK)
				ba_transport_pcm_drop(pcm);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_PAUSE, len) == 0) {
			ba_transport_pcm_pause(pcm);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else if (strncmp(command, BLUEALSA_PCM_CTRL_RESUME, len) == 0) {
			ba_transport_pcm_resume(pcm);
			g_io_channel_write_chars(ch, "OK", -1, &len, NULL);
		}
		else {
			warn("Invalid PCM control command: %*s", (int)len, command);
			g_io_channel_write_chars(ch, "Invalid", -1, &len, NULL);
		}
		g_io_channel_flush(ch, NULL);
		return TRUE;
	case G_IO_STATUS_AGAIN:
		return TRUE;
	case G_IO_STATUS_EOF:
		ba_transport_pcm_release(pcm);
		ba_transport_thread_send_signal(pcm->th, BA_TRANSPORT_SIGNAL_PCM_CLOSE);
		/* remove channel from watch */
		return FALSE;
	}

	return TRUE;
}

static void bluealsa_pcm_open(GDBusMethodInvocation *inv) {

	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	const bool is_sink = pcm->mode == BA_TRANSPORT_PCM_MODE_SINK;
	struct ba_transport_thread *th = pcm->th;
	struct ba_transport *t = pcm->t;
	int pcm_fds[4] = { -1, -1, -1, -1 };
	size_t i;

	/* Prevent two (or more) clients trying to
	 * open the same PCM at the same time. */
	pthread_mutex_lock(&pcm->dbus_mtx);

	/* We must ensure that transport release is not in progress before
	 * accessing transport critical section. Otherwise, we might have
	 * the IO thread closing it in the middle of the open procedure! */
	ba_transport_thread_cleanup_lock(th);

	/* preliminary check whether HFP codes is selected */
	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO &&
			t->type.codec == HFP_CODEC_UNDEFINED) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "HFP audio codec not selected");
		goto fail;
	}

	if (pcm->fd != -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "%s", strerror(EBUSY));
		goto fail;
	}

	/* create PCM stream PIPE and PCM control socket */
	if (pipe2(&pcm_fds[0], O_CLOEXEC) == -1 ||
			socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, &pcm_fds[2]) == -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "Create PIPE: %s", strerror(errno));
		goto fail;
	}

	/* set our internal endpoint as non-blocking. */
	if (fcntl(pcm_fds[is_sink ? 0 : 1], F_SETFL, O_NONBLOCK) == -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "Setup PIPE: %s", strerror(errno));
		goto fail;
	}

	/* Source profiles (A2DP Source and SCO Audio Gateway) should be initialized
	 * only if the audio is about to be transferred. It is most likely, that BT
	 * headset will not run voltage converter (power-on its circuit board) until
	 * the transport is acquired in order to extend battery life. For profiles
	 * like A2DP Sink and HFP headset, we will wait for incoming connection. */
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE ||
			t->type.profile & BA_TRANSPORT_PROFILE_MASK_AG)
		if (t->acquire(t) == -1) {
			g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
					G_DBUS_ERROR_FAILED, "Acquire transport: %s", strerror(errno));
			goto fail;
		}

	/* get correct PIPE endpoint - PIPE is unidirectional */
	pcm->fd = pcm_fds[is_sink ? 0 : 1];

	GIOChannel *ch = g_io_channel_unix_new(pcm_fds[2]);
	g_io_add_watch_full(ch, G_PRIORITY_DEFAULT, G_IO_IN,
			bluealsa_pcm_controller, ba_transport_pcm_ref(pcm),
			(GDestroyNotify)ba_transport_pcm_unref);
	g_io_channel_set_close_on_unref(ch, TRUE);
	g_io_channel_set_encoding(ch, NULL, NULL);
	g_io_channel_unref(ch);

	/* notify our audio thread that the FIFO is ready */
	ba_transport_thread_send_signal(th, BA_TRANSPORT_SIGNAL_PCM_OPEN);

	/* For source profiles (A2DP Source and SCO Audio Gateway) wait
	 * until the underlying IO thread is ready to process audio. */
	if (t->type.profile & BA_TRANSPORT_PROFILE_A2DP_SOURCE ||
			t->type.profile & BA_TRANSPORT_PROFILE_MASK_AG) {
		pthread_mutex_lock(&th->ready_mtx);
		while (!th->running)
			pthread_cond_wait(&th->ready, &th->ready_mtx);
		pthread_mutex_unlock(&th->ready_mtx);
	}

	ba_transport_thread_cleanup_unlock(th);
	pthread_mutex_unlock(&pcm->dbus_mtx);
	ba_transport_pcm_unref(pcm);

	int fds[2] = { pcm_fds[is_sink ? 1 : 0], pcm_fds[3] };
	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(fds, 2);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(hh)", 0, 1), fd_list);
	g_object_unref(fd_list);

	return;

fail:
	ba_transport_thread_cleanup_unlock(th);
	pthread_mutex_unlock(&pcm->dbus_mtx);
	ba_transport_pcm_unref(pcm);
	/* clean up created file descriptors */
	for (i = 0; i < ARRAYSIZE(pcm_fds); i++)
		if (pcm_fds[i] != -1)
			close(pcm_fds[i]);
}

static void bluealsa_pcm_get_codecs(GDBusMethodInvocation *inv) {

	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	const struct ba_transport *t = pcm->t;
	const GArray *seps = t->d->seps;

	GVariantBuilder codecs;
	g_variant_builder_init(&codecs, G_VARIANT_TYPE("a{sa{sv}}"));

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		size_t i;
		for (i = 0; seps != NULL && i < seps->len; i++) {
			const struct a2dp_sep *sep = &g_array_index(seps, struct a2dp_sep, i);
			/* match complementary PCM directions, e.g. A2DP-source with SEP-sink */
			if (t->a2dp.codec->dir == !sep->dir) {
				GVariantBuilder props;
				if (ba_variant_populate_sep(&props, sep)) {
					g_variant_builder_add(&codecs, "{sa{sv}}",
							ba_transport_codecs_a2dp_to_string(sep->codec_id), &props);
					g_variant_builder_clear(&props);
				}
			}
		}

	}
	else if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_SCO) {

		g_variant_builder_add(&codecs, "{sa{sv}}",
				ba_transport_codecs_hfp_to_string(HFP_CODEC_CVSD), NULL);

#if ENABLE_MSBC
		if (t->sco.rfcomm != NULL && t->sco.rfcomm->msbc)
			g_variant_builder_add(&codecs, "{sa{sv}}",
					ba_transport_codecs_hfp_to_string(HFP_CODEC_MSBC), NULL);
#endif

	}

	g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sa{sv}})", &codecs));
	g_variant_builder_clear(&codecs);

	ba_transport_pcm_unref(pcm);
}

static void bluealsa_pcm_select_codec(GDBusMethodInvocation *inv) {

	GVariant *params = g_dbus_method_invocation_get_parameters(inv);
	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	struct ba_transport *t = pcm->t;
	GVariantIter *properties;
	GVariant *value = NULL;
	const char *errmsg = NULL;
	const char *property;
	const char *codec;

	void *a2dp_configuration = NULL;
	size_t a2dp_configuration_size = 0;

	g_variant_get(params, "(sa{sv})", &codec, &properties);
	while (g_variant_iter_next(properties, "{&sv}", &property, &value)) {

		if (strcmp(property, "Configuration") == 0 &&
				g_variant_validate_value(value, G_VARIANT_TYPE_BYTESTRING, property)) {

			const void *data = g_variant_get_fixed_array(value,
					&a2dp_configuration_size, sizeof(char));

			g_free(a2dp_configuration);
			a2dp_configuration = g_memdup(data, a2dp_configuration_size);

		}

		g_variant_unref(value);
		value = NULL;
	}

	/* synchronize codec selection and e.g. PCM open */
	pthread_mutex_lock(&pcm->dbus_mtx);

	if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_A2DP) {

		/* support for Stream End-Points not enabled in BlueZ */
		if (t->d->seps == NULL) {
			errmsg = "No BlueZ SEP support";
			goto fail;
		}

		uint16_t codec_id = ba_transport_codecs_a2dp_from_string(codec);
		enum a2dp_dir dir = !t->a2dp.codec->dir;
		const GArray *seps = t->d->seps;
		struct a2dp_sep *sep = NULL;
		size_t i;

		for (i = 0; i < seps->len; i++)
			if (g_array_index(seps, struct a2dp_sep, i).dir == dir &&
					g_array_index(seps, struct a2dp_sep, i).codec_id == codec_id) {
				sep = &g_array_index(seps, struct a2dp_sep, i);
				break;
			}

		/* requested codec not available */
		if (sep == NULL) {
			errmsg = "SEP codec not available";
			goto fail;
		}

		const struct a2dp_codec *codec;
		if ((codec = a2dp_codec_lookup(codec_id, !dir)) == NULL) {
			errmsg = "SEP codec not supported";
			goto fail;
		}

		/* setup default codec configuration */
		memcpy(sep->configuration, sep->capabilities, sep->capabilities_size);
		if (a2dp_select_configuration(codec, sep->configuration, sep->capabilities_size) == -1)
			goto fail;

		/* use codec configuration blob provided by user */
		if (a2dp_configuration != NULL) {
			if (a2dp_check_configuration(codec, a2dp_configuration,
						a2dp_configuration_size) != A2DP_CHECK_OK) {
				errmsg = "Invalid configuration blob";
				goto fail;
			}
			memcpy(sep->configuration, a2dp_configuration, sep->capabilities_size);
		}

		if (ba_transport_select_codec_a2dp(t, sep) == -1)
			goto fail;

	}
	else {

		uint16_t codec_id = ba_transport_codecs_hfp_from_string(codec);
		if (ba_transport_select_codec_sco(t, codec_id) == -1)
			goto fail;

	}

	g_dbus_method_invocation_return_value(inv, NULL);
	goto final;

fail:
	if (errmsg == NULL)
		errmsg = strerror(errno);
	error("Couldn't select codec: %s: %s", codec, errmsg);
	g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
			G_DBUS_ERROR_FAILED, "%s", errmsg);

final:
	pthread_mutex_unlock(&pcm->dbus_mtx);
	ba_transport_pcm_unref(pcm);
	g_free(a2dp_configuration);
	g_variant_iter_free(properties);
	if (value != NULL)
		g_variant_unref(value);
}

static void bluealsa_pcm_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)params;

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "Open",
			.handler = bluealsa_pcm_open,
			.asynchronous_call = true },
		{ .method = "GetCodecs",
			.handler = bluealsa_pcm_get_codecs,
			.asynchronous_call = true },
		{ .method = "SelectCodec",
			.handler = bluealsa_pcm_select_codec,
			.asynchronous_call = true },
		{ NULL },
	};

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	ba_transport_pcm_ref(pcm);

	if (!g_dbus_dispatch_method_call(dispatchers, sender, path, interface, method, invocation)) {
		error("Couldn't dispatch D-Bus method call: %s.%s()", interface, method);
		ba_transport_pcm_unref(pcm);
	}

}

static void bluealsa_rfcomm_open(GDBusMethodInvocation *inv) {

	void *userdata = g_dbus_method_invocation_get_user_data(inv);
	struct ba_rfcomm *r = (struct ba_rfcomm *)userdata;
	int fds[2] = { -1, -1 };

	if (r->handler_fd != -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "%s", strerror(EBUSY));
		return;
	}

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, fds) == -1) {
		g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR,
				G_DBUS_ERROR_FAILED, "Create socket: %s", strerror(errno));
		return;
	}

	r->handler_fd = fds[0];
	ba_rfcomm_send_signal(r, BA_RFCOMM_SIGNAL_PING);

	GUnixFDList *fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	g_dbus_method_invocation_return_value_with_unix_fd_list(inv,
			g_variant_new("(h)", 0), fd_list);
	g_object_unref(fd_list);
}

static void bluealsa_rfcomm_method_call(GDBusConnection *conn, const char *sender,
		const char *path, const char *interface, const char *method, GVariant *params,
		GDBusMethodInvocation *invocation, void *userdata) {
	(void)conn;
	(void)params;
	(void)userdata;

	static const GDBusMethodCallDispatcher dispatchers[] = {
		{ .method = "Open",
			.handler = bluealsa_rfcomm_open },
		{ NULL },
	};

	if (!g_dbus_dispatch_method_call(dispatchers, sender, path, interface, method, invocation))
		error("Couldn't dispatch D-Bus method call: %s.%s()", interface, method);

}

static GVariant *bluealsa_pcm_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *interface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;
	struct ba_device *d = pcm->t->d;

	if (strcmp(property, "Device") == 0)
		return ba_variant_new_device_path(d);
	if (strcmp(property, "Sequence") == 0)
		return ba_variant_new_device_sequence(d);
	if (strcmp(property, "Transport") == 0)
		return ba_variant_new_transport_type(pcm->t);
	if (strcmp(property, "Mode") == 0)
		return ba_variant_new_pcm_mode(pcm);
	if (strcmp(property, "Format") == 0)
		return ba_variant_new_pcm_format(pcm);
	if (strcmp(property, "Channels") == 0)
		return ba_variant_new_pcm_channels(pcm);
	if (strcmp(property, "Sampling") == 0)
		return ba_variant_new_pcm_sampling(pcm);
	if (strcmp(property, "Codec") == 0)
		return ba_variant_new_pcm_codec(pcm);
	if (strcmp(property, "Delay") == 0)
		return ba_variant_new_pcm_delay(pcm);
	if (strcmp(property, "SoftVolume") == 0)
		return ba_variant_new_pcm_soft_volume(pcm);
	if (strcmp(property, "Volume") == 0)
		return ba_variant_new_pcm_volume(pcm);

	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"Property not supported '%s'", property);
	return NULL;
}

static GVariant *bluealsa_rfcomm_get_property(GDBusConnection *conn,
		const char *sender, const char *path, const char *interface,
		const char *property, GError **error, void *userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;

	struct ba_rfcomm *r = (struct ba_rfcomm *)userdata;
	struct ba_transport *t = r->sco;
	struct ba_device *d = t->d;

	if (strcmp(property, "Transport") == 0)
		return ba_variant_new_transport_type(t);
	if (strcmp(property, "Features") == 0)
		return ba_variant_new_rfcomm_features(r);
	if (strcmp(property, "Battery") == 0)
		return ba_variant_new_device_battery(d);

	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"Property not supported '%s'", property);
	return NULL;
}

static gboolean bluealsa_pcm_set_property(GDBusConnection *conn,
		const gchar *sender, const gchar *path, const gchar *interface,
		const gchar *property, GVariant *value, GError **error, gpointer userdata) {
	(void)conn;
	(void)sender;
	(void)path;
	(void)interface;

	struct ba_transport_pcm *pcm = (struct ba_transport_pcm *)userdata;

	if (strcmp(property, "SoftVolume") == 0) {
		pcm->soft_volume = g_variant_get_boolean(value);
		bluealsa_dbus_pcm_update(pcm, BA_DBUS_PCM_UPDATE_SOFT_VOLUME);
		return TRUE;
	}
	if (strcmp(property, "Volume") == 0) {

		uint16_t packed = g_variant_get_uint16(value);
		uint8_t ch1 = packed >> 8;
		uint8_t ch2 = packed & 0xFF;

		pcm->volume[0].level = ba_transport_pcm_volume_bt_to_level(pcm, ch1 & 0x7F);
		pcm->volume[0].muted = !!(ch1 & 0x80);
		pcm->volume[1].level = ba_transport_pcm_volume_bt_to_level(pcm, ch2 & 0x7F);
		pcm->volume[1].muted = !!(ch2 & 0x80);

		debug("Setting volume: %u [%.2f dB] %c%c %u [%.2f dB]",
				ch1 & 0x7F, 0.01 * pcm->volume[0].level, pcm->volume[0].muted ? 'x' : '<',
				pcm->volume[1].muted ? 'x' : '>', ch2 & 0x7F, 0.01 * pcm->volume[1].level);

		ba_transport_pcm_volume_update(pcm);
		return TRUE;
	}

	*error = g_error_new(G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"Property not supported '%s'", property);
	return FALSE;
}

/**
 * Register BlueALSA D-Bus PCM interface. */
unsigned int bluealsa_dbus_pcm_register(struct ba_transport_pcm *pcm, GError **error) {

	static const GDBusInterfaceVTable vtable = {
		.method_call = bluealsa_pcm_method_call,
		.get_property = bluealsa_pcm_get_property,
		.set_property = bluealsa_pcm_set_property,
	};

	if ((pcm->ba_dbus_id = g_dbus_connection_register_object(config.dbus,
					pcm->ba_dbus_path, (GDBusInterfaceInfo *)&bluealsa_iface_pcm, &vtable,
					pcm, (GDestroyNotify)ba_transport_pcm_unref, error)) != 0) {

		ba_transport_pcm_ref(pcm);

		GVariantBuilder props;
		ba_variant_populate_pcm(&props, pcm);

		g_dbus_connection_emit_signal(config.dbus, NULL,
				"/org/bluealsa", BLUEALSA_IFACE_MANAGER, "PCMAdded",
				g_variant_new("(oa{sv})", pcm->ba_dbus_path, &props), NULL);
		g_variant_builder_clear(&props);

	}

	return pcm->ba_dbus_id;
}

void bluealsa_dbus_pcm_update(struct ba_transport_pcm *pcm, unsigned int mask) {

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	if (mask & BA_DBUS_PCM_UPDATE_FORMAT)
		g_variant_builder_add(&props, "{sv}", "Format", ba_variant_new_pcm_format(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CHANNELS)
		g_variant_builder_add(&props, "{sv}", "Channels", ba_variant_new_pcm_channels(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_SAMPLING)
		g_variant_builder_add(&props, "{sv}", "Sampling", ba_variant_new_pcm_sampling(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_CODEC)
		g_variant_builder_add(&props, "{sv}", "Codec", ba_variant_new_pcm_codec(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_DELAY)
		g_variant_builder_add(&props, "{sv}", "Delay", ba_variant_new_pcm_delay(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_SOFT_VOLUME)
		g_variant_builder_add(&props, "{sv}", "SoftVolume", ba_variant_new_pcm_soft_volume(pcm));
	if (mask & BA_DBUS_PCM_UPDATE_VOLUME)
		g_variant_builder_add(&props, "{sv}", "Volume", ba_variant_new_pcm_volume(pcm));

	g_dbus_connection_emit_signal(config.dbus, NULL, pcm->ba_dbus_path,
			DBUS_IFACE_PROPERTIES, "PropertiesChanged",
			g_variant_new("(sa{sv}as)", BLUEALSA_IFACE_PCM, &props, NULL), NULL);

	g_variant_builder_clear(&props);
}

void bluealsa_dbus_pcm_unregister(struct ba_transport_pcm *pcm) {

	if (pcm->ba_dbus_id == 0)
		return;

	g_dbus_connection_unregister_object(config.dbus, pcm->ba_dbus_id);
	pcm->ba_dbus_id = 0;

	g_dbus_connection_emit_signal(config.dbus, NULL,
			"/org/bluealsa", BLUEALSA_IFACE_MANAGER, "PCMRemoved",
			g_variant_new("(o)", pcm->ba_dbus_path), NULL);

}

/**
 * Register BlueALSA D-Bus RFCOMM interface. */
unsigned int bluealsa_dbus_rfcomm_register(struct ba_rfcomm *r, GError **error) {

	static const GDBusInterfaceVTable vtable = {
		.method_call = bluealsa_rfcomm_method_call,
		.get_property = bluealsa_rfcomm_get_property,
	};

	r->ba_dbus_id = g_dbus_connection_register_object(config.dbus,
			r->ba_dbus_path, (GDBusInterfaceInfo *)&bluealsa_iface_rfcomm,
			&vtable, r, NULL, error);

	return r->ba_dbus_id;
}

void bluealsa_dbus_rfcomm_update(struct ba_rfcomm *r, unsigned int mask) {

	GVariantBuilder props;
	g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));

	if (mask & BA_DBUS_RFCOMM_UPDATE_FEATURES)
		g_variant_builder_add(&props, "{sv}", "Features", ba_variant_new_rfcomm_features(r));
	if (mask & BA_DBUS_RFCOMM_UPDATE_BATTERY)
		g_variant_builder_add(&props, "{sv}", "Battery", ba_variant_new_device_battery(r->sco->d));

	g_dbus_connection_emit_signal(config.dbus, NULL, r->ba_dbus_path,
			DBUS_IFACE_PROPERTIES, "PropertiesChanged",
			g_variant_new("(sa{sv}as)", BLUEALSA_IFACE_RFCOMM, &props, NULL), NULL);

	g_variant_builder_clear(&props);
}

void bluealsa_dbus_rfcomm_unregister(struct ba_rfcomm *r) {
	if (r->ba_dbus_id == 0)
		return;
	g_dbus_connection_unregister_object(config.dbus, r->ba_dbus_id);
	r->ba_dbus_id = 0;
}
