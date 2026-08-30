# Native FLEX / direct VITA-49 port into WSJT-Z

**Date:** 2026-08-30
**Status:** Approved, ready for implementation planning

## Purpose

Bring the W7PP "Native FLEX" feature — direct VITA-49 receive and transmit
audio plus a SmartSDR CAT backend — from *WSJT-X v3.0.2 W7PP Mods v1.05.8
RC3* into the WSJT-Z fork, and make it build and run on macOS and Linux as
well as Windows.

Today WSJT-Z has no FlexRadio-specific code at all. A Flex operator must run
SmartSDR DAX virtual audio devices and a separate CAT path. Native FLEX
removes both: WSJT-Z talks to the radio directly over the SmartSDR TCP API
and streams audio as VITA-49 UDP.

## Source material

| | |
|---|---|
| Donor | WSJT-X v3.0.2 W7PP Mods v1.05.8 RC3, `W7PP_Mods_v1.05.8_SOURCE_CLEAN_20260827_112353.zip` |
| Donor base | WSJT-X v3.0.2 |
| Recipient | WSJT-Z 2.0.19, base WSJT-X 3.0.0 |
| Licence | GPLv3, both sides |

W7PP modifications are Copyright (C) 2026 Dick Hale / W7PP, conveyed under
GPLv3. This port preserves that attribution.

## Scope

**In scope** — the full Native FLEX stack:

- `FlexVitaReceiver` — SmartSDR TCP control + DAX VITA-49 UDP receive
- VITA-49 transmit packetiser in `SoundOutput`
- `NativeFlexTransceiver` — CAT backend, selected as rig "Flex Native VITA-49"
- `NativeFlexSafetyMonitor` — PA/SWR/ATU telemetry and TX inhibit
- `NativeFlexRadioSelection` — radio discovery/selection dialog
- Configuration and main-window UI for all of the above

**Out of scope** — every other W7PP modification. W7PP also changes
`DecodeHighlightingModel`, `displaytext`, `widegraph`, `about`, and ships
contest tooling. None of that is part of this port. Only changes that serve
Native FLEX are carried over.

## Architecture

Native FLEX is a *third, optional* receive source, parallel to the two that
already exist. It is inert unless the operator selects it.

```
Standard:  SoundInput -> Detector          -> dec_data.d2 -> dataSink()
TCI:       TCI network audio               -> dec_data.d2 -> dataSink()
Native:    FlexVitaReceiver                -> dec_data.d2 -> dataSink()
```

### Receive

```
SmartSDR TCP :4992          DAX VITA-49 UDP :4995-5010
        |                            |
        +------ FlexVitaReceiver worker thread ------+
                             |
                48 kHz float samples
                             |
                127-tap anti-alias FIR
                             |
                    decimate 4:1
                             |
                12 kHz signed int16 blocks
                             |
              MainWindow::flexDataSink()
                             |
        dec_data.d2  ->  dataSink() at m_FFTSize boundaries
```

`flexDataSink` deliberately mirrors the stock `Detector` producer
semantics: it accumulates samples and calls `dataSink()` only on
`m_FFTSize` boundaries, and resets `dec_data.params.kin` on T/R period
wrap. This is the same contract TCI honours. Getting it wrong desynchronises
the decoder, so it is copied rather than reinvented.

An operator-adjustable RX gain (dB, ≤ 0) is applied during the copy. At
exactly 0 dB the code takes a plain copy path with no arithmetic, preserving
bit-exact behaviour.

### Transmit

```
Modulator -> SoundOutput (property W7PPNativeFlexTxCapture == true)
               |
    software 48 kHz TX clock (QTimer + QElapsedTimer pacing)
               |
      VITA-49 packetiser, sequence-numbered
               |
      QUdpSocket -> radio TX stream id
```

The TX stream id and radio address are published by `NativeFlexTransceiver`
as Qt properties on the application object and read by `SoundOutput` at
stream start. This keeps `SoundOutput` free of any direct dependency on the
transceiver class.

### Control and safety

`NativeFlexTransceiver` derives from `TransceiverBase` and drives frequency,
mode, PTT and RF power over the SmartSDR TCP API. `NativeFlexSafetyMonitor`
runs an independent telemetry connection and can raise a *recoverable* TX
inhibit.

That recoverability is why `TransceiverBase` needs a change: an error
beginning `"Native FLEX TX INHIBITED:"` must not call `offline()`, because
`offline()` calls `shutdown()` and would tear down a still-healthy RX path.
It is reported to `MainWindow` instead, and PTT is dropped.

## Portability

`FlexVitaReceiver.cpp` is the only file in the donor set that is not already
Qt-based. It runs a `std::thread` doing blocking `recv`/`recvfrom` on raw
Winsock sockets. Everything else — the CAT backend, the safety monitor, the
radio-selection dialog, the VITA TX path in `soundout.cpp` — is Qt and
compiles unchanged.

A new header, `Transceiver/FlexSocketCompat.hpp`, provides the mapping. The
receiver's threading and timing behaviour is preserved exactly; only the
socket vocabulary changes. This keeps the file diffable against future W7PP
releases.

