/*
 * BlueALSA - sco.c
 * Copyright (c) 2016-2020 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "sco.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sco.h>

#include "a2dp-audio.h"
#include "ba-device.h"
#include "bluealsa.h"
#include "hci.h"
#include "hfp.h"
#include "msbc.h"
#include "utils.h"
#include "shared/defs.h"
#include "shared/ffb.h"
#include "shared/log.h"
#include "shared/rt.h"

#define SCO_DRAIN_TIMEOUT   250 /* milliseconds */
#define SCO_LINGER_TIMEOUT 1000 /* milliseconds */
#define SCO_CLOSE_TIMEOUT   600 /* milliseconds */

/**
 * SCO dispatcher internal data. */
struct sco_data {
	struct ba_adapter *a;
	struct pollfd pfd;
};

static void sco_dispatcher_cleanup(struct sco_data *data) {
	debug("SCO dispatcher cleanup: %s", data->a->hci.name);
	if (data->pfd.fd != -1)
		close(data->pfd.fd);
}

static int sco_release_bt(struct ba_transport *t) {
	int result;
	pthread_mutex_lock(&t->mutex);
	result = t->release(t);
	pthread_mutex_unlock(&t->mutex);
	return result;
}

static int sco_refresh_bt(struct ba_transport *t, int sco_fd) {
	pthread_mutex_lock(&t->mutex);
	/* make sure, we are not leaking file descriptor */
	t->release(t);
	t->bt_fd = sco_fd;
	t->mtu_read = t->mtu_write = hci_sco_get_mtu(sco_fd);
	pthread_mutex_unlock(&t->mutex);
	return t->bt_fd;
}

static void *sco_dispatcher_thread(struct ba_adapter *a) {

	struct sco_data data = { .a = a, .pfd = { -1, POLLIN, 0 } };

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(sco_dispatcher_cleanup), &data);

	if ((data.pfd.fd = hci_sco_open(data.a->hci.dev_id)) == -1) {
		error("Couldn't open SCO socket: %s", strerror(errno));
		goto fail;
	}

#if ENABLE_MSBC
	uint32_t defer = 1;
	if (setsockopt(data.pfd.fd, SOL_BLUETOOTH, BT_DEFER_SETUP, &defer, sizeof(defer)) == -1) {
		error("Couldn't set deferred connection setup: %s", strerror(errno));
		goto fail;
	}
