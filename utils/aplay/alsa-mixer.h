/*
 * BlueALSA - alsa-mixer.h
 * Copyright (c) 2016-2021 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_APLAY_ALSAMIXER_H_
#define BLUEALSA_APLAY_ALSAMIXER_H_

#include <alsa/asoundlib.h>

int alsa_mixer_open(snd_mixer_t **mixer, snd_mixer_elem_t **elem,
		const char *dev_name, const char *elem_name, unsigned int elem_idx,
		char **msg);

#endif