### Mapping table

| Windows | POSIX |
|---|---|
| `<winsock2.h>`, `<ws2tcpip.h>` | `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<unistd.h>`, `<errno.h>` |
| `SOCKET` | `int` |
| `INVALID_SOCKET` | `-1` |
| `SOCKET_ERROR` | `-1` |
| `closesocket(s)` | `::close(s)` |
| `WSAStartup` / `WSACleanup` / `WSADATA` | no-ops |
| `WSAGetLastError()` | `errno` |
| `WSAETIMEDOUT`, `WSAEWOULDBLOCK` | `EAGAIN`, `EWOULDBLOCK`, `EINTR` |
| `shutdown(s, SD_BOTH)` | `shutdown(s, SHUT_RDWR)` |
| `SO_RCVTIMEO` as `DWORD` milliseconds | `SO_RCVTIMEO` as `struct timeval` |
| `recvfrom` length as `int` | `socklen_t` |

Call sites: 15, all inside `FlexVitaReceiver.cpp`.

### Three portability hazards

These are genuine behaviour differences, not mechanical substitutions.

1. **SIGPIPE.** On POSIX, `::send()` to a socket the radio has closed raises
   `SIGPIPE`, whose default disposition terminates the process. Windows has
   no equivalent. A Flex power-off mid-transmission would kill WSJT-Z.
   Fix: `MSG_NOSIGNAL` on the `send()` flags on Linux, `SO_NOSIGPIPE` set on
   the socket on macOS.

2. **`EINTR`.** Blocking `recv`/`recvfrom` on POSIX can return `EINTR` when a
   signal arrives. The Windows code has no such case, so an unhandled `EINTR`
   would be misread as a fatal socket error and drop the stream. It must be
   treated exactly like the timeout case: continue the loop.

3. **`SO_RCVBUF` doubling.** Linux doubles the requested receive buffer and
   reports the doubled value back, and clamps against
   `net.core.rmem_max`. The requested 1 MiB must be set with that in mind,
   and the result read back rather than assumed. An undersized buffer shows
   up as dropped VITA packets under load, which surfaces as decode loss
   rather than an error — worth a log line stating the value actually
   obtained.

## Files

### New

| File | Origin | Lines |
|---|---|---|
| `Transceiver/FlexVitaReceiver.hpp` | verbatim | 88 |
| `Transceiver/FlexVitaReceiver.cpp` | verbatim + shim edits | 1191 |
| `Transceiver/NativeFlexTransceiver.hpp` | verbatim | 90 |
| `Transceiver/NativeFlexTransceiver.cpp` | verbatim | 1601 |
| `Transceiver/NativeFlexSafetyMonitor.hpp` | verbatim | 245 |
| `Transceiver/NativeFlexSafetyMonitor.cpp` | verbatim | 1803 |
| `Transceiver/NativeFlexRadioSelection.hpp` | verbatim | 40 |
| `Transceiver/NativeFlexRadioSelection.cpp` | verbatim | 318 |
| `Transceiver/FlexSocketCompat.hpp` | new, written for this port | ~60 |

### Modified

| File | Change |
|---|---|
| `CMakeLists.txt` | add the four new `.cpp` files to the sources list |
| `Transceiver/TransceiverFactory.cpp` | `NativeFlexId` enum member; register `"Flex Native VITA-49"` with `Capabilities {NativeFlexId, none, true, false, false, true}`; construction case |
| `Transceiver/TransceiverBase.cpp` | recoverable handling of `"Native FLEX TX INHIBITED:"` |
| `Audio/soundout.h` | VITA TX member state: source device, pacing timers, packet queue, UDP socket, sequence, stream id, decimation phase |
| `Audio/soundout.cpp` | `nativeFlexPump`, `nativeFlexTxPace`, `nativeFlexWriteVitaPacket`, `nativeFlexCloseVitaDump`; native-flex branch in stream start |
| `Modulator/Modulator.cpp` | optional TX PCM capture to file, gated on the `W7PPNativeFlexTxCapture` property and `W7PP_NATIVE_FLEX_TX_CAPTURE_FILE` env var — a debug aid, off by default |
| `Configuration.hpp` | `flex_native_rx()`, `flex_dax_channel()` accessors |
| `Configuration.cpp` | member state, settings read/write, combo wiring, `update_w7pp_flex_rx_controls()`, DAX channel handoff before rig start, `W7PPNativeFlexTxCapture` property, radio-selection hook, audio-device validation bypass when native RX is active |
| `Configuration.ui` | `w7pp_flex_rx_method_combo_box` (Standard DAX / Native VITA-49) and `w7pp_flex_dax_channel_combo_box` on the Audio tab, with labels |
| `widgets/mainwindow.h` | 6 method declarations, ~12 members |
| `widgets/mainwindow.cpp` | 6 new methods; insertions into 19 existing ones |
| `widgets/mainwindow.ui` | `w7ppFlexTxAudioAttenuation` slider + scale labels; `w7ppFlexRxGainWidget` group |

