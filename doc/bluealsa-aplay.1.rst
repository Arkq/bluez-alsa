==============
bluealsa-aplay
==============

------------------------
a simple bluealsa player
------------------------

:Date: July 2021
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**bluealsa-aplay** [*OPTION*]... [*BT-ADDR*]...

DESCRIPTION
===========

Capture audio streams from Bluetooth devices (via ``bluealsa(8)``) and play them to an ALSA
playback device.

By default **bluealsa-aplay** captures audio from all connected Bluetooth devices.
It is possible to select specific Bluetooth devices by providing a list of *BT-ADDR* MAC
addresses.
Using the special MAC address **00:00:00:00:00:00** will disable device filtering - the
same as the default behavior.

OPTIONS
=======

-h, --help
    Output a usage message and exit.

-V, --version
    Output the version number and exit.

-v, --verbose
    Make the output more verbose.

-l, --list-devices
    List connected Bluetooth audio devices.

-L, --list-pcms
    List available Bluetooth audio PCMs on connected devices.

-B NAME, --dbus=NAME
    BlueALSA service name suffix.
    For more information see ``--dbus=NAME`` option of ``bluealsa(8)`` service daemon.

-D NAME, --pcm=NAME
    Select ALSA playback PCM device to use for audio output.
    The default is ``default``.

    **bluealsa-aplay** does not perform any mixing of streams. If multiple devices
    are connected it opens a new connection to the ALSA PCM device for each stream.
    Therefore the PCM *NAME* must itself allow multiple open connections and
    mix the streams together. See option **--single-audio** to change this
    behavior. Similarly, **bluealsa-aplay** does not apply any
    transformations to the stream. For this reason it is often necessary to use
    the ALSA **dmix** and **plug** plugins in the *NAME* PCM.

--pcm-buffer-time=INT
    Set the playback PCM buffer duration time to *INT* microseconds.
    The default is 500000.
    ALSA may choose the nearest available alternative if the requested value is
    not supported.

    If you experience underruns on the ALSA device then a larger buffer may
    help. However, a larger buffer will also increase the latency. For reliable
    performance the buffer time should be at least 3 times the period time.

--pcm-period-time=INT
    Set the playback PCM period duration time to *INT* microseconds.
    The default is 100000.
    ALSA may choose the nearest available alternative if the requested value is
    not supported.

    The ALSA **rate** plugin, which may be invoked by **plug**, does not always
    produce the exact required effective sample rate, especially with small
    period sizes. This can result in stream underruns (if the effective rate is
    too fast) or dropped A2DP frames in the **bluealsa(8)** server (if the
    effective rate is too slow). Increase the period time with this option if
    this problem occurs.

-M NAME, --mixer=NAME
    Select ALSA mixer device to use for controlling audio output.
    The default is ``default``.

--mixer-name=NAME
    Set the name of the mixer element.
    The default is ``Master``.

--mixer-index=NUM
    Set the index of the mixer channel.
    The default is ``0``.

--profile-a2dp
    Use A2DP profile (default).

--profile-sco
    Use SCO profile.

    Note: Only one of A2DP or SCO can be used. If both are specified, the
    last one given will be selected.

--single-audio
    Allow only one Bluetooth device to play audio at a time.
    If multiple devices are connected, only the first to start will play, the
    others will be paused. When that first device stops, then the next to send
    audio will be played.

    Without this option, **bluealsa-aplay** plays audio from all selected
    Bluetooth devices.
    Please note that playing from all Bluetooth devices at a time requires used
    PCM to be able to mix audio from multiple sources (i.e., it can be opened
    more than once; for example the ALSA **dmix** plugin).

SEE ALSO
========

``bluealsa(8)``, ``bluealsa-rfcomm(1)``

Project web site at https://github.com/Arkq/bluez-alsa

COPYRIGHT
=========

Copyright (c) 2016-2021 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.