#endif

	if (listen(data.pfd.fd, 10) == -1) {
		error("Couldn't listen on SCO socket: %s", strerror(errno));
		goto fail;
	}

	debug("Starting SCO dispatcher loop: %s", a->hci.name);
	for (;;) {

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		if (poll(&data.pfd, 1, -1) == -1) {
			if (errno == EINTR)
				continue;
			error("SCO dispatcher poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		struct sockaddr_sco addr;
		socklen_t addrlen = sizeof(addr);
		struct ba_device *d = NULL;
		struct ba_transport *t = NULL;
		int fd = -1;

		if ((fd = accept(data.pfd.fd, (struct sockaddr *)&addr, &addrlen)) == -1) {
			error("Couldn't accept incoming SCO link: %s", strerror(errno));
			goto cleanup;
		}

		debug("New incoming SCO link: %s: %d", batostr_(&addr.sco_bdaddr), fd);

		if ((d = ba_device_lookup(data.a, &addr.sco_bdaddr)) == NULL) {
			error("Couldn't lookup device: %s", batostr_(&addr.sco_bdaddr));
			goto cleanup;
		}

		if ((t = ba_transport_lookup(d, d->bluez_dbus_path)) == NULL) {
			error("Couldn't lookup transport: %s", d->bluez_dbus_path);
			goto cleanup;
		}

#if ENABLE_MSBC
		struct bt_voice voice = { .setting = BT_VOICE_TRANSPARENT };
		if (t->type.codec == HFP_CODEC_MSBC &&
				setsockopt(fd, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof(voice)) == -1) {
			error("Couldn't setup transparent voice: %s", strerror(errno));
			goto cleanup;
		}
		if (read(fd, &voice, 1) == -1) {
			error("Couldn't authorize SCO connection: %s", strerror(errno));
			goto cleanup;
		}
#endif

		sco_refresh_bt(t, fd);
		fd = -1;

		ba_transport_thread_send_signal(t->sco.spk_pcm.th, BA_TRANSPORT_SIGNAL_PING);
		ba_transport_thread_send_signal(t->sco.mic_pcm.th, BA_TRANSPORT_SIGNAL_PING);

cleanup:
		if (d != NULL)
			ba_device_unref(d);
		if (t != NULL)
			ba_transport_unref(t);
		if (fd != -1)
			close(fd);

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_pop(1);
	return NULL;
}

int sco_setup_connection_dispatcher(struct ba_adapter *a) {

	/* skip setup if dispatcher thread is already running */
	if (!pthread_equal(a->sco_dispatcher, config.main_thread))
		return 0;

	/* XXX: It is a known issue with Broadcom chips, that by default, the SCO
	 *      packets are routed via the chip's PCM interface. However, the IO
	 *      thread expects data to be available via the transport interface. */
	if (a->chip.manufacturer == BT_COMPID_BROADCOM) {

		int dd;
		uint8_t routing, clock, frame, sync, clk;

		debug("Checking Broadcom internal SCO routing");

		if ((dd = hci_open_dev(a->hci.dev_id)) == -1 ||
				hci_bcm_read_sco_pcm_params(dd, &routing, &clock, &frame, &sync, &clk, 1000) == -1)
			error("Couldn't read SCO routing params: %s", strerror(errno));
		else {
			debug("Current SCO interface setup: %u %u %u %u %u", routing, clock, frame, sync, clk);
			if (routing != BT_BCM_PARAM_ROUTING_TRANSPORT) {
				debug("Setting SCO routing via transport interface");
				if (hci_bcm_write_sco_pcm_params(dd, BT_BCM_PARAM_ROUTING_TRANSPORT,
						clock, frame, sync, clk, 1000) == -1)
				error("Couldn't write SCO routing params: %s", strerror(errno));
			}
		}

		if (dd != -1)
			hci_close_dev(dd);

	}

	int ret;

	/* Please note, that during the SCO dispatcher thread creation the adapter
	 * is not referenced. It is guaranteed that the adapter will be available
	 * during the whole live-span of the thread, because the thread is canceled
	 * in the adapter cleanup routine. See the ba_adapter_unref() function. */
	if ((ret = pthread_create(&a->sco_dispatcher, NULL,
					PTHREAD_ROUTINE(sco_dispatcher_thread), a)) != 0) {
		error("Couldn't create SCO dispatcher: %s", strerror(ret));
		a->sco_dispatcher = config.main_thread;
		return -1;
	}

	pthread_setname_np(a->sco_dispatcher, "ba-sco-dispatch");
	debug("Created SCO dispatcher [%s]: %s", "ba-sco-dispatch", a->hci.name);

	return 0;
}

static void sco_start_timer(struct ba_transport *t, long msec) {
	struct itimerspec timeout = {
		.it_interval = { 0 },
		.it_value = {
			.tv_sec = msec / 1000,
			.tv_nsec = (msec * 1000000) % 1000000000,
		},
	};
	timerfd_settime(t->sco.timer_fd, 0, &timeout, NULL);
}

static void sco_cancel_timer(struct ba_transport *t) {
	struct itimerspec timeout = { 0 };
	timerfd_settime(t->sco.timer_fd, 0, &timeout, NULL);
}

static bool sco_pcm_is_closed(struct ba_transport_pcm *pcm) {
	return pcm->fd == -1;
}

void *sco_thread(struct ba_transport_thread *th) {

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	pthread_cleanup_push(PTHREAD_CLEANUP(ba_transport_thread_cleanup), th);

	/* buffers for transferring data to and from SCO socket */
	ffb_t bt_in = { 0 };
	ffb_t bt_out = { 0 };
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt_in);
	pthread_cleanup_push(PTHREAD_CLEANUP(ffb_free), &bt_out);

#if ENABLE_MSBC
	struct esco_msbc msbc = { .initialized = false };
	pthread_cleanup_push(PTHREAD_CLEANUP(msbc_finish), &msbc);
	bool initialize_msbc = true;
#endif

	/* these buffers shall be bigger than the SCO MTU */
	if (ffb_init_uint8_t(&bt_in, 128) == -1 ||
			ffb_init_uint8_t(&bt_out, 128) == -1) {
		error("Couldn't create data buffer: %s", strerror(errno));
		goto fail_ffb;
	}

	struct ba_transport *t = th->t;
	struct asrsync asrs = { .frames = 0 };
	struct pollfd pfds[] = {
		{ th->pipe[0], POLLIN, 0 },
		/* SCO socket */
		{ -1, POLLIN, 0 },
		{ -1, POLLOUT, 0 },
		/* PCM FIFO */
		{ -1, POLLIN, 0 },
		{ -1, POLLOUT, 0 },
		/* Timer */
		{ t->sco.timer_fd, POLLIN , 0 },
	};

	debug("Starting SCO loop: %s", ba_transport_type_to_string(t->type));
	for (;;) {

		/* prevent an unexpected change of the codec value */
		const uint16_t codec = t->type.codec;

		/* fresh-start for file descriptors polling */
		pfds[1].fd = pfds[2].fd = -1;
		pfds[3].fd = pfds[4].fd = -1;

#if ENABLE_MSBC
		if (initialize_msbc && codec == HFP_CODEC_MSBC) {
			initialize_msbc = false;
			if (msbc_init(&msbc) != 0) {
				error("Couldn't initialize mSBC codec: %s", strerror(errno));
				goto fail;
			}
		}
#endif

		switch (codec) {
		case HFP_CODEC_CVSD:
		default:
			if (ffb_len_in(&bt_in) >= t->mtu_read)
				pfds[1].fd = t->bt_fd;
			if (ffb_len_out(&bt_out) >= t->mtu_write)
				pfds[2].fd = t->bt_fd;
			if (t->bt_fd != -1 && ffb_len_in(&bt_out) >= t->mtu_write)
				pfds[3].fd = t->sco.spk_pcm.fd;
			if (ffb_len_out(&bt_in) > 0)
				pfds[4].fd = t->sco.mic_pcm.fd;
			break;
#if ENABLE_MSBC
		case HFP_CODEC_MSBC:
			if (msbc_encode(&msbc) == -1)
				warn("Couldn't encode mSBC: %s", strerror(errno));
			if (msbc_decode(&msbc) == -1)
				warn("Couldn't decode mSBC: %s", strerror(errno));
			if (ffb_blen_in(&msbc.dec_data) >= t->mtu_read)
				pfds[1].fd = t->bt_fd;
			if (ffb_blen_out(&msbc.enc_data) >= t->mtu_write)
				pfds[2].fd = t->bt_fd;
			if (t->bt_fd != -1 && ffb_blen_in(&msbc.enc_pcm) >= t->mtu_write)
				pfds[3].fd = t->sco.spk_pcm.fd;
			if (ffb_blen_out(&msbc.dec_pcm) > 0)
				pfds[4].fd = t->sco.mic_pcm.fd;
			/* If SCO is not opened or PCM is not connected,
			 * mark mSBC encoder/decoder for reinitialization. */
			if ((t->sco.spk_pcm.fd == -1 && t->sco.mic_pcm.fd == -1) ||
					t->bt_fd == -1)
				initialize_msbc = true;
			break;
#endif
		}

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		if (poll(pfds, ARRAYSIZE(pfds), -1) <= 0) {
			if (errno == EINTR)
				continue;
			error("SCO poll error: %s", strerror(errno));
			goto fail;
		}

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		if (pfds[0].revents & POLLIN) {
			/* dispatch incoming event */
			switch (ba_transport_thread_recv_signal(th)) {
			case BA_TRANSPORT_SIGNAL_PING:
				continue;
			case BA_TRANSPORT_SIGNAL_PCM_OPEN:
			case BA_TRANSPORT_SIGNAL_PCM_RESUME:
				sco_cancel_timer(t);
				t->sco.state = BA_TRANSPORT_SCO_STATE_RUNNING;
				asrs.frames = 0;
				continue;
			case BA_TRANSPORT_SIGNAL_PCM_CLOSE:
				/* For Audio Gateway profile it is required to release SCO if we
				 * are not transferring audio (not sending nor receiving), because
				 * it will free Bluetooth bandwidth - headset will send microphone
				 * signal even though we are not reading it! */
				if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_AG &&
						      sco_pcm_is_closed(&t->sco.spk_pcm) &&
				              sco_pcm_is_closed(&t->sco.mic_pcm) &&
				              t->sco.state != BA_TRANSPORT_SCO_STATE_LINGER) {
					t->sco.state = BA_TRANSPORT_SCO_STATE_LINGER;
					sco_start_timer(t, SCO_LINGER_TIMEOUT);
				}
				continue;
			case BA_TRANSPORT_SIGNAL_PCM_SYNC:
				t->sco.state = BA_TRANSPORT_SCO_STATE_DRAINING;
				sco_start_timer(t, SCO_DRAIN_TIMEOUT);
				break;
			case BA_TRANSPORT_SIGNAL_PCM_DROP:
				sco_cancel_timer(t);
				ba_transport_pcm_flush(&t->sco.spk_pcm);
				continue;
			default:
				break;
			}
		}

		if (pfds[5].revents & POLLIN) {
			/* timer expired */
			uint64_t val;
			if (read(t->sco.timer_fd, &val, sizeof(val)) == -1) {
				sco_cancel_timer(t);
				debug("SCO timer failed");
				goto fail;
			}
			switch (t->sco.state) {
			case BA_TRANSPORT_SCO_STATE_DRAINING:
				/* drain is complete */
				t->sco.state = BA_TRANSPORT_SCO_STATE_RUNNING;
				pthread_cond_signal(&t->sco.spk_pcm.synced);
				continue;
			case BA_TRANSPORT_SCO_STATE_LINGER:
				/* If no new client has connected during the linger time,
				 * release the SCO connection */
				if (t->type.profile & BA_TRANSPORT_PROFILE_MASK_AG &&
						 sco_pcm_is_closed(&t->sco.spk_pcm) &&
				         sco_pcm_is_closed(&t->sco.mic_pcm)) {
					debug("Releasing SCO due to PCM inactivity");
					t->sco.state = BA_TRANSPORT_SCO_STATE_CLOSING;
					sco_release_bt(t);
					sco_start_timer(t, SCO_CLOSE_TIMEOUT);
				}
				continue;
			case BA_TRANSPORT_SCO_STATE_CLOSING:
				t->sco.state = BA_TRANSPORT_SCO_STATE_IDLE;
				break;
			default:
				break;
			}
		}

		if (asrs.frames == 0)
			asrsync_init(&asrs, t->sco.spk_pcm.sampling);

		if (pfds[1].revents & POLLIN) {
			/* dispatch incoming SCO data */

			uint8_t *buffer;
			size_t buffer_len;
			ssize_t len;

			switch (codec) {
			case HFP_CODEC_CVSD:
			default:
				if (t->sco.mic_pcm.fd == -1)
					ffb_rewind(&bt_in);
				buffer = bt_in.tail;
				buffer_len = ffb_len_in(&bt_in);
				break;
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				buffer = msbc.dec_data.tail;
				buffer_len = ffb_len_in(&msbc.dec_data);
				break;
#endif
			}

retry_sco_read:
			errno = 0;
			if ((len = read(pfds[1].fd, buffer, buffer_len)) <= 0)
				switch (errno) {
				case EINTR:
					goto retry_sco_read;
				case 0:
				case ECONNABORTED:
				case ECONNRESET:
					sco_release_bt(t);
					continue;
				default:
					error("SCO read error: %s", strerror(errno));
					continue;
				}

			/* If microphone (capture) PCM is not connected ignore incoming data. In
			 * the worst case scenario, we might lose few milliseconds of data (one
			 * mSBC frame which is 7.5 ms), but we will be sure, that the microphone
			 * latency will not build up. */
			if (t->sco.mic_pcm.fd != -1)
				switch (codec) {
				case HFP_CODEC_CVSD:
				default:
					ffb_seek(&bt_in, len);
					break;
#if ENABLE_MSBC
				case HFP_CODEC_MSBC:
					ffb_seek(&msbc.dec_data, len);
					break;
#endif
				}

		}
		else if (pfds[1].revents & (POLLERR | POLLHUP)) {
			debug("SCO poll error status: %#x", pfds[1].revents);
			sco_release_bt(t);
		}

		if (pfds[2].revents & POLLOUT) {
			/* write-out SCO data */

			uint8_t *buffer;
			size_t buffer_len;
			ssize_t len;

			switch (codec) {
			case HFP_CODEC_CVSD:
			default:
				buffer = bt_out.data;
				buffer_len = t->mtu_write;
				break;
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				buffer = msbc.enc_data.data;
				buffer_len = t->mtu_write;
				break;
#endif
			}

retry_sco_write:
			errno = 0;
			if ((len = write(pfds[2].fd, buffer, buffer_len)) <= 0)
				switch (errno) {
				case EINTR:
					goto retry_sco_write;
				case 0:
				case ECONNABORTED:
				case ECONNRESET:
					sco_release_bt(t);
					continue;
				default:
					error("SCO write error: %s", strerror(errno));
					continue;
				}

			switch (codec) {
			case HFP_CODEC_CVSD:
			default:
				ffb_shift(&bt_out, len);
				break;
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				ffb_shift(&msbc.enc_data, len);
				break;
#endif
			}

		}

		if (pfds[3].revents & POLLIN) {
			/* dispatch incoming PCM data */

			int16_t *buffer;
			ssize_t samples;

			switch (codec) {
			case HFP_CODEC_CVSD:
			default:
				buffer = (int16_t *)bt_out.tail;
				samples = ffb_len_in(&bt_out) / sizeof(int16_t);
				break;
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				buffer = msbc.enc_pcm.tail;
				samples = ffb_len_in(&msbc.enc_pcm);
				break;
#endif
			}

			if ((samples = ba_transport_pcm_read(&t->sco.spk_pcm, buffer, samples)) <= 0) {
				if (samples == -1 && errno != EAGAIN)
					error("PCM read error: %s", strerror(errno));
				if (samples == 0)
					ba_transport_thread_send_signal(th, BA_TRANSPORT_SIGNAL_PCM_CLOSE);
				continue;
			}

			switch (codec) {
			case HFP_CODEC_CVSD:
			default:
				ffb_seek(&bt_out, samples * sizeof(int16_t));
				break;
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				ffb_seek(&msbc.enc_pcm, samples);
				break;
#endif
			}

		}
		else if (pfds[3].revents & (POLLERR | POLLHUP)) {
			debug("PCM poll error status: %#x", pfds[3].revents);
			ba_transport_pcm_release(&t->sco.spk_pcm);
			ba_transport_thread_send_signal(th, BA_TRANSPORT_SIGNAL_PCM_CLOSE);
		}

		if (pfds[4].revents & POLLOUT) {
			/* write-out PCM data */

			int16_t *buffer;
			ssize_t samples;

			switch (codec) {
			case HFP_CODEC_CVSD:
			default:
				buffer = (int16_t *)bt_in.data;
				samples = ffb_len_out(&bt_in) / sizeof(int16_t);
				break;
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				buffer = msbc.dec_pcm.data;
				samples = ffb_len_out(&msbc.dec_pcm);
				break;
#endif
			}

			if ((samples = ba_transport_pcm_write(&t->sco.mic_pcm, buffer, samples)) <= 0) {
				if (samples == -1)
					error("FIFO write error: %s", strerror(errno));
				if (samples == 0)
					ba_transport_thread_send_signal(th, BA_TRANSPORT_SIGNAL_PCM_CLOSE);
			}

			switch (codec) {
			case HFP_CODEC_CVSD:
			default:
				ffb_shift(&bt_in, samples * sizeof(int16_t));
				break;
#if ENABLE_MSBC
			case HFP_CODEC_MSBC:
				ffb_shift(&msbc.dec_pcm, samples);
				break;
#endif
			}

		}

		/* keep data transfer at a constant bit rate */
		switch (codec) {
		case HFP_CODEC_CVSD:
		default:
			asrsync_sync(&asrs, t->mtu_write / sizeof(int16_t));
			break;
#if ENABLE_MSBC
		case HFP_CODEC_MSBC:
			if (msbc.enc_frames > 0) {
				asrsync_sync(&asrs, msbc.enc_frames * MSBC_CODESAMPLES);
				msbc.enc_frames = 0;
			}
#endif
		}

		/* update busy delay (encoding overhead) */
		const unsigned int delay = asrsync_get_busy_usec(&asrs) / 100;
		t->sco.spk_pcm.delay = t->sco.mic_pcm.delay = delay;

	}

fail:
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
fail_ffb:
#if ENABLE_MSBC
	pthread_cleanup_pop(1);
#endif
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}
