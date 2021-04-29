============
bluealsa-cli
============

----------------------------------------------------------
a simple command line interface for the BlueALSA D-Bus API
----------------------------------------------------------

:Date: February 2021
:Manual section: 1
:Manual group: General Commands Manual
:Version: $VERSION$

SYNOPSIS
========

**bluealsa-cli** [*OPTION*]... *COMMAND* [*ARG*]...

DESCRIPTION
===========

bluealsa-cli provides command-line access to the BlueALSA D-Bus API
"org.bluealsa.Manager1" and "org.bluealsa.PCM1" interfaces and thus
allows introspection and some control of BlueALSA PCMs while they are running.

The *PCM_PATH* command argument, where required, must be a BlueALSA PCM D-Bus
path.

OPTIONS
=======

-h, --help
    Output a usage message.

-V, --version
    Output the version number.

-B NAME, --dbus=NAME
    BlueALSA service name suffix. For more information see ``--dbus``
    option of ``bluealsa(8)`` service daemon.

-q, --quiet
    Do not print any error messages.

COMMANDS
========

list-pcms
    Print a list of BlueALSA PCM D-Bus paths, one per line.

list-services
    Print a list of all running BlueALSA D-Bus service names, one per line.

status
    Print BlueALSA daemon runtime properties. The properties include status and
    configuration, and are printed one per line, in the format
    'Property Name: Value'.

info *PCM_PATH*
    Print the properties and available codecs of the given PCM.
    The properties are printed one per line, in the format
    'PropertyName: Value'. Values are presented in human-readable format - for
    example the Volume property is printed as:

    ``Volume: L: 127 R: 127``

    The list of available codecs requires BlueZ SEP support (BlueZ >= 5.52)

codec *PCM_PATH* [*CODEC*]
    If *CODEC* is given, change the codec to be used by the given PCM. This
    command will terminate the PCM if it is currently running.

    If *CODEC* is not given, print a list of additional codecs supported by the
    given PCM and the currently selected codec.

    Selecting a codec and listing available codecs requires BlueZ SEP support
    (BlueZ >= 5.52).

volume *PCM_PATH* [*N*] [*N*]
    If *N* is given, set the loudness component of the volume property of the
    given PCM.

    If only one value *N* is given it is applied to all channels.
    For stereo (2-channel) PCMs the first value *N* is applied to channel 1
    (Left), and the second value *N* is applied to channel 2 (Right).
    For mono (1-channel) PCMs the second value *N* is ignored.

    Valid A2DP values for *N* are 0-127, valid HFP/HSP values are 0-15.

    If no *N* is given, print the current volume setting of the given PCM.

mute *PCM_PATH* [y|n] [y|n]
    If y|n argument(s) are given, set mute component of the volume property of
    the given PCM - 'y' mutes the volume, 'n' unmutes it. The second y|n
    argument is used for stereo PCMs as described for ``volume``.

    If no argument is given, print the current mute setting of the given PCM.

soft-volume *PCM_PATH* [y|n]
    If the y|n argument is given, set the SoftVolume property for the given PCM.
    This property determines whether BlueALSA will make volume control
    internally or will delegate this task to BlueALSA PCM client or connected
    Bluetooth device respectively for PCM sink or PCM source. The value 'y'
    enables SoftVolume, 'n' disables it.

    If no argument is given, print the current SoftVolume property of the given
    PCM.

monitor
    Listen for ``PCMAdded`` and ``PCMRemoved`` signals and print a message on
    standard output for each one received. Output lines are formed as:

    ``PCMAdded PCM_PATH``

    ``PCMRemoved PCM_PATH``

open *PCM_PATH*
    Transfer raw audio frames to or from the given PCM. For sink PCMs
    the frames are read from standard input and written to the PCM. For
    source PCMs the frames are read from the PCM and written to standard
    output. The format, channels and sampling rate must match the properties
    of the PCM, as no format conversions are performed by this tool.

SEE ALSO
========

``bluealsa(8)``, ``bluealsa-aplay(1)``, ``bluealsa-rfcomm(1)``

Project web site at https://github.com/Arkq/bluez-alsa

COPYRIGHT
=========

Copyright (c) 2016-2021 Arkadiusz Bokowy.

The bluez-alsa project is licensed under the terms of the MIT license.