### New settings keys

| Key | Type | Default |
|---|---|---|
| `W7PPFlexNativeRx` | bool | `false` |
| `W7PPFlexDaxChannel` | int, clamped 1–8 | `1` |

### MainWindow methods

New:

- `syncFlexVitaReceiver()` — start/stop the receiver to match configuration
- `verifyFlexVitaStart(...)` — confirm packets are arriving after start
- `armFlexVitaWatchdog(int generation)` — detect a stream that dies silently
- `flexDataSink(FlexVitaReceiver::DecoderBlock const&)` — the RX bridge
- `nativeFlexSafetyTrip(QString const& problems)` — handle a safety inhibit
- `nativeFlexSafetyProblemText(bool require_ready) const` — describe it

Modified: `MainWindow()`, `~MainWindow()`, `guiUpdate()`, `createStatusBar()`,
`readSettings()`, `writeSettings()`, `on_actionSettings_triggered()`,
`on_autoButton_clicked()`, `monitor()`, `stopTx()`, `stopTx2()`, `fixStop()`,
`on_outAttenuation_valueChanged()`, `handle_transceiver_failure()`,
`WSPR_scheduling()`, `on_tuneButton_clicked()`, `on_monitorButton_clicked()`,
`end_tuning()`, `band_changed()`.

All 19 exist in WSJT-Z and were verified present before this design was
written.

## Merge method

W7PP marks its work densely — 325 `W7PP` comment markers in `mainwindow.cpp`
alone — and every added symbol carries a `w7pp`, `Flex`, or `NativeFlex`
prefix. Ported hunks are located by those markers and hand-placed into the
WSJT-Z counterpart function.

Naming and markers are preserved verbatim. This keeps GPLv3 attribution
legible and allows re-diffing against future W7PP releases to pull in fixes.

Two alternatives were considered and rejected:

- *Patch from stock WSJT-X 3.0.2.* The upstream SourceForge git has no 3.0.x
  tag, and WSJT-Z is based on 3.0.0 rather than 3.0.2, so a stock-derived
  patch would not apply cleanly even if obtained.
- *Wholesale file replacement.* Would destroy WSJT-Z's own modifications.
  The two forks' `mainwindow.cpp` are 16k and 26k lines respectively; they
  have diverged far too much.

## Build sequence

Six stages, each independently compilable. A stage is not done until the
macOS build is clean.

1. **Foundation** — `FlexSocketCompat.hpp`; copy the eight donor files;
   apply shim edits to `FlexVitaReceiver.cpp`; add sources to CMake.
   Nothing references the new code yet; it must compile as dead weight.
2. **Transceiver plumbing** — `TransceiverFactory` registration and
   construction, `TransceiverBase` inhibit handling. The rig becomes
   selectable but does nothing.
3. **Configuration** — accessors, settings, UI combos, validation bypass,
   DAX handoff.
4. **Transmit** — `soundout.{h,cpp}` VITA packetiser, `Modulator` capture.
5. **Receive** — `mainwindow` RX bridge, watchdog, UI gain controls.
6. **Safety and status** — safety monitor wiring, status-bar indicators.

## Verification

**What will be verified.** Every stage compiles clean against the existing
macOS build tree in `build/`. `FlexVitaReceiver.cpp` additionally gets a
standalone Linux-target compile check to exercise the POSIX branch of the
shim. When the rig is set to anything other than "Flex Native VITA-49", the
Native FLEX paths must be provably inert — no sockets opened, no worker
thread spawned, no timers armed. That is checked by inspection of every
guard, and it is the property that makes this port safe to merge without
FlexRadio hardware.

**What will not be verified.** On-air behaviour. There is no FlexRadio on
this network, so nothing about actual VITA-49 framing, DAX channel
negotiation, decode quality, TX timing, or safety-monitor telemetry can be
confirmed here. Those need an operator with a radio.

This limit is worth stating plainly: the deliverable is a correct,
portable, compiling integration — not a field-tested one.

## Risks

| Risk | Mitigation |
|---|---|
| `SIGPIPE` kills the app on radio disconnect | `MSG_NOSIGNAL` / `SO_NOSIGPIPE`, called out as hazard 1 |
| Silent VITA packet loss on Linux from an undersized socket buffer | read back `SO_RCVBUF` and log the value obtained |
| Base-version drift (3.0.0 vs 3.0.2) shifts a hunk's context | hunks are placed by W7PP marker and function identity, not by line offset |
| `dec_data`/`dataSink` contract subtly violated, desynchronising the decoder | `flexDataSink` copied verbatim rather than reinterpreted |
| Feature accidentally active for non-Flex users | inertness-when-unselected is an explicit acceptance criterion |
| No hardware test before merge | stated as an unresolved limitation, not papered over |

## Open question

Whether to carry over W7PP's `doc/user_guide/en/w7pp-mods-help-addendum.adoc`.
It documents Native FLEX alongside unrelated W7PP features, so it cannot be
copied wholesale. Deferred until the code is working; not a blocker.
