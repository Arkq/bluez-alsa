# BlueALSA - Makefile.am
# Copyright (c) 2016-2021 Arkadiusz Bokowy

if ENABLE_APLAY

bin_PROGRAMS = bluealsa-aplay

bluealsa_aplay_SOURCES = \
	../../src/shared/dbus-client.c \
	../../src/shared/ffb.c \
	../../src/shared/log.c \
	alsa-mixer.c \
	alsa-pcm.c \
	dbus.c \
	aplay.c

bluealsa_aplay_CFLAGS = \
	-I$(top_srcdir)/src \
	@ALSA_CFLAGS@ \
	@BLUEZ_CFLAGS@ \
	@DBUS1_CFLAGS@ \
	@LIBUNWIND_CFLAGS@

bluealsa_aplay_LDADD = \
	@ALSA_LIBS@ \
	@BLUEZ_LIBS@ \
	@DBUS1_LIBS@ \
	@LIBUNWIND_LIBS@

endif
