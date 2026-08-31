<p align="center">
  <img src="docs/wsjtz_icon.png" alt="WSJT-Z" width="160">
</p>

<h1 align="center">WSJT-Z — Native FLEX (VITA-49)</h1>

<p align="center">
  <strong>Talk to a FlexRadio directly. No SmartSDR. No DAX. No other client running.</strong>
</p>

<p align="center">
  <a href="https://www.gnu.org/licenses/gpl-3.0.txt"><img src="https://img.shields.io/badge/license-GPL--3.0-blue" alt="License: GPL-3.0"></a>
  <img src="https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey" alt="Platforms">
  <img src="https://img.shields.io/badge/radio-FLEX--6000%20%2F%208000-orange" alt="Radios">
</p>

---

## What this is

A fork of [WSJT-Z](https://github.com/sq9fve/wsjt-z) that adds **Native FLEX** — a direct
VITA-49 audio and control path to FlexRadio SIGNATURE radios.

WSJT-Z registers itself with the radio as its own client over the SmartSDR TCP API, creates
its own slice and its own DAX streams, and moves receive and transmit audio as raw VITA-49
UDP packets. Nothing sits in between.

## The point: nothing else has to be running

The conventional way to run WSJT-X with a Flex is a stack of moving parts — SmartSDR for the
radio, DAX for virtual audio devices, a CAT shim for rig control, and an audio routing layer
to connect them. Every one of those is a thing to configure, a thing to keep running, and a
thing that can break.

Native FLEX removes the entire stack:

|                          | Conventional          | Native FLEX     |
| ------------------------ | --------------------- | --------------- |
| SmartSDR running         | required              | **not needed**  |
| DAX virtual audio driver | required              | **not needed**  |
| Virtual audio routing    | required              | **not needed**  |
| Separate CAT path        | required              | **not needed**  |
| Sound-card configuration | required              | **not needed**  |

Select **`Flex Native VITA-49`** as the radio, pick your DAX channel, choose the radio when
prompted, and that is the whole setup. WSJT-Z finds the radio by UDP discovery, opens the
SmartSDR API, creates its slice, and streams audio itself.

It genuinely runs headless. In fact it works *better* with SmartSDR closed — a second client
competes for the radio's limited client slots and for ownership of the transmit slice.

## How it works

```
RX:  SmartSDR TCP :4992  +  DAX VITA-49 UDP :4995-5010
       -> 48 kHz float32 -> 127-tap anti-alias FIR -> decimate 4:1
       -> 12 kHz int16 -> dec_data.d2 -> decoder

TX:  Modulator -> software 48 kHz clock -> decimate 2:1
       -> VITA-49 packetiser (24 kHz int16 big-endian)
       -> UDP :4991 -> radio dax_tx stream

CAT: SmartSDR TCP API — slice, mode, frequency, PTT
     + an independent safety monitor (PA power, SWR, voltage, temperature, ATU)
```

Receive is a *third, optional* audio source alongside standard sound-card input and TCI. It
is completely inert unless you select the Native FLEX radio — no socket, no thread, no timer
is created for anyone else.

## Status

Working and used on the air:

- **Receive** — 24 decodes in a single FT8 cycle, DT 0.0–0.7, signals down to −23 dB
- **Transmit** — confirmed two-way QSO (F4JZW → LW2EDM, 20 m FT8, RR73)
- **CAT** — slice creation, frequency, mode, PTT, and a TX safety interlock that refuses to
  key when the radio reports transmit is not permitted
- **RF power** — the power slider sets the radio's RF output (watts, scaled to the PA
  capability the radio reports); disabled when the radio reports power changes are not
  permitted

Verified on a **FLEX-8400M**, SmartSDR 3.1.0.4, firmware 4.2.20.41343, on macOS.

### Known limitations

- **The radio selection is not persisted** — re-select it in *Settings → Radio* after a
  restart.
- **Radio discovery blocks the UI** for up to ~3.5 s while it searches.
- **You need a free client slot.** If the radio reports *"maximum number of connected clients
  has been reached"*, close another client (SmartSDR, or another API client) first.
- Tested only on an 8000-series radio so far. The donor was developed against 6000-series.

## Build (macOS)

```bash
brew install qt@5 fftw boost hamlib libusb cmake gcc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=$(brew --prefix qt@5)
cmake --build build -j8
```

Tests:

```bash
./build/tests/test_flex_socket_compat     # POSIX/Winsock shim
./build/tests/test_flex_vita_receiver     # VITA-49 receive, against a fake radio
./build/tests/test_native_flex_factory    # rig registration
```

`test_flex_vita_receiver` stands up a loopback SmartSDR handshake and feeds synthetic VITA-49
packets, so the receive path is testable without hardware.

## Portability

The donor was written for Windows. Two files used raw Winsock directly and now go through
`Transceiver/FlexSocketCompat.hpp`, which handles the three differences that actually matter
rather than just the spelling:

- **`SIGPIPE`** — on POSIX, writing to a socket the radio closed terminates the process.
- **`EINTR`** — a blocking receive interrupted by a signal must be retried, not treated as fatal.
- **`SO_RCVTIMEO` / `SO_RCVBUF`** — `timeval` rather than `DWORD`, and Linux doubles and
  clamps the buffer, so the granted size is read back rather than assumed.

## Credits and licence

GPLv3. This work stands on three others:

- **[WSJT-X](https://wsjt.sourceforge.io/wsjtx.html)** — Joe Taylor **K1JT** and the WSJT
  Development Group.
- **[WSJT-Z](https://github.com/sq9fve/wsjt-z)** — **SQ9FVE**. This fork tracks WSJT-Z 2.0.19;
  its original README is preserved as [README-WSJT-Z.md](README-WSJT-Z.md).
- **W7PP Mods** — Dick Hale **W7PP**, from *WSJT-X v3.0.2 W7PP Mods v1.05.8 RC3*. The Native
  FLEX implementation is his. It is carried here as close to verbatim as portability allowed,
  with every `W7PP` marker intact so it stays diffable against future releases. Deliberate
  changes are marked `DEVIATION from W7PP` with the reason stated at the point of change.

Design notes and the implementation plan are in
[`docs/superpowers/`](docs/superpowers/).
