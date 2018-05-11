/*
 * BlueALSA - ctl.h
 * Copyright (c) 2016-2017 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#ifndef BLUEALSA_CTL_H_
#define BLUEALSA_CTL_H_

#include "bluealsa/ctl-proto.h"

int bluealsa_ctl_thread_init(void);
void bluealsa_ctl_free(void);

int bluealsa_ctl_event(enum event event);

#endif
