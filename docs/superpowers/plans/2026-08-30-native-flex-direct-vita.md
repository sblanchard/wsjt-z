# Native FLEX / direct VITA-49 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the W7PP Native FLEX feature — direct VITA-49 receive and transmit audio plus a SmartSDR CAT backend — into WSJT-Z, building and running on macOS, Linux and Windows.

**Architecture:** Native FLEX is a third, optional receive source parallel to standard audio and TCI. `FlexVitaReceiver` runs a worker thread reading DAX VITA-49 UDP, filters and decimates 48 kHz float to 12 kHz int16, and hands blocks to `MainWindow::flexDataSink`, which writes `dec_data.d2` and calls `dataSink()` on FFT boundaries. Transmit reverses the path through a VITA packetiser in `SoundOutput`. Everything is inert unless the operator selects the rig `"Flex Native VITA-49"`.

**Tech Stack:** C++11, Qt 5 (homebrew `qt@5`), CMake, Qt5Test for unit tests, POSIX/Winsock sockets, `std::thread`.

**Spec:** `docs/superpowers/specs/2026-08-30-native-flex-direct-vita-design.md`

## Global Constraints

- **Donor tree:** `/private/tmp/claude-501/-Users-stephaneblanchard-dev-wsjt-z/fab3962f-6799-43a0-a307-c5c7c6e1f112/scratchpad/w7pp/SRC` — referred to below as `$W7PP`. If that scratchpad is gone, re-extract from `/Users/stephaneblanchard/dev/WSJT-X v3.0.2 W7PP Mods v1.05.8 RC3/W7PP_Mods_v1.05.8_SOURCE_CLEAN_20260827_112353.zip`.
- **Attribution:** W7PP modifications are Copyright (C) 2026 Dick Hale / W7PP, GPLv3. Preserve every `W7PP` comment marker and every `w7pp`/`Flex`/`NativeFlex` symbol name verbatim. Do not rename, do not reformat, do not "tidy" donor code.
- **C++ standard:** C++11. No C++14/17 constructs.
- **Branch:** `native-flex-vita`. Already created.
- **Build directory:** `build/` already configured for macOS with Qt5 at `/opt/homebrew/opt/qt@5`. Never delete or re-run `cmake` from scratch; just `cmake --build build --target <target> -j8`.
- **Source list:** all new `.cpp` files go in `wsjt_qt_CXXSRCS` (starts at `CMakeLists.txt:183`), alongside the other `Transceiver/*.cpp` entries at lines 208-216. This puts them in the `wsjt_qt` library, which `tests/` already links.
- **Inertness requirement:** when the configured rig is anything other than `"Flex Native VITA-49"`, no Native FLEX socket, thread or timer may be created. This is an acceptance criterion for every task that adds a code path.
- **Scope fence:** port only what serves Native FLEX. W7PP also modifies `DecodeHighlightingModel`, `displaytext`, `widegraph`, `about` and ships contest tooling — none of that comes over.
- **No hardware:** there is no FlexRadio on this network. On-air behaviour cannot be verified. Do not claim it has been.

---

### Task 1: POSIX/Winsock compatibility shim

The donor's `FlexVitaReceiver.cpp` is raw Winsock. This header lets it compile unchanged on POSIX for everything except six spots that need real behavioural fixes (handled in Task 2).

**Files:**
- Create: `Transceiver/FlexSocketCompat.hpp`
- Create: `tests/test_flex_socket_compat.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces, for Task 2:
  - `SOCKET`, `INVALID_SOCKET`, `SOCKET_ERROR`, `closesocket(SOCKET)`, `WSADATA`, `MAKEWORD(a,b)`, `WSAStartup(unsigned short, WSADATA*) -> int`, `WSACleanup() -> int`, `WSAGetLastError() -> int`, `WSAETIMEDOUT`, `WSAEWOULDBLOCK`, `SD_BOTH`
  - `bool flexSetReceiveTimeout (SOCKET, int milliseconds)`
  - `int flexSetReceiveBuffer (SOCKET, int bytes)` — returns the buffer size actually obtained, or `-1` on failure
  - `void flexSuppressSigPipe (SOCKET)`
  - `int flexSendFlags ()`
  - `bool flexIsRetryableReceiveError (int)`

- [ ] **Step 1: Write the failing test**

Create `tests/test_flex_socket_compat.cpp`:

```cpp
#include <QtTest>
#include <QElapsedTimer>

#include "Transceiver/FlexSocketCompat.hpp"

class TestFlexSocketCompat : public QObject
{
  Q_OBJECT

private slots:
  void socket_lifecycle ();
  void startup_and_cleanup_succeed ();
  void receive_timeout_is_honoured ();
  void receive_buffer_is_reported ();
  void retryable_errors_classified ();
  void sigpipe_suppression_does_not_fail ();
};

void TestFlexSocketCompat::startup_and_cleanup_succeed ()
{
  WSADATA data {};
  QCOMPARE (WSAStartup (MAKEWORD (2, 2), &data), 0);
  QCOMPARE (WSACleanup (), 0);
}

void TestFlexSocketCompat::socket_lifecycle ()
{
  SOCKET const s = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  QVERIFY (INVALID_SOCKET != s);

  sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  local.sin_port = htons (0);           // let the OS choose

  QCOMPARE (::bind (s, reinterpret_cast<sockaddr *> (&local), sizeof (local)), 0);

  shutdown (s, SD_BOTH);                // must compile and not crash
  QCOMPARE (closesocket (s), 0);
}

