/*
 * BlueALSA - alsa-mixer.c
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#include "alsa-mixer.h"

#include <stdio.h>
#include <string.h>

int alsa_mixer_open(snd_mixer_t **mixer, snd_mixer_elem_t **elem,
		const char *dev_name, const char *elem_name, unsigned int elem_idx,
		char **msg) {

	snd_mixer_t *_mixer = NULL;
	snd_mixer_elem_t *_elem;
	char buf[256];
	int err;

	snd_mixer_selem_id_t *id;
	snd_mixer_selem_id_alloca(&id);
	snd_mixer_selem_id_set_name(id, elem_name);
	snd_mixer_selem_id_set_index(id, elem_idx);

	if ((err = snd_mixer_open(&_mixer, 0)) != 0) {
		snprintf(buf, sizeof(buf), "Open mixer: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_attach(_mixer, dev_name)) != 0) {
		snprintf(buf, sizeof(buf), "Attach mixer: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_selem_register(_mixer, NULL, NULL)) != 0) {
		snprintf(buf, sizeof(buf), "Register mixer class: %s", snd_strerror(err));
		goto fail;
	}
	if ((err = snd_mixer_load(_mixer)) != 0) {
		snprintf(buf, sizeof(buf), "Load mixer elements: %s", snd_strerror(err));
		goto fail;
	}
	if ((_elem = snd_mixer_find_selem(_mixer, id)) == NULL) {
		snprintf(buf, sizeof(buf), "Mixer element not found");
		err = -1;
		goto fail;
	}

	*mixer = _mixer;
	*elem = _elem;
	return 0;

fail:
	if (_mixer != NULL)
		snd_mixer_close(_mixer);
	if (msg != NULL)
		*msg = strdup(buf);
	return err;
}