void TestFlexSocketCompat::receive_timeout_is_honoured ()
{
  SOCKET const s = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  QVERIFY (INVALID_SOCKET != s);

  sockaddr_in local {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  local.sin_port = htons (0);
  QCOMPARE (::bind (s, reinterpret_cast<sockaddr *> (&local), sizeof (local)), 0);

  QVERIFY (flexSetReceiveTimeout (s, 200));

  char buffer [64];
  QElapsedTimer clock;
  clock.start ();

  int const received = ::recv (s, buffer, sizeof (buffer), 0);
  qint64 const elapsed = clock.elapsed ();

  QCOMPARE (received, -1);
  QVERIFY2 (flexIsRetryableReceiveError (WSAGetLastError ()),
            "a timed-out recv must classify as retryable, not fatal");

  // Generous bounds: we are asserting the timeout took effect at all,
  // not measuring scheduler precision.
  QVERIFY2 (elapsed >= 150, qPrintable (QString ("returned after only %1 ms").arg (elapsed)));
  QVERIFY2 (elapsed <  2000, qPrintable (QString ("blocked for %1 ms").arg (elapsed)));

  closesocket (s);
}

void TestFlexSocketCompat::receive_buffer_is_reported ()
{
  SOCKET const s = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  QVERIFY (INVALID_SOCKET != s);

  int const obtained = flexSetReceiveBuffer (s, 1024 * 1024);

  // Linux doubles the request and clamps to net.core.rmem_max; macOS may
  // clamp too. We require only that we learn the real value and that it is
  // large enough to matter.
  QVERIFY2 (obtained > 0, "must report the buffer size actually obtained");
  QVERIFY2 (obtained >= 64 * 1024,
            qPrintable (QString ("receive buffer only %1 bytes").arg (obtained)));

  closesocket (s);
}

void TestFlexSocketCompat::retryable_errors_classified ()
{
  QVERIFY (flexIsRetryableReceiveError (WSAETIMEDOUT));
  QVERIFY (flexIsRetryableReceiveError (WSAEWOULDBLOCK));
#ifndef Q_OS_WIN
  QVERIFY2 (flexIsRetryableReceiveError (EINTR),
            "EINTR must be retryable or a signal will kill the VITA stream");
#endif
  QVERIFY (!flexIsRetryableReceiveError (ECONNRESET));
}

void TestFlexSocketCompat::sigpipe_suppression_does_not_fail ()
{
  SOCKET const s = ::socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  QVERIFY (INVALID_SOCKET != s);
  flexSuppressSigPipe (s);              // must be safe on every platform
  closesocket (s);
}

QTEST_MAIN (TestFlexSocketCompat)
#include "test_flex_socket_compat.moc"
```

Add to `tests/CMakeLists.txt`, after the existing `test_qt_helpers` lines:

```cmake
add_executable (test_flex_socket_compat test_flex_socket_compat.cpp)
target_link_libraries (test_flex_socket_compat wsjt_qt Qt5::Test)
add_test (test_flex_socket_compat test_flex_socket_compat)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_flex_socket_compat -j8`
Expected: FAIL — `fatal error: 'Transceiver/FlexSocketCompat.hpp' file not found`

- [ ] **Step 3: Write minimal implementation**

Create `Transceiver/FlexSocketCompat.hpp`:

```cpp
#ifndef W7PP_FLEX_SOCKET_COMPAT_HPP
#define W7PP_FLEX_SOCKET_COMPAT_HPP

/*
 * Portability shim for the W7PP Native FLEX VITA receiver.
 *
 * FlexVitaReceiver.cpp was written against Winsock. This header supplies
 * the same vocabulary on POSIX so that file stays diffable against future
 * W7PP releases.
 *
 * Three differences are real behaviour, not spelling, and are handled by
 * the helper functions rather than by macros:
 *
 *   1. SIGPIPE. On POSIX, send() to a socket the peer closed raises
 *      SIGPIPE, which by default terminates the process. Windows has no
 *      equivalent. Use flexSuppressSigPipe() plus flexSendFlags().
 *
 *   2. EINTR. Blocking recv()/recvfrom() on POSIX can return EINTR when a
 *      signal arrives. Treat it exactly like a timeout, or a stray signal
 *      drops the VITA stream. Use flexIsRetryableReceiveError().
 *
 *   3. SO_RCVTIMEO takes DWORD milliseconds on Windows and a struct
 *      timeval on POSIX; SO_RCVBUF is doubled and clamped by Linux. Use
 *      flexSetReceiveTimeout() and flexSetReceiveBuffer().
 */

#include <QtGlobal>

#if defined (Q_OS_WIN)

# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif

# include <winsock2.h>
# include <ws2tcpip.h>

#else

# include <arpa/inet.h>
# include <errno.h>
# include <netinet/in.h>
# include <sys/socket.h>
# include <sys/time.h>
# include <sys/types.h>
# include <unistd.h>

typedef int SOCKET;

# ifndef INVALID_SOCKET
#  define INVALID_SOCKET (-1)
# endif

# ifndef SOCKET_ERROR
#  define SOCKET_ERROR (-1)
# endif

# ifndef SD_BOTH
#  define SD_BOTH SHUT_RDWR
# endif

# ifndef WSAETIMEDOUT
#  define WSAETIMEDOUT ETIMEDOUT
# endif

# ifndef WSAEWOULDBLOCK
#  define WSAEWOULDBLOCK EWOULDBLOCK
# endif

# ifndef MAKEWORD
#  define MAKEWORD(low, high) \
    (static_cast<unsigned short> ((low) | ((high) << 8)))
# endif

struct WSADATA
{
  int unused;
};

inline int closesocket (SOCKET socket)
{
  return ::close (socket);
}

inline int WSAStartup (unsigned short, WSADATA *)
{
  return 0;
}

inline int WSACleanup ()
{
  return 0;
}

inline int WSAGetLastError ()
{
  return errno;
}

#endif  // Q_OS_WIN

/*
 * Set the blocking receive timeout, in milliseconds.
 */
inline bool flexSetReceiveTimeout (SOCKET socket, int milliseconds)
{
#if defined (Q_OS_WIN)
  DWORD timeout = static_cast<DWORD> (milliseconds);

  return SOCKET_ERROR != setsockopt (
      socket,
      SOL_SOCKET,
      SO_RCVTIMEO,
      reinterpret_cast<char const *> (&timeout),
      sizeof (timeout));
#else
  timeval timeout {};

  timeout.tv_sec  = milliseconds / 1000;
  timeout.tv_usec = (milliseconds % 1000) * 1000;

  return SOCKET_ERROR != setsockopt (
      socket,
      SOL_SOCKET,
      SO_RCVTIMEO,
      &timeout,
      sizeof (timeout));
#endif
}

/*
 * Request a receive buffer and report what was actually granted.
 *
 * Linux doubles the request for bookkeeping and clamps it against
 * net.core.rmem_max, so the requested value is not what you get. An
 * undersized buffer shows up as dropped VITA packets under load, which
 * looks like decode loss rather than an error - so the caller logs the
 * value this returns.
 */
inline int flexSetReceiveBuffer (SOCKET socket, int bytes)
{
  if (SOCKET_ERROR == setsockopt (
          socket,
          SOL_SOCKET,
          SO_RCVBUF,
          reinterpret_cast<char const *> (&bytes),
          sizeof (bytes)))
    {
      return -1;
    }

  int obtained = 0;

#if defined (Q_OS_WIN)
  int length = sizeof (obtained);
#else
  socklen_t length = sizeof (obtained);
#endif

  if (SOCKET_ERROR == getsockopt (
          socket,
          SOL_SOCKET,
          SO_RCVBUF,
          reinterpret_cast<char *> (&obtained),
          &length))
    {
      return -1;
    }

  return obtained;
}

/*
 * Stop a write to a closed socket from killing the process.
 *
 * macOS and the BSDs use the SO_NOSIGPIPE socket option; Linux has no
 * such option and uses the MSG_NOSIGNAL send flag instead - see
 * flexSendFlags(). Windows needs neither.
 */
inline void flexSuppressSigPipe (SOCKET socket)
{
#if defined (SO_NOSIGPIPE)
  int on = 1;

  setsockopt (
      socket,
      SOL_SOCKET,
      SO_NOSIGPIPE,
      reinterpret_cast<char const *> (&on),
      sizeof (on));
#else
  Q_UNUSED (socket);
#endif
}

inline int flexSendFlags ()
{
#if defined (MSG_NOSIGNAL)
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

/*
 * True when a failed blocking receive should simply be retried.
 */
inline bool flexIsRetryableReceiveError (int error)
{
  if (WSAETIMEDOUT == error || WSAEWOULDBLOCK == error)
    return true;

#if !defined (Q_OS_WIN)
  if (EINTR == error || EAGAIN == error)
    return true;
#endif

  return false;
}

#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake --build build --target test_flex_socket_compat -j8 && ./build/tests/test_flex_socket_compat
```
Expected: PASS — `Totals: 8 passed, 0 failed, 0 skipped, 0 blacklisted` (6 test slots plus `initTestCase` and `cleanupTestCase`).

If `receive_buffer_is_reported` fails with a small value on Linux, that is a real finding, not a flaky test: report the value and the `net.core.rmem_max` setting rather than weakening the assertion.

- [ ] **Step 5: Commit**

```bash
git add Transceiver/FlexSocketCompat.hpp tests/test_flex_socket_compat.cpp tests/CMakeLists.txt
git commit -m "Add POSIX/Winsock compatibility shim for Native FLEX receiver

Maps Winsock vocabulary to POSIX and handles the three real behaviour
differences: SIGPIPE on send to a closed socket, EINTR on blocking
receive, and SO_RCVTIMEO/SO_RCVBUF semantics."
```

---

### Task 2: Import donor files and make the receiver portable

**Files:**
- Create (copied verbatim from `$W7PP/Transceiver/`): `Transceiver/FlexVitaReceiver.hpp`, `Transceiver/NativeFlexTransceiver.{hpp,cpp}`, `Transceiver/NativeFlexSafetyMonitor.{hpp,cpp}`, `Transceiver/NativeFlexRadioSelection.{hpp,cpp}`
- Create (copied, then edited): `Transceiver/FlexVitaReceiver.cpp`
- Create: `tests/test_flex_vita_receiver.cpp`
- Modify: `CMakeLists.txt` (source list at lines 208-216), `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Task 1's `FlexSocketCompat.hpp`.
- Produces, for Tasks 3 and 6:
  - `class FlexVitaReceiver` — `start(Configuration const&) -> bool`, `stop()`, `reset()`, `running() const -> bool`, `streaming() const -> bool`, `statistics() const -> Statistics`, `lastError() const -> std::string`, `setAudioCallback(AudioCallback)`
  - `FlexVitaReceiver::Configuration { std::string radioAddress; unsigned short tcpPort; int daxChannel; int firstUdpPort; int lastUdpPort; }`
  - `FlexVitaReceiver::DecoderBlock` = `std::vector<std::int16_t>`
  - `FlexVitaReceiver::AudioCallback` = `std::function<void(DecoderBlock const&)>`
  - `FlexVitaReceiver::Statistics { vitaPackets, audioFloats, decoderSamples, sequenceErrors, malformedPackets, invalidFloats }` — all `std::uint64_t`
  - `class NativeFlexTransceiver : public TransceiverBase` — `NativeFlexTransceiver(logger_type *, QObject * parent = nullptr)`, statics `set_startup_frequency(Frequency)`, `set_dax_channel(int)`
  - `NativeFlexRadioSelection::refresh(QWidget *)`

- [ ] **Step 1: Copy the donor files**

```bash
W7PP=/private/tmp/claude-501/-Users-stephaneblanchard-dev-wsjt-z/fab3962f-6799-43a0-a307-c5c7c6e1f112/scratchpad/w7pp/SRC
cp "$W7PP/Transceiver/FlexVitaReceiver.hpp" \
   "$W7PP/Transceiver/FlexVitaReceiver.cpp" \
   "$W7PP/Transceiver/NativeFlexTransceiver.hpp" \
   "$W7PP/Transceiver/NativeFlexTransceiver.cpp" \
   "$W7PP/Transceiver/NativeFlexSafetyMonitor.hpp" \
   "$W7PP/Transceiver/NativeFlexSafetyMonitor.cpp" \
   "$W7PP/Transceiver/NativeFlexRadioSelection.hpp" \
   "$W7PP/Transceiver/NativeFlexRadioSelection.cpp" \
   Transceiver/
```

Do not edit anything except `FlexVitaReceiver.cpp`, and in that file only the six edits below.

- [ ] **Step 2: Apply the shim edits to `Transceiver/FlexVitaReceiver.cpp`**

**Edit 1 — headers.** Replace lines 1-6, which read:

```cpp
#define WIN32_LEAN_AND_MEAN

#include "FlexVitaReceiver.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
```

with:

```cpp
#include "FlexVitaReceiver.hpp"

// W7PP : portability shim, see Transceiver/FlexSocketCompat.hpp
#include "FlexSocketCompat.hpp"
```

**Edit 2 — UDP receive timeout and buffer.** Replace this block (around line 886):

```cpp
    DWORD udpTimeout = 200;

    setsockopt(
        udp,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<char const *>(&udpTimeout),
        sizeof(udpTimeout));

    int receiveBuffer = 1024 * 1024;

    setsockopt(
        udp,
        SOL_SOCKET,
        SO_RCVBUF,
        reinterpret_cast<char const *>(&receiveBuffer),
        sizeof(receiveBuffer));
```

with:

```cpp
    flexSetReceiveTimeout(udp, 200);

    // Linux doubles the request and clamps against net.core.rmem_max, so
    // report what we actually got. An undersized buffer drops VITA packets
    // under load, which looks like decode loss rather than an error.
    int const obtainedReceiveBuffer =
        flexSetReceiveBuffer(udp, 1024 * 1024);

    if (obtainedReceiveBuffer < 0)
      {
        fail("UDP receive buffer configuration failed");
        return;
      }
```

**Edit 3 — TCP receive timeout.** Replace this block (around line 918):

```cpp
    DWORD tcpTimeout = 250;

    setsockopt(
        tcp,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<char const *>(&tcpTimeout),
        sizeof(tcpTimeout));
```

with:

```cpp
    flexSetReceiveTimeout(tcp, 250);

    // POSIX: a send to a socket the radio has closed would otherwise
    // raise SIGPIPE and terminate the process.
    flexSuppressSigPipe(tcp);
```

**Edit 4 — SIGPIPE-safe send.** In `sendAll`, replace:

```cpp
        int const sent =
            ::send(
                socket,
                current,
                remaining,
                0);
```

with:

```cpp
        int const sent =
            static_cast<int>(
                ::send(
                    socket,
                    current,
                    remaining,
                    flexSendFlags()));
```

**Edit 5 — retryable TCP receive errors.** In `listenTcpFor`, replace:

```cpp
        if (WSAETIMEDOUT == socketError
            || WSAEWOULDBLOCK == socketError)
          {
            continue;
          }
```

with:

```cpp
        if (flexIsRetryableReceiveError(socketError))
          {
            continue;
          }
```

**Edit 6 — retryable UDP receive errors, and the `recvfrom` length type.** In the main receive loop, replace:

```cpp
        sockaddr_in remote {};
        int remoteLength = sizeof(remote);
```

with:

```cpp
        sockaddr_in remote {};
        socklen_t remoteLength = sizeof(remote);
```

and, further down in the same loop, replace:

```cpp
        if (WSAETIMEDOUT == socketError
            || WSAEWOULDBLOCK == socketError)
          {
```

with:

```cpp
        if (flexIsRetryableReceiveError(socketError))
          {
```

Also change the two `::recv` / `::recvfrom` result declarations from `int const received =` to `int const received = static_cast<int>(` … `)` if the compiler warns about narrowing `ssize_t` to `int`. Leave them alone if it does not.

- [ ] **Step 3: Wire into the build**

In `CMakeLists.txt`, in `wsjt_qt_CXXSRCS`, immediately after the line `  Transceiver/TCITransceiver.cpp` (line 216), insert:

```cmake
  Transceiver/FlexVitaReceiver.cpp
  Transceiver/NativeFlexRadioSelection.cpp
  Transceiver/NativeFlexTransceiver.cpp
  Transceiver/NativeFlexSafetyMonitor.cpp
```

- [ ] **Step 4: Verify it compiles as dead weight**

Run: `cmake --build build --target wsjt_qt -j8`
Expected: PASS. Nothing references the new classes yet; they must compile clean anyway.

If `NativeFlexTransceiver.cpp` fails on a `TransceiverBase` API difference between WSJT-X 3.0.0 and 3.0.2, that is expected base-version drift. Fix it by adapting the donor code to WSJT-Z's `TransceiverBase` signature, and note the adaptation in the commit message.

- [ ] **Step 5: Write the fake-radio integration test**

This is the only way to exercise the receiver without hardware. It stands up a loopback TCP server speaking just enough of the SmartSDR API to reach the streaming state, then feeds synthetic VITA-49 packets and asserts decoded audio comes back.

The handshake the receiver expects:
1. On connect, the radio sends `H<hex handle>\n`.
2. Client sends `C3|sub slice all`, `C4|sub audio_stream all`, `C5|sub dax all`, `C6|client udpport <port>`, `C7|stream create type=dax_rx dax_channel=<n>`.
3. Radio replies with a line containing `|stream <id> ... type=dax_rx ... dax_channel=<n> ... client_handle=<handle>`, which sets the stream id and flips `streaming()` true.

VITA-49 packet layout the receiver parses — word 0, big-endian:
`bits 31-28` packet type (1 or 3), `bit 27` class-ID present, `bit 26` trailer present, `bits 23-22` TSI, `bits 21-20` TSF, `bits 19-16` packet count (mod 16), `bits 15-0` total packet size in 32-bit words. Word 1 is the stream ID. Payload is big-endian `float32` at 48 kHz.

Create `tests/test_flex_vita_receiver.cpp`:

```cpp
#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QHostAddress>
#include <QDeadlineTimer>

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include "Transceiver/FlexVitaReceiver.hpp"

namespace
{
  quint32 const FakeClientHandle = 0x1234ABCDu;
  quint32 const FakeStreamId     = 0x84000001u;
  int     const FakeDaxChannel   = 1;

  void appendBigEndian32 (QByteArray& out, quint32 value)
  {
    out.append (static_cast<char> ((value >> 24) & 0xFF));
    out.append (static_cast<char> ((value >> 16) & 0xFF));
    out.append (static_cast<char> ((value >>  8) & 0xFF));
    out.append (static_cast<char> ( value        & 0xFF));
  }

  void appendBigEndianFloat (QByteArray& out, float value)
  {
    quint32 bits = 0;
    std::memcpy (&bits, &value, sizeof (bits));
    appendBigEndian32 (out, bits);
  }

  // Type 1 (IF data with stream ID), no class ID, no timestamps.
  QByteArray buildVitaPacket (unsigned packetCount, std::vector<float> const& samples)
  {
    quint32 const words = 2u + static_cast<quint32> (samples.size ());

    quint32 header = 0;
    header |= 1u << 28;                       // packet type 1
    header |= (packetCount & 0x0Fu) << 16;    // packet count
    header |= words & 0xFFFFu;                // total size in words

    QByteArray packet;
    appendBigEndian32 (packet, header);
    appendBigEndian32 (packet, FakeStreamId);

    for (float const sample : samples)
      appendBigEndianFloat (packet, sample);

    return packet;
  }
}

class TestFlexVitaReceiver : public QObject
{
  Q_OBJECT

private slots:
  void init ();
  void cleanup ();

  void reaches_streaming_state ();
  void decodes_vita_audio_to_12khz ();
  void rejects_foreign_stream_id ();
  void counts_malformed_packets ();
  void start_fails_on_bad_address ();

private:
  bool waitForStreaming (FlexVitaReceiver&, int timeoutMs = 8000);

  QTcpServer * server_ {nullptr};
  QTcpSocket * client_ {nullptr};
  int          udpPort_ {0};
};

void TestFlexVitaReceiver::init ()
{
  server_ = new QTcpServer {this};
  QVERIFY (server_->listen (QHostAddress::LocalHost, 0));

  client_  = nullptr;
  udpPort_ = 0;

  // Drive the fake radio side of the handshake as the receiver talks.
  connect (server_, &QTcpServer::newConnection, this, [this] ()
    {
      client_ = server_->nextPendingConnection ();

      // Step 1: announce our client handle.
      client_->write (QByteArray ("H") +
                      QByteArray::number (FakeClientHandle, 16) + "\n");

      connect (client_, &QTcpSocket::readyRead, this, [this] ()
        {
          while (client_->canReadLine ())
            {
              QByteArray const line = client_->readLine ().trimmed ();

              if (line.contains ("client udpport"))
                {
                  udpPort_ = line.mid (line.lastIndexOf (' ') + 1).toInt ();
                }
              else if (line.contains ("stream create")
                       && line.contains ("type=dax_rx"))
                {
                  // Step 3: report the stream we just "created".
                  QByteArray reply = "S" +
                      QByteArray::number (FakeClientHandle, 16) +
                      "|stream " + QByteArray::number (FakeStreamId, 16) +
                      " type=dax_rx dax_channel=" +
                      QByteArray::number (FakeDaxChannel) +
                      " client_handle=" +
                      QByteArray::number (FakeClientHandle, 16) + "\n";

                  client_->write (reply);
                }
            }
        });
    });
}

void TestFlexVitaReceiver::cleanup ()
{
  delete server_;
  server_ = nullptr;
  client_ = nullptr;
}

bool TestFlexVitaReceiver::waitForStreaming (FlexVitaReceiver& receiver, int timeoutMs)
{
  QDeadlineTimer deadline {timeoutMs};

  while (!deadline.hasExpired ())
    {
      QCoreApplication::processEvents (QEventLoop::AllEvents, 20);

      if (receiver.streaming () && udpPort_ > 0)
        return true;
    }

  return false;
}

void TestFlexVitaReceiver::reaches_streaming_state ()
{
  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration configuration;
  configuration.radioAddress = "127.0.0.1";
  configuration.tcpPort      = server_->serverPort ();
  configuration.daxChannel   = FakeDaxChannel;

  QVERIFY2 (receiver.start (configuration),
            qPrintable (QString::fromStdString (receiver.lastError ())));

  QVERIFY2 (waitForStreaming (receiver),
            qPrintable (QString::fromStdString (receiver.lastError ())));

  QVERIFY (receiver.running ());
  receiver.stop ();
  QVERIFY (!receiver.running ());
}

void TestFlexVitaReceiver::decodes_vita_audio_to_12khz ()
{
  FlexVitaReceiver receiver;

  std::atomic<int> delivered {0};
  std::atomic<int> nonZero   {0};

  receiver.setAudioCallback (
      [&delivered, &nonZero] (FlexVitaReceiver::DecoderBlock const& block)
      {
        delivered.fetch_add (static_cast<int> (block.size ()));

        for (std::int16_t const sample : block)
          if (0 != sample)
            nonZero.fetch_add (1);
      });

  FlexVitaReceiver::Configuration configuration;
  configuration.radioAddress = "127.0.0.1";
  configuration.tcpPort      = server_->serverPort ();
  configuration.daxChannel   = FakeDaxChannel;

  QVERIFY (receiver.start (configuration));
  QVERIFY (waitForStreaming (receiver));

  // A 1 kHz tone at 48 kHz, well below the 6 kHz Nyquist of the decimated
  // stream, so the anti-alias FIR should pass it through.
  QUdpSocket sender;

  int const samplesPerPacket = 256;
  int const packetCount      = 40;      // 10240 input samples -> ~2560 out
  double    phase            = 0.0;
  double const increment     = 2.0 * M_PI * 1000.0 / 48000.0;

  for (int p = 0; p < packetCount; ++p)
    {
      std::vector<float> samples;
      samples.reserve (samplesPerPacket);

      for (int i = 0; i < samplesPerPacket; ++i)
        {
          samples.push_back (static_cast<float> (0.5 * std::sin (phase)));
          phase += increment;
        }

      QByteArray const packet =
          buildVitaPacket (static_cast<unsigned> (p), samples);

      sender.writeDatagram (packet, QHostAddress::LocalHost,
                            static_cast<quint16> (udpPort_));

      QTest::qWait (2);
    }

  QDeadlineTimer deadline {5000};

  while (!deadline.hasExpired () && delivered.load () < 1200)
    QCoreApplication::processEvents (QEventLoop::AllEvents, 20);

  auto const statistics = receiver.statistics ();

  QVERIFY2 (statistics.vitaPackets >= 30,
            qPrintable (QString ("only %1 VITA packets accepted")
                        .arg (statistics.vitaPackets)));
  QCOMPARE (statistics.malformedPackets, quint64 (0));
  QCOMPARE (statistics.invalidFloats,    quint64 (0));

  // 4:1 decimation, delivered in 1200-sample blocks.
  QVERIFY2 (delivered.load () >= 1200,
            qPrintable (QString ("only %1 decoder samples delivered")
                        .arg (delivered.load ())));
  QCOMPARE (delivered.load () % 1200, 0);

  QVERIFY2 (nonZero.load () > delivered.load () / 4,
            "decimated tone should be mostly non-zero");

  receiver.stop ();
}

void TestFlexVitaReceiver::rejects_foreign_stream_id ()
{
  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration configuration;
  configuration.radioAddress = "127.0.0.1";
  configuration.tcpPort      = server_->serverPort ();
  configuration.daxChannel   = FakeDaxChannel;

  QVERIFY (receiver.start (configuration));
  QVERIFY (waitForStreaming (receiver));

  // Same shape, wrong stream: another DAX client on the same network.
  QByteArray packet = buildVitaPacket (0, std::vector<float> (64, 0.25f));
  packet [4] = static_cast<char> (0x99);        // corrupt the stream ID

  QUdpSocket sender;

  for (int i = 0; i < 10; ++i)
    {
      sender.writeDatagram (packet, QHostAddress::LocalHost,
                            static_cast<quint16> (udpPort_));
      QTest::qWait (5);
    }

  QTest::qWait (300);

  // Foreign streams are ignored silently - not counted as malformed.
  auto const statistics = receiver.statistics ();
  QCOMPARE (statistics.vitaPackets,      quint64 (0));
  QCOMPARE (statistics.malformedPackets, quint64 (0));

  receiver.stop ();
}

void TestFlexVitaReceiver::counts_malformed_packets ()
{
  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration configuration;
  configuration.radioAddress = "127.0.0.1";
  configuration.tcpPort      = server_->serverPort ();
  configuration.daxChannel   = FakeDaxChannel;

  QVERIFY (receiver.start (configuration));
  QVERIFY (waitForStreaming (receiver));

  // Right stream ID, impossible declared length.
  QByteArray packet;
  appendBigEndian32 (packet, (1u << 28) | 0xFFFFu);   // claims 65535 words
  appendBigEndian32 (packet, FakeStreamId);
  packet.append (16, '\0');

  QUdpSocket sender;

  for (int i = 0; i < 5; ++i)
    {
      sender.writeDatagram (packet, QHostAddress::LocalHost,
                            static_cast<quint16> (udpPort_));
      QTest::qWait (5);
    }

  QTest::qWait (300);

  QVERIFY2 (receiver.statistics ().malformedPackets >= 1,
            "an over-long declared packet size must be counted malformed");

  receiver.stop ();
}

void TestFlexVitaReceiver::start_fails_on_bad_address ()
{
  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration configuration;
  configuration.radioAddress = "not-an-ip-address";
  configuration.tcpPort      = server_->serverPort ();
  configuration.daxChannel   = FakeDaxChannel;

  receiver.start (configuration);

  QDeadlineTimer deadline {3000};

  while (!deadline.hasExpired () && receiver.lastError ().empty ())
    QCoreApplication::processEvents (QEventLoop::AllEvents, 20);

  QVERIFY2 (!receiver.lastError ().empty (),
            "an invalid IPv4 address must produce an error");
  QVERIFY (!receiver.streaming ());

  receiver.stop ();
}

QTEST_MAIN (TestFlexVitaReceiver)
#include "test_flex_vita_receiver.moc"
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable (test_flex_vita_receiver test_flex_vita_receiver.cpp)
target_link_libraries (test_flex_vita_receiver wsjt_qt Qt5::Test Qt5::Network)
add_test (test_flex_vita_receiver test_flex_vita_receiver)
```

- [ ] **Step 6: Run the integration test**

Run:
```bash
cmake --build build --target test_flex_vita_receiver -j8 && ./build/tests/test_flex_vita_receiver
```
Expected: PASS, all five slots.

These tests are timing-dependent by nature — they drive a real socket across loopback. If one fails intermittently, diagnose it; do not paper over it by widening a tolerance without understanding which step was slow. If `decodes_vita_audio_to_12khz` reports zero packets accepted, the likeliest cause is that the fake radio's stream-reply line does not match what `processTcpLine` looks for: it requires `|stream `, `type=dax_rx`, `dax_channel=<n>`, and a `client_handle=` equal to the handle sent at connect.

- [ ] **Step 7: Commit**

```bash
git add Transceiver/FlexVitaReceiver.hpp Transceiver/FlexVitaReceiver.cpp \
        Transceiver/NativeFlexTransceiver.hpp Transceiver/NativeFlexTransceiver.cpp \
        Transceiver/NativeFlexSafetyMonitor.hpp Transceiver/NativeFlexSafetyMonitor.cpp \
        Transceiver/NativeFlexRadioSelection.hpp Transceiver/NativeFlexRadioSelection.cpp \
        tests/test_flex_vita_receiver.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "Import W7PP Native FLEX sources and make the VITA receiver portable

Adds FlexVitaReceiver, NativeFlexTransceiver, NativeFlexSafetyMonitor and
NativeFlexRadioSelection from WSJT-X v3.0.2 W7PP Mods v1.05.8 RC3,
Copyright (C) 2026 Dick Hale / W7PP, GPLv3.

FlexVitaReceiver.cpp is edited only where Winsock and POSIX genuinely
differ: SIGPIPE, EINTR, SO_RCVTIMEO/SO_RCVBUF and the recvfrom length
type. Covered by a fake-radio integration test."
```

---

### Task 3: Register the transceiver backend

**Files:**
- Modify: `Transceiver/TransceiverFactory.cpp`
- Modify: `Transceiver/TransceiverBase.cpp`
- Create: `tests/test_native_flex_factory.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `NativeFlexTransceiver` from Task 2.
- Produces, for Task 4: the rig name string `"Flex Native VITA-49"` appears in `TransceiverFactory::supported_transceivers()`. Task 4's Configuration code matches on this exact string.

- [ ] **Step 1: Write the failing test**

Create `tests/test_native_flex_factory.cpp`:

```cpp
#include <QtTest>

#include "Transceiver/TransceiverFactory.hpp"

class TestNativeFlexFactory : public QObject
{
  Q_OBJECT

private slots:
  void native_flex_is_registered ();
  void native_flex_capabilities ();
  void other_rigs_still_registered ();
};

void TestNativeFlexFactory::native_flex_is_registered ()
{
  TransceiverFactory factory;

  QVERIFY2 (factory.supported_transceivers ().contains ("Flex Native VITA-49"),
            "the Native FLEX rig must be selectable by exact name");
}

void TestNativeFlexFactory::native_flex_capabilities ()
{
  TransceiverFactory factory;

  auto const capabilities =
      factory.supported_transceivers ().value ("Flex Native VITA-49");

  // Port type none: the radio is found over the network, so no serial
  // port or TCI endpoint is offered in the settings UI.
  QCOMPARE (capabilities.port_type_, TransceiverFactory::Capabilities::none);
  QVERIFY  (capabilities.has_CAT_PTT_);
  QVERIFY  (!capabilities.has_CAT_PTT_mic_data_);
  QVERIFY  (!capabilities.has_CAT_indirect_serial_PTT_);
  QVERIFY  (capabilities.asynchronous_);
}

void TestNativeFlexFactory::other_rigs_still_registered ()
{
  TransceiverFactory factory;

  // Adding a backend must not disturb the existing ones.
  QVERIFY (factory.supported_transceivers ().contains ("None"));
  QVERIFY (factory.supported_transceivers ().size () > 10);
}

QTEST_MAIN (TestNativeFlexFactory)
#include "test_native_flex_factory.moc"
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable (test_native_flex_factory test_native_flex_factory.cpp)
target_link_libraries (test_native_flex_factory wsjt_qt Qt5::Test)
add_test (test_native_flex_factory test_native_flex_factory)
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
cmake --build build --target test_native_flex_factory -j8 && ./build/tests/test_native_flex_factory
```
Expected: FAIL — `native_flex_is_registered` fails, "the Native FLEX rig must be selectable by exact name".

- [ ] **Step 3: Write the implementation**

In `Transceiver/TransceiverFactory.cpp`, add the include after the `HRDTransceiver.hpp` include:

```cpp
#include "NativeFlexTransceiver.hpp"
```

Add `NativeFlexId` as the last member of the anonymous-namespace enum, so existing model numbers do not shift:

```cpp
  enum				// supported non-hamlib radio interfaces
    {
      NonHamlibBaseId = 99899
      , TCI1Id
      , TCI2Id
      , CommanderId
      , HRDId
      , OmniRigOneId
      , OmniRigTwoId
      , NativeFlexId
    };
```

In the constructor, after the `HRDTransceiver::register_transceivers` line:

```cpp
  // W7PP Native Flex is an isolated additional backend.
  // Port type NONE is intentional: radio discovery will be automatic.
  // No Native Flex object or socket exists unless this radio is selected.
  transceivers_.insert (
      "Flex Native VITA-49",
      Capabilities {
          NativeFlexId,
          Capabilities::none,
          true,
          false,
          false,
          true});
```

In the factory's create/construction switch, add the case alongside the other non-hamlib ids. Match the surrounding style for wrapping the result in `EmulateSplitTransceiver` where the existing cases do so:

```cpp
    case NativeFlexId:
      {
        std::unique_ptr<Transceiver> basic_transceiver {
            new NativeFlexTransceiver {&logger_}};

        return basic_transceiver;
      }
      break;
```

Consult `$W7PP/Transceiver/TransceiverFactory.cpp` around line 224 for the donor's exact construction, and follow WSJT-Z's local conventions for the surrounding cases where the two differ.

- [ ] **Step 4: Run test to verify it passes**

Run:
```bash
cmake --build build --target test_native_flex_factory -j8 && ./build/tests/test_native_flex_factory
```
Expected: PASS, all three slots.

- [ ] **Step 5: Make the TX safety inhibit recoverable**

In `Transceiver/TransceiverBase.cpp`, find the error-handling block that calls `offline()`. Before that call, insert:

```cpp
      /*
       * W7PP 
       *
       * Native FLEX TX safety inhibit is recoverable.
       * Report it to MainWindow without calling offline(),
       * because offline() immediately calls shutdown() and
       * would tear down the still-good Native FLEX RX path.
       *
       * Every other transceiver error retains the original
       * behaviour.
       */
      if (
          message.startsWith(
              QStringLiteral("Native FLEX TX INHIBITED:")))
        {
          requested_.ptt(actual_.ptt ());
```

Copy the complete block from `$W7PP/Transceiver/TransceiverBase.cpp` around lines 215-240 — including the closing brace and whatever follows — rather than reconstructing it from this excerpt.

- [ ] **Step 6: Verify the whole application still builds**

Run: `cmake --build build -j8`
Expected: PASS. Also re-run the earlier tests to confirm nothing regressed:
```bash
./build/tests/test_flex_socket_compat && ./build/tests/test_flex_vita_receiver
```

- [ ] **Step 7: Commit**

```bash
git add Transceiver/TransceiverFactory.cpp Transceiver/TransceiverBase.cpp \
        tests/test_native_flex_factory.cpp tests/CMakeLists.txt
git commit -m "Register Flex Native VITA-49 transceiver backend

Adds the rig to TransceiverFactory and makes a Native FLEX TX safety
inhibit recoverable in TransceiverBase, so it no longer tears down a
healthy RX path."
```

---

### Task 4: Configuration and settings

**Files:**
- Modify: `Configuration.hpp`, `Configuration.cpp`, `Configuration.ui`

**Interfaces:**
- Consumes: the rig name `"Flex Native VITA-49"` from Task 3; `NativeFlexTransceiver::set_dax_channel(int)` and `NativeFlexRadioSelection::refresh(QWidget *)` from Task 2.
- Produces, for Task 6: `bool Configuration::flex_native_rx () const` and `int Configuration::flex_dax_channel () const`.

Settings keys: `W7PPFlexNativeRx` (bool, default `false`), `W7PPFlexDaxChannel` (int, clamped 1-8, default `1`).

- [ ] **Step 1: Add the UI controls**

Open `$W7PP/Configuration.ui` at lines 1735-1780 and copy the two labels and two combo boxes into the same position on WSJT-Z's Audio tab in `Configuration.ui`:

- `w7pp_flex_rx_method_label` + `w7pp_flex_rx_method_combo_box`, items `"Standard Flex / Windows DAX RX"` (index 0) and `"Native Flex VITA-49 RX"` (index 1), tooltip `"Select standard Windows/DAX receive audio or direct native Flex VITA-49 receive audio."`
- `w7pp_flex_dax_channel_label` (text `"Flex DAX channel:"`) + `w7pp_flex_dax_channel_combo_box`, items `"1"` through `"8"`, tooltip `"Select the SmartSDR DAX channel assigned to the slice used for native Flex VITA-49 RX."`

Widget names must match exactly — `Configuration.cpp` refers to them by name through the generated `ui_Configuration.h`.

- [ ] **Step 2: Add the accessors**

In `Configuration.hpp`, alongside the other audio accessors:

```cpp
  bool flex_native_rx () const;
  int flex_dax_channel () const;
```

- [ ] **Step 3: Add implementation state and wiring**

In `Configuration.cpp`:

Includes, near the other Transceiver includes:

```cpp
#include "Transceiver/NativeFlexTransceiver.hpp"
#include "Transceiver/NativeFlexRadioSelection.hpp"
```

Private method declaration on `Configuration::impl`:

```cpp
  void update_w7pp_flex_rx_controls ();
```

Members on `Configuration::impl`:

```cpp
  bool w7pp_flex_native_rx_ {false};
  int w7pp_flex_dax_channel_ {1};
```

Accessor definitions, next to the other one-line accessors:

```cpp
bool Configuration::flex_native_rx () const {return m_->w7pp_flex_native_rx_;}
int Configuration::flex_dax_channel () const {return m_->w7pp_flex_dax_channel_;}
```

Combo wiring, in the constructor next to the other `connect` calls:

```cpp
  // W7PP Flex RX method selector on the Audio tab.
  connect (ui_->w7pp_flex_rx_method_combo_box,
           static_cast<void (QComboBox::*) (int)> (&QComboBox::currentIndexChanged),
           this,
           [this] (int) { update_w7pp_flex_rx_controls (); });
```

Enable/disable logic:

```cpp
void Configuration::impl::update_w7pp_flex_rx_controls ()
{
  bool const native =
      ui_->w7pp_flex_rx_method_combo_box->currentIndex () == 1;

  ui_->w7pp_flex_dax_channel_label->setEnabled (native);
  ui_->w7pp_flex_dax_channel_combo_box->setEnabled (native);
}
```

Consult `$W7PP/Configuration.cpp:2821-2833` for the donor's full version — it also disables the standard audio-input widgets when native RX is active. Copy that behaviour.

Reading settings, in `read_settings`:

```cpp
  w7pp_flex_native_rx_ = settings_->value ("W7PPFlexNativeRx", false).toBool ();
  w7pp_flex_dax_channel_ = settings_->value ("W7PPFlexDaxChannel", 1).toInt ();
  if (w7pp_flex_dax_channel_ < 1 || w7pp_flex_dax_channel_ > 8)
    w7pp_flex_dax_channel_ = 1;
```

Writing settings, in `write_settings`:

```cpp
  settings_->setValue ("W7PPFlexNativeRx", w7pp_flex_native_rx_);
  settings_->setValue ("W7PPFlexDaxChannel", w7pp_flex_dax_channel_);
```

Initialising the widgets, in `initialise_models`:

```cpp
  ui_->w7pp_flex_rx_method_combo_box->setCurrentIndex (w7pp_flex_native_rx_ ? 1 : 0);
  ui_->w7pp_flex_dax_channel_combo_box->setCurrentIndex (w7pp_flex_dax_channel_ - 1);
  update_w7pp_flex_rx_controls ();
```

Accepting the dialog, in `accept`:

```cpp
  w7pp_flex_native_rx_ =
      (ui_->w7pp_flex_rx_method_combo_box->currentIndex () == 1);
  w7pp_flex_dax_channel_ =
      ui_->w7pp_flex_dax_channel_combo_box->currentIndex () + 1;
```

Validation bypass — the two audio-device checks must not reject an empty sound input when native RX is selected. Change each guard to require standard RX first:

```cpp
  if (ui_->w7pp_flex_rx_method_combo_box->currentIndex () == 0 && ui_->sound_input_combo_box->currentIndex () < 0
```

and

```cpp
  if (ui_->w7pp_flex_rx_method_combo_box->currentIndex () == 0 && ui_->sound_input_channel_combo_box->currentIndex () < 0
```

Rig-start handoff, where `rig_params_` is finalised:

```cpp
  // Flex Native VITA-49 backend.
  QCoreApplication::instance ()->setProperty (
      "W7PPNativeFlexTxCapture",
      m_->rig_params_.rig_name == "Flex Native VITA-49");

  // Native FLEX only: supply selected DAX channel before rig start.
  if (m_->rig_params_.rig_name == "Flex Native VITA-49")
    {
      NativeFlexTransceiver::set_dax_channel(
          m_->w7pp_flex_dax_channel_);
    }
```

Radio-selection hook, where the rig combo selection is handled:

```cpp
  // Only this exact W7PP backend activates Native FLEX.
  if (ui_->rig_combo_box->currentText() == "Flex Native VITA-49")
    {
      NativeFlexRadioSelection::refresh(this);
    }
```

Cross-check every one of these against the donor line numbers listed in the spec's file table before considering the step done.

- [ ] **Step 4: Verify it builds**

Run: `cmake --build build -j8`
Expected: PASS.

- [ ] **Step 5: Verify the settings round-trip by inspection**

There is no headless test for `Configuration` — it requires a full `QApplication` and a settings file. Verify by reading the code that:
- a fresh profile with no stored keys yields `flex_native_rx() == false` and `flex_dax_channel() == 1`
- a stored `W7PPFlexDaxChannel` of `0`, `9` or a non-integer clamps to `1`
- `W7PPNativeFlexTxCapture` is set `false` for every rig except `"Flex Native VITA-49"`

State in the commit message that this was verified by inspection, not by test.

- [ ] **Step 6: Commit**

```bash
git add Configuration.hpp Configuration.cpp Configuration.ui
git commit -m "Add Native FLEX configuration: RX method and DAX channel

Adds the Audio-tab RX method selector and DAX channel combo, settings
keys W7PPFlexNativeRx and W7PPFlexDaxChannel, the audio-device
validation bypass for native RX, and the DAX channel handoff before rig
start. Settings round-trip verified by inspection; Configuration has no
headless test harness."
```

---

### Task 5: VITA-49 transmit path

**Files:**
- Modify: `Audio/soundout.h`, `Audio/soundout.cpp`, `Modulator/Modulator.cpp`

**Interfaces:**
- Consumes: the `W7PPNativeFlexTxCapture` application property set in Task 4.
- Produces, for Task 7: `SoundOutput` reads application properties `W7PPNativeFlexTxRadioAddress` (QByteArray) and `W7PPNativeFlexTxStreamId` (quint32), which `NativeFlexTransceiver` publishes.

- [ ] **Step 1: Port the SoundOutput members**

Copy from `$W7PP/Audio/soundout.h` into `Audio/soundout.h`:

Method declarations:

```cpp
  void nativeFlexPump ();
  void nativeFlexTxPace ();
  void nativeFlexWriteVitaPacket ();
  void nativeFlexCloseVitaDump ();
```

Members (donor lines 71-87), verbatim including the comment:

```cpp
  // W7PP Native FLEX software TX clock.
  QIODevice * m_native_flex_source;
  QTimer * m_native_flex_timer;
  QByteArray m_native_flex_vita_payload;
  quint8 m_native_flex_vita_sequence {0};
  int m_native_flex_decimate_phase {0};
  quint32 m_native_flex_vita_dump_stream_id {0x84000000u};
  QByteArray m_native_flex_tx_radio_address;
  quint32 m_native_flex_tx_stream_id {0};
  QQueue<QByteArray> m_native_flex_tx_packets;
  QTimer * m_native_flex_tx_timer {nullptr};
  QUdpSocket * m_native_flex_tx_socket {nullptr};
  QElapsedTimer m_native_flex_tx_elapsed;
  qint64 m_native_flex_tx_next_packet_us {0};
  int m_native_flex_tx_pace_phase {0};
  qint64 m_native_flex_bytes_per_frame;
  QByteArray m_native_flex_buffer;
```

Constructor initialisers (donor lines 31-33):

```cpp
    , m_native_flex_source {nullptr}
    , m_native_flex_timer {nullptr}
    , m_native_flex_bytes_per_frame {0}
```

Add the includes `<QQueue>`, `<QUdpSocket>`, `<QElapsedTimer>`, `<QTimer>`, `<QByteArray>` if not already present.

- [ ] **Step 2: Port the SoundOutput implementation**

Copy from `$W7PP/Audio/soundout.cpp`:
- the file-scope `static QFile w7pp_native_flex_vita_dump_file;` (donor line 54)
- the `native_flex` branch at the top of the stream-start method (donor lines 67-130), which reads the `W7PPNativeFlexTxCapture` property, verifies the source is an `AudioDevice`, stops any existing timer, records `m_native_flex_bytes_per_frame`, and reads `W7PPNativeFlexTxRadioAddress` and `W7PPNativeFlexTxStreamId`
- the four method bodies `nativeFlexPump`, `nativeFlexTxPace`, `nativeFlexWriteVitaPacket`, `nativeFlexCloseVitaDump`

Port verbatim. This is real-time packet pacing; do not restructure it.

- [ ] **Step 3: Port the Modulator TX capture**

Copy from `$W7PP/Modulator/Modulator.cpp` into `Modulator/Modulator.cpp`:
- file-scope `QFile w7pp_native_flex_tx_capture_file;` (donor line 7)
- the capture open/close logic at donor lines 72-95, gated on the `W7PPNativeFlexTxCapture` property *and* the `W7PP_NATIVE_FLEX_TX_CAPTURE_FILE` environment variable
- the close at donor lines 189-191
- the write at donor lines 219-228

This is a debug aid that stays inert unless the environment variable is set. Preserve that gating exactly — an always-on file write in the modulator would be a real-time hazard.

- [ ] **Step 4: Verify it builds**

Run: `cmake --build build -j8`
Expected: PASS.

- [ ] **Step 5: Verify inertness by inspection**

Read every new branch and confirm that with `W7PPNativeFlexTxCapture` false (that is, any rig other than `"Flex Native VITA-49"`):
- `m_native_flex_timer` and `m_native_flex_tx_timer` are never constructed or started
- `m_native_flex_tx_socket` is never constructed
- `w7pp_native_flex_tx_capture_file` is never opened
- the stock `QAudioOutput` path runs exactly as before

This is the acceptance criterion. If any branch is reachable with the property false, fix it before committing.

- [ ] **Step 6: Commit**

```bash
git add Audio/soundout.h Audio/soundout.cpp Modulator/Modulator.cpp
git commit -m "Add Native FLEX VITA-49 transmit path

Software 48 kHz TX clock, VITA-49 packetiser and paced UDP output in
SoundOutput, plus an env-gated TX PCM capture in Modulator for
debugging. All paths inert unless the Flex Native VITA-49 rig is
selected."
```

---

### Task 6: Receive bridge in MainWindow

**Files:**
- Modify: `widgets/mainwindow.h`, `widgets/mainwindow.cpp`, `widgets/mainwindow.ui`

**Interfaces:**
- Consumes: `FlexVitaReceiver` from Task 2; `Configuration::flex_native_rx()` and `flex_dax_channel()` from Task 4.
- Produces, for Task 7: `m_flexVitaReceiver` member and the `syncFlexVitaReceiver()` entry point.

- [ ] **Step 1: Add the UI controls**

From `$W7PP/widgets/mainwindow.ui`:
- donor lines 1274-1400: `w7ppFlexTxAudioLabel`, `w7ppFlexTxAudioAttenuation` slider, `w7ppFlexTxAudioScaleWidget` with its scale labels and spacers
- donor lines 1550-1629, between the `<!-- W7PP BEGIN NATIVE FLEX RX GAIN -->` and `<!-- W7PP END NATIVE FLEX RX GAIN -->` markers: `w7ppFlexRxGainWidget`, `w7ppFlexRxGainTitle`, `w7ppFlexRxGainSlider`, `w7ppFlexRxGainValue`

Place them in the equivalent layouts in WSJT-Z's `mainwindow.ui`. Keep the W7PP comment markers. Widget names must match exactly.

- [ ] **Step 2: Add the header declarations**

In `widgets/mainwindow.h`, add the include and the forward-declared receiver:

```cpp
#include "Transceiver/FlexVitaReceiver.hpp"
```

Method declarations (donor lines 156-160), verbatim:

```cpp
  // W7PP native Flex 12 kHz decoder-buffer ingress.
  void flexDataSink (FlexVitaReceiver::DecoderBlock const& samples);
  void syncFlexVitaReceiver ();
  void verifyFlexVitaStart (int generation, quint64 baselinePackets, int checksRemaining = 20);
  void armFlexVitaWatchdog (int generation);
```

Receiver member (donor line 566) — note this is a **raw pointer**, not a smart
pointer, and `~MainWindow()` is responsible for stopping it:

```cpp
  FlexVitaReceiver * m_flexVitaReceiver;
```

State members (donor lines 731-737), verbatim — note `m_flex_rx_audio` and
`m_flex_last_period_msec` deliberately have no brace initialiser; they are set
in the constructor's initialiser list:

```cpp
  bool    m_flex_rx_audio;  // optional native Flex RX
  qint64  m_flex_last_period_msec;
  int     m_flex_vita_dax_channel {0};
  int     m_flex_vita_start_attempt {0};
  int     m_flex_vita_generation {0};
  bool    m_flex_vita_watchdog_armed {false};
  quint64 m_flex_vita_watchdog_packets {0};
```

The safety members and status-bar widgets (donor lines 817-854) belong to
Task 7; do not add them yet.

- [ ] **Step 3: Port `flexDataSink` verbatim**

Copy the complete `MainWindow::flexDataSink` body from `$W7PP/widgets/mainwindow.cpp`. Do not reinterpret it.

It reproduces the stock `Detector` producer contract: accumulate 12 kHz samples, call `dataSink()` only on `m_FFTSize` boundaries, and reset `dec_data.params.kin` on T/R period wrap. TCI honours the same contract. Getting it wrong desynchronises the decoder in ways that look like poor propagation rather than a bug.

Note the deliberate 0 dB fast path: at exactly 0 dB gain it copies samples with no arithmetic, preserving bit-exact behaviour. Keep that branch.

- [ ] **Step 4: Port the remaining new methods**

Copy verbatim from the donor: `syncFlexVitaReceiver`, `verifyFlexVitaStart`, `armFlexVitaWatchdog`. Leave `nativeFlexSafetyTrip` and `nativeFlexSafetyProblemText` for Task 7 — add them as stubs now only if the build requires it, and if so mark each with `// W7PP : implemented in the safety-monitor task`.

- [ ] **Step 5: Port the insertions into existing methods**

Each of these WSJT-Z methods needs the donor's W7PP-marked insertions. Locate them in the donor by searching for `W7PP` within the method, and place them at the equivalent point in WSJT-Z's version:

`MainWindow()` (construct the receiver, set the audio callback), `~MainWindow()` (stop it), `guiUpdate()` (the largest set — level meters, watchdog polling, TX audio attenuation), `readSettings()`, `writeSettings()`, `on_actionSettings_triggered()`, `on_autoButton_clicked()`, `monitor()`, `stopTx()`, `stopTx2()`, `fixStop()`, `on_outAttenuation_valueChanged()`, `handle_transceiver_failure()`, `WSPR_scheduling()`, `on_tuneButton_clicked()`, `on_monitorButton_clicked()`, `end_tuning()`, `band_changed()`.

Leave `createStatusBar()` for Task 7.

All eighteen exist in WSJT-Z and were verified present. Where WSJT-Z's version of a method has diverged from the donor's, place the insertion by what it is *for* — the W7PP comment above each hunk says — not by line position.

- [ ] **Step 6: Verify it builds and the receiver tests still pass**

Run:
```bash
cmake --build build -j8 && ./build/tests/test_flex_vita_receiver
```
Expected: PASS both.

- [ ] **Step 7: Verify inertness by inspection**

Confirm `syncFlexVitaReceiver()` starts nothing when `Configuration::flex_native_rx()` is false, and that `flexDataSink` returns immediately when `m_flex_rx_audio` is false. Confirm the receiver object itself opens no socket until `start()` is called.

- [ ] **Step 8: Commit**

```bash
git add widgets/mainwindow.h widgets/mainwindow.cpp widgets/mainwindow.ui
git commit -m "Add Native FLEX receive bridge to MainWindow

Routes decimated 12 kHz VITA audio into dec_data.d2 on FFT boundaries,
matching the stock Detector and TCI producer contract, with a start
watchdog and an operator RX gain control."
```

---

### Task 7: Safety monitor and status bar

**Files:**
- Modify: `widgets/mainwindow.cpp` (`createStatusBar`, safety methods)

**Interfaces:**
- Consumes: `NativeFlexSafetyMonitor` from Task 2; the `"Native FLEX TX INHIBITED:"` handling from Task 3; `m_nativeFlexSafetyInhibited` / `m_nativeFlexSafetyPopupPending` from Task 6.
- Produces: nothing downstream. This is the last task.

- [ ] **Step 1: Port the safety methods and their members**

Add to `widgets/mainwindow.h` (donor lines 851-854):

```cpp
  QString nativeFlexSafetyProblemText(bool require_ready) const;
  void nativeFlexSafetyTrip(QString const& problems);
  bool m_nativeFlexSafetyInhibited {false};
  bool m_nativeFlexSafetyPopupPending {false};
```

Add the status-bar widget members (donor lines 817-830), alongside WSJT-Z's
existing status-bar widget members:

```cpp
  QWidget flex_meter_row;
  QLabel flex_power_label;
  QLabel flex_swr_label;
  QLabel flex_voltage_label;
  QLabel flex_temperature_label;
  QProgressBar flex_level_meter;
  QPushButton flex_atu_button;
  QPushButton flex_bypass_button;
```

These are value members, not pointers — they must appear in the same
declaration order the donor uses relative to each other, and any that WSJT-Z's
constructor initialiser list touches must be initialised in declaration order
to avoid `-Wreorder` warnings.

Then copy verbatim from `$W7PP/widgets/mainwindow.cpp`: `MainWindow::nativeFlexSafetyTrip(QString const& problems)` and `MainWindow::nativeFlexSafetyProblemText(bool require_ready) const`, replacing any stubs left from Task 6.

- [ ] **Step 2: Port the status bar indicators**

`createStatusBar()` carries the second-largest set of W7PP changes. Copy the W7PP-marked hunks from the donor: the Flex meter labels and their stylesheets (`ppFlexMeterNeutralStyle`, `ppFlexMeterGreenStyle`, `ppFlexMeterYellowStyle`, `ppFlexMeterRedStyle`) and the ATU indicator styles (`ppFlexAtuNeutralStyle`, `ppFlexAtuGreenStyle`, `ppFlexAtuBlueStyle`, `ppFlexAtuRedStyle`).

Place them in WSJT-Z's `createStatusBar()` in the same relative order as the donor. Where WSJT-Z has already added its own status-bar widgets, append the Flex ones rather than displacing anything.

- [ ] **Step 3: Verify the safety telemetry wiring**

Confirm the connection from `NativeFlexSafetyMonitor` to `nativeFlexSafetyTrip` exists — check `$W7PP/Transceiver/NativeFlexTransceiver.cpp` and the donor's `MainWindow` constructor for how the trip reaches the GUI thread. A cross-thread signal must be queued, not direct.

- [ ] **Step 4: Verify the full build and the whole test suite**

Run:
```bash
cmake --build build -j8 \
  && ./build/tests/test_qt_helpers \
  && ./build/tests/test_flex_socket_compat \
  && ./build/tests/test_flex_vita_receiver \
  && ./build/tests/test_native_flex_factory
```
Expected: build PASS, all four test binaries PASS.

- [ ] **Step 5: Verify Linux portability of the receiver**

The macOS build exercises the `SO_NOSIGPIPE` branch of the shim; Linux uses `MSG_NOSIGNAL` instead. Compile-check the other branch:

```bash
clang++ -fsyntax-only -std=c++11 -DMSG_NOSIGNAL=0x4000 -USO_NOSIGPIPE \
  -I. -I build \
  $(pkg-config --cflags Qt5Core 2>/dev/null || echo -I/opt/homebrew/opt/qt@5/include -I/opt/homebrew/opt/qt@5/include/QtCore) \
  Transceiver/FlexVitaReceiver.cpp
```

Expected: no errors. If the environment cannot resolve Qt headers this way, report that the Linux branch could not be compile-checked rather than claiming it passed.

- [ ] **Step 6: Final inertness audit**

Grep every guard and confirm the feature is fully inert with a non-Flex rig:

```bash
grep -n "Flex Native VITA-49\|W7PPNativeFlexTxCapture\|flex_native_rx\|m_flex_rx_audio" \
  Configuration.cpp Audio/soundout.cpp Modulator/Modulator.cpp widgets/mainwindow.cpp
```

For each hit, confirm the guarded code cannot run when the rig is something else. This is the property that makes the port safe to merge without hardware.

- [ ] **Step 7: Commit**

```bash
git add widgets/mainwindow.cpp
git commit -m "Add Native FLEX safety monitor and status bar indicators

Completes the Native FLEX port: PA/SWR/ATU telemetry indicators and
recoverable TX inhibit handling in the GUI."
```

---

## Completion criteria

The port is done when:

1. `cmake --build build -j8` succeeds on macOS with no new warnings in the ported files.
2. All four test binaries pass.
3. The Linux branch of `FlexSocketCompat.hpp` compile-checks, or its failure to do so is reported.
4. The inertness audit in Task 7 Step 6 finds no reachable Native FLEX code path when the rig is not `"Flex Native VITA-49"`.
5. W7PP attribution and naming are intact throughout.

**Not** included in "done": any claim about on-air behaviour. VITA-49 framing against real hardware, DAX channel negotiation, decode quality, TX timing and safety telemetry are unverified and can only be confirmed by an operator with a FlexRadio. Say so when reporting completion.
