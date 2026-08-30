#include "NativeFlexSafetyMonitor.hpp"

// W7PP : portability shim, see Transceiver/FlexSocketCompat.hpp
#include "FlexSocketCompat.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

namespace
{
  std::uint32_t readBigEndian32(
      unsigned char const * p)
  {
    return
        (static_cast<std::uint32_t>(p[0]) << 24)
        | (static_cast<std::uint32_t>(p[1]) << 16)
        | (static_cast<std::uint32_t>(p[2]) << 8)
        | static_cast<std::uint32_t>(p[3]);
  }


  std::int16_t lowerSigned16(
      std::uint32_t word)
  {
    return static_cast<std::int16_t>(
        word & 0xFFFFu);
  }


  std::string trimLine(
      std::string line)
  {
    while (
        !line.empty()
        && ('\r' == line.back()
            || '\n' == line.back()))
      {
        line.pop_back();
      }

    return line;
  }


  bool sendAll(
      SOCKET socket,
      std::string const& text)
  {
    char const * data = text.data();
    int remaining = static_cast<int>(text.size());

    while (remaining > 0)
      {
        int const sent =
            static_cast<int>(
                ::send(
                    socket,
                    data,
                    remaining,
                    flexSendFlags()));

        if (sent <= 0)
          return false;

        data += sent;
        remaining -= sent;
      }

    return true;
  }


  bool sendCommand(
      SOCKET socket,
      unsigned sequence,
      std::string const& command)
  {
    std::ostringstream wire;

    wire
        << 'C'
        << sequence
        << '|'
        << command
        << "\r\n";

    return sendAll(
        socket,
        wire.str());
  }


  bool socketReadable(
      SOCKET socket,
      long microseconds)
  {
    fd_set readSet;

    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);

    timeval timeout {};

    timeout.tv_sec =
        microseconds / 1000000L;

    timeout.tv_usec =
        microseconds % 1000000L;

    int const result =
        ::select(
            socket + 1,
            &readSet,
            nullptr,
            nullptr,
            &timeout);

    return
        result > 0
        && FD_ISSET(socket, &readSet);
  }


  /*
   * :
   *
   * Process-wide compact safety gate cache.
   *
   * The independent safety worker is the only writer.
   * Future Native FLEX PTT will be a reader only.
   */
  std::atomic<bool> g_gateMonitorConnected {false};
  std::atomic<bool> g_gateMeterStreamActive {false};

  std::atomic<bool> g_gateInterlockSeen {false};
  std::atomic<bool> g_gateTxAllowed {false};
  std::atomic<bool> g_gateInterlockReady {false};

  std::atomic<bool> g_gateAllMetersSeen {false};

  std::atomic<std::int64_t> g_gateInterlockNs {-1};
  std::atomic<std::int64_t> g_gateMeterNs {-1};

  // W7PP display-only FLEX meter cache.
  std::atomic<bool> g_displayLevelSeen {false};
  std::atomic<bool> g_displayForwardPowerSeen {false};
  std::atomic<bool> g_displaySwrSeen {false};
  std::atomic<bool> g_displayVoltageBSeen {false};
  std::atomic<bool> g_displayPaTemperatureSeen {false};

  // W7PP ATU cache.
  // command request:
  //   0 = none
  //   1 = atu start
  //   2 = atu bypass
  std::atomic<bool> g_displayAtuSeen {false};

  std::atomic<int> g_displayAtuState {
      static_cast<int>(
          NativeFlexSafetyMonitor::AtuState::unknown)};

  std::atomic<int> g_atuCommandRequest {0};

  std::atomic<std::int16_t> g_displayLevelRaw {0};
  std::atomic<std::int16_t> g_displayForwardPowerRaw {0};
  std::atomic<std::int16_t> g_displaySwrRaw {0};
  std::atomic<std::int16_t> g_displayVoltageBRaw {0};
  std::atomic<std::int16_t> g_displayPaTemperatureRaw {0};


  std::int64_t gateNowNs() noexcept
  {
    return
        std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                NativeFlexSafetyMonitor::Clock::now()
                    .time_since_epoch())
            .count();
  }


  void clearGateCache() noexcept
  {
    g_gateMonitorConnected.store(
        false,
        std::memory_order_release);

    g_gateMeterStreamActive.store(
        false,
        std::memory_order_release);

    g_gateInterlockSeen.store(
        false,
        std::memory_order_release);

    g_gateTxAllowed.store(
        false,
        std::memory_order_release);

    g_gateInterlockReady.store(
        false,
        std::memory_order_release);

    g_gateAllMetersSeen.store(
        false,
        std::memory_order_release);

    g_gateInterlockNs.store(
        -1,
        std::memory_order_release);

    g_gateMeterNs.store(
        -1,
        std::memory_order_release);

    g_displayLevelSeen.store(false, std::memory_order_release);
    g_displayForwardPowerSeen.store(false, std::memory_order_release);
    g_displaySwrSeen.store(false, std::memory_order_release);
    g_displayVoltageBSeen.store(false, std::memory_order_release);
    g_displayPaTemperatureSeen.store(false, std::memory_order_release);

    g_displayAtuSeen.store(false, std::memory_order_release);

    g_displayAtuState.store(
        static_cast<int>(
            NativeFlexSafetyMonitor::AtuState::unknown),
        std::memory_order_release);

    g_atuCommandRequest.store(
        0,
        std::memory_order_release);

    g_displayLevelRaw.store(0, std::memory_order_release);
    g_displayForwardPowerRaw.store(0, std::memory_order_release);
    g_displaySwrRaw.store(0, std::memory_order_release);
    g_displayVoltageBRaw.store(0, std::memory_order_release);
    g_displayPaTemperatureRaw.store(0, std::memory_order_release);
  }
}


struct NativeFlexSafetyMonitor::Impl
{
  enum class MeterKind
  {
    unknown,
    voltage_a,
    voltage_b,
    forward_power,
    reflected_power,
    swr,
    pa_temperature,
    tx_level
  };


  explicit Impl(
      NativeFlexSafetyMonitor * ownerIn)
    : owner {ownerIn}
  {
  }


  ~Impl()
  {
    stop();
  }


  void invalidateSnapshot()
  {
    std::lock_guard<std::mutex> guard {snapshotMutex};

    snapshot =
        NativeFlexSafetyMonitor::Snapshot {};

    clearGateCache();
  }


  void setError(
      std::string text)
  {
    std::lock_guard<std::mutex> guard {errorMutex};

    error =
        std::move(text);
  }


  std::string getError() const
  {
    std::lock_guard<std::mutex> guard {errorMutex};

    return error;
  }


  void recomputeValidLocked()
  {
    snapshot.valid =
        snapshot.monitor_connected
        && snapshot.interlock_seen;
  }


  void setMonitorConnected(
      bool connected)
  {
    std::lock_guard<std::mutex> guard {snapshotMutex};

    snapshot.monitor_connected =
        connected;

    g_gateMonitorConnected.store(
        connected,
        std::memory_order_release);

    recomputeValidLocked();
  }


  void setMeterStreamActive(
      bool active)
  {
    std::lock_guard<std::mutex> guard {snapshotMutex};

    snapshot.meter_stream_active =
        active;

    g_gateMeterStreamActive.store(
        active,
        std::memory_order_release);

    recomputeValidLocked();
  }


  void captureInterlock(
      std::string const& line)
  {
    auto const marker =
        line.find("|interlock ");

    if (std::string::npos == marker)
      return;

    std::string const fields =
        line.substr(
            marker
            + std::strlen("|interlock "));

    bool haveState = false;
    bool haveAllowed = false;

    std::string state;
    std::string reason;
    std::string source;

    bool allowed = false;

    std::istringstream input {fields};

    std::string token;

    while (input >> token)
      {
        auto const equals =
            token.find('=');

        if (std::string::npos == equals)
          continue;

        std::string const key =
            token.substr(0, equals);

        std::string const value =
            token.substr(equals + 1);

        if ("state" == key)
          {
            state = value;
            haveState = true;
          }
        else if ("reason" == key)
          {
            reason = value;
          }
        else if ("source" == key)
          {
            source = value;
          }
        else if ("tx_allowed" == key)
          {
            allowed =
                ("1" == value);

            haveAllowed = true;
          }
      }

    if (!haveState || !haveAllowed)
      return;

    std::lock_guard<std::mutex> guard {snapshotMutex};

    snapshot.interlock_seen = true;
    snapshot.tx_allowed = allowed;

    snapshot.interlock_state =
        std::move(state);

    snapshot.interlock_reason =
        std::move(reason);

    snapshot.interlock_source =
        std::move(source);

    snapshot.interlock_updated =
        NativeFlexSafetyMonitor::Clock::now();

    g_gateInterlockSeen.store(
        true,
        std::memory_order_release);

    g_gateTxAllowed.store(
        snapshot.tx_allowed,
        std::memory_order_release);

    g_gateInterlockReady.store(
        "READY" == snapshot.interlock_state,
        std::memory_order_release);

    g_gateInterlockNs.store(
        gateNowNs(),
        std::memory_order_release);

    recomputeValidLocked();
  }


  void captureAtu(
      std::string const& line)
  {
    auto const marker =
        line.find("|atu ");

    if (std::string::npos == marker)
      return;

    auto const statusMarker =
        line.find("status=", marker + 5);

    if (std::string::npos == statusMarker)
      return;

    std::size_t const valueStart =
        statusMarker + std::strlen("status=");

    std::size_t const valueEnd =
        line.find_first_of(" \r\n", valueStart);

    std::string const status =
        line.substr(
            valueStart,
            std::string::npos == valueEnd
                ? std::string::npos
                : valueEnd - valueStart);

    NativeFlexSafetyMonitor::AtuState state =
        NativeFlexSafetyMonitor::AtuState::unknown;

    if ("TUNE_MANUAL_BYPASS" == status)
      {
        state =
            NativeFlexSafetyMonitor::AtuState::bypassed;
      }
    else if ("TUNE_IN_PROGRESS" == status)
      {
        state =
            NativeFlexSafetyMonitor::AtuState::in_progress;
      }
    else if ("TUNE_SUCCESSFUL" == status)
      {
        state =
            NativeFlexSafetyMonitor::AtuState::successful;
      }

    g_displayAtuState.store(
        static_cast<int>(state),
        std::memory_order_release);

    g_displayAtuSeen.store(
        true,
        std::memory_order_release);
  }


  static MeterKind meterKindForName(
      std::string const& name)
  {
    if ("+13.8A" == name)
      return MeterKind::voltage_a;

    if ("+13.8B" == name)
      return MeterKind::voltage_b;

    if ("FWDPWR" == name)
      return MeterKind::forward_power;

    if ("REFPWR" == name)
      return MeterKind::reflected_power;

    if ("SWR" == name)
      return MeterKind::swr;

    if ("PATEMP" == name)
      return MeterKind::pa_temperature;

    if ("GAIN" == name)
      return MeterKind::tx_level;

    return MeterKind::unknown;
  }


  void captureMeterMetadata(
      std::string const& line)
  {
    /*
     * FLEX meter definitions may all be packed into one line:
     *
     *   ...#4.nam=+13.8A#...
     *   ...#5.nam=+13.8B#...
     *   ...#6.nam=FWDPWR#...
     *   ...#7.nam=REFPWR#...
     *   ...#8.nam=SWR#...
     *   ...#9.nam=PATEMP#...
     *
     * Scan every .nam= field in the line.
     *
     * The VITA meter identifier is the decimal number directly
     * between the preceding '#' and ".nam=".
     *
     * Unknown meter names are ignored.
     *
     * This parser handles metadata only.  It has no RX-audio,
     * TX-audio, PTT or DAX-stream involvement.
     */
    std::size_t search = 0;

    for (;;)
      {
        auto const nameMarker =
            line.find(
                ".nam=",
                search);

        if (std::string::npos == nameMarker)
          break;


        auto const idMarker =
            line.rfind(
                '#',
                nameMarker);


        if (std::string::npos == idMarker)
          {
            search =
                nameMarker
                + std::strlen(".nam=");

            continue;
          }


        std::size_t const idBegin =
            idMarker + 1;


        if (idBegin >= nameMarker)
          {
            search =
                nameMarker
                + std::strlen(".nam=");

            continue;
          }


        std::string const idText =
            line.substr(
                idBegin,
                nameMarker - idBegin);


        char * end = nullptr;

        unsigned long const parsedId =
            std::strtoul(
                idText.c_str(),
                &end,
                10);


        if (
            !end
            || '\0' != *end
            || parsedId > 0xFFFFul)
          {
            search =
                nameMarker
                + std::strlen(".nam=");

            continue;
          }


        std::size_t const nameBegin =
            nameMarker
            + std::strlen(".nam=");


        auto const nameEnd =
            line.find(
                '#',
                nameBegin);


        std::string const name =
            line.substr(
                nameBegin,
                std::string::npos == nameEnd
                    ? std::string::npos
                    : nameEnd - nameBegin);


        MeterKind const kind =
            meterKindForName(name);


        if (MeterKind::unknown != kind)
          {
            meterKinds[
                static_cast<std::uint16_t>(
                    parsedId)] =
                kind;
          }


        if (std::string::npos == nameEnd)
          break;


        search =
            nameEnd + 1;
      }
  }

  void captureResponse(
      std::string const& line)
  {
    if (
        line.size() < 4
        || 'R' != line[0])
      {
        return;
      }

    auto const firstBar =
        line.find('|');

    if (std::string::npos == firstBar)
      return;

    auto const secondBar =
        line.find(
            '|',
            firstBar + 1);

    if (std::string::npos == secondBar)
      return;

    std::string const sequenceText =
        line.substr(
            1,
            firstBar - 1);

    std::string const codeText =
        line.substr(
            firstBar + 1,
            secondBar - firstBar - 1);

    char * sequenceEnd = nullptr;
    char * codeEnd = nullptr;

    unsigned long const sequence =
        std::strtoul(
            sequenceText.c_str(),
            &sequenceEnd,
            10);

    unsigned long const code =
        std::strtoul(
            codeText.c_str(),
            &codeEnd,
            16);

    if (
        !sequenceEnd
        || '\0' != *sequenceEnd
        || !codeEnd
        || '\0' != *codeEnd)
      {
        return;
      }

    responses[
        static_cast<unsigned>(sequence)] =
        static_cast<std::uint32_t>(code);
  }


  void captureLine(
      std::string line)
  {
    line =
        trimLine(
            std::move(line));

    if (line.empty())
      return;

    if ('H' == line[0])
      {
        haveClientHandle = true;
        return;
      }

    captureInterlock(line);
    captureAtu(line);
    captureMeterMetadata(line);
    captureResponse(line);
  }


  void consumeTcp(
      char const * data,
      int length)
  {
    pendingTcp.append(
        data,
        static_cast<std::size_t>(length));

    for (;;)
      {
        auto const newline =
            pendingTcp.find('\n');

        if (std::string::npos == newline)
          break;

        std::string line =
            pendingTcp.substr(
                0,
                newline + 1);

        pendingTcp.erase(
            0,
            newline + 1);

        captureLine(
            std::move(line));
      }
  }


  bool receiveTcpOnce(
      SOCKET tcp)
  {
    std::array<char, 65536> buffer {};

    int const received =
        ::recv(
            tcp,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0);

    if (received > 0)
      {
        consumeTcp(
            buffer.data(),
            received);

        return true;
      }

    if (0 == received)
      return false;

    //
    // A negative result is a receive error, not necessarily a
    // disconnect. EINTR/EAGAIN mean "try again", not "give up".
    //
    return flexIsRetryableReceiveError(WSAGetLastError());
  }


  bool waitForHandle(
      SOCKET tcp,
      std::chrono::milliseconds duration)
  {
    auto const deadline =
        NativeFlexSafetyMonitor::Clock::now()
        + duration;

    while (
        !stopRequested.load()
        && NativeFlexSafetyMonitor::Clock::now() < deadline)
      {
        if (!socketReadable(tcp, 100000L))
          continue;

        if (!receiveTcpOnce(tcp))
          return false;

        if (haveClientHandle)
          return true;
      }

    return
        haveClientHandle;
  }


  bool takeResponse(
      unsigned sequence,
      std::uint32_t& code)
  {
    auto const found =
        responses.find(sequence);

    if (responses.end() == found)
      return false;

    code =
        found->second;

    responses.erase(found);

    return true;
  }


  bool waitForResponse(
      SOCKET tcp,
      unsigned sequence,
      std::chrono::milliseconds duration)
  {
    auto const deadline =
        NativeFlexSafetyMonitor::Clock::now()
        + duration;

    for (;;)
      {
        std::uint32_t code = 0;

        if (takeResponse(sequence, code))
          return 0 == code;

        if (
            stopRequested.load()
            || NativeFlexSafetyMonitor::Clock::now() >= deadline)
          {
            return false;
          }

        if (!socketReadable(tcp, 100000L))
          continue;

        if (!receiveTcpOnce(tcp))
          return false;
      }
  }


  void captureMeterPacket(
      unsigned char const * data,
      int received)
  {
    /*
     * Independent safety meter VITA only.
     *
     * Proven live FLEX class:
     *
     *   OUI      0x00001C2D
     *   class    0x534C8002
     *
     * No RX DAX stream ID and no TX DAX stream ID is accepted
     * or processed here.
     */
    if (received < 32)
      return;

    std::uint32_t const header =
        readBigEndian32(data);

    unsigned const packetType =
        (header >> 28) & 0x0Fu;

    bool const hasClass =
        0 != (header & 0x08000000u);

    if (
        3u != packetType
        || !hasClass)
      {
        return;
      }

    std::uint32_t const oui =
        readBigEndian32(data + 8);

    std::uint32_t const classId =
        readBigEndian32(data + 12);

    if (
        0x00001C2Du != oui
        || 0x534C8002u != classId)
      {
        return;
      }

    std::uint32_t const declaredWords =
        header & 0xFFFFu;

    std::size_t const declaredBytes =
        static_cast<std::size_t>(
            declaredWords)
        * 4u;

    std::size_t const usableBytes =
        std::min(
            declaredBytes,
            static_cast<std::size_t>(received));

    /*
     * Meter payload starts at VITA word 7 in the proven FLEX
     * meter packet shape.
     */
    for (
        std::size_t offset = 28;
        offset + 4 <= usableBytes;
        offset += 4)
      {
        std::uint32_t const word =
            readBigEndian32(
                data + offset);

        std::uint16_t const meterId =
            static_cast<std::uint16_t>(
                (word >> 16) & 0xFFFFu);

        auto const found =
            meterKinds.find(meterId);

        if (meterKinds.end() == found)
          continue;

        std::int16_t const raw =
            lowerSigned16(word);

        std::lock_guard<std::mutex> guard {snapshotMutex};

        switch (found->second)
          {
          case MeterKind::voltage_a:
            snapshot.voltage_a_seen = true;
            snapshot.voltage_a_raw = raw;
            break;

          case MeterKind::voltage_b:
            snapshot.voltage_b_seen = true;
            snapshot.voltage_b_raw = raw;
            g_displayVoltageBRaw.store(raw, std::memory_order_release);
            g_displayVoltageBSeen.store(true, std::memory_order_release);
            break;

          case MeterKind::forward_power:
            snapshot.forward_power_seen = true;
            snapshot.forward_power_raw = raw;
            g_displayForwardPowerRaw.store(raw, std::memory_order_release);
            g_displayForwardPowerSeen.store(true, std::memory_order_release);
            break;

          case MeterKind::reflected_power:
            snapshot.reflected_power_seen = true;
            snapshot.reflected_power_raw = raw;
            break;

          case MeterKind::swr:
            snapshot.swr_seen = true;
            snapshot.swr_raw = raw;
            g_displaySwrRaw.store(raw, std::memory_order_release);
            g_displaySwrSeen.store(true, std::memory_order_release);
            break;

          case MeterKind::pa_temperature:
            snapshot.pa_temperature_seen = true;
            snapshot.pa_temperature_raw = raw;
            g_displayPaTemperatureRaw.store(raw, std::memory_order_release);
            g_displayPaTemperatureSeen.store(true, std::memory_order_release);
            break;

          case MeterKind::tx_level:
            g_displayLevelRaw.store(raw, std::memory_order_release);
            g_displayLevelSeen.store(true, std::memory_order_release);
            break;

          case MeterKind::unknown:
            break;
          }

        snapshot.meter_updated =
            NativeFlexSafetyMonitor::Clock::now();

        bool const allMetersSeen =
            snapshot.voltage_a_seen
            && snapshot.voltage_b_seen
            && snapshot.forward_power_seen
            && snapshot.reflected_power_seen
            && snapshot.swr_seen
            && snapshot.pa_temperature_seen;

        g_gateAllMetersSeen.store(
            allMetersSeen,
            std::memory_order_release);

        g_gateMeterNs.store(
            gateNowNs(),
            std::memory_order_release);
      }
  }


  void closeSockets()
  {
    SOCKET const tcp =
        tcpSocket.exchange(
            INVALID_SOCKET);

    if (INVALID_SOCKET != tcp)
      {
        ::shutdown(
            tcp,
            SD_BOTH);

        ::closesocket(tcp);
      }

    SOCKET const udp =
        udpSocket.exchange(
            INVALID_SOCKET);

    if (INVALID_SOCKET != udp)
      {
        ::closesocket(udp);
      }
  }


  bool start(
      NativeFlexSafetyMonitor::Configuration const& requested)
  {
    std::lock_guard<std::mutex> guard {stateMutex};

    if (running.load())
      return false;

    if (
        requested.radio_address.empty()
        || requested.first_udp_port <= 0
        || requested.last_udp_port < requested.first_udp_port
        || requested.last_udp_port > 65535)
      {
        setError("Invalid independent safety-monitor configuration.");
        return false;
      }

    configuration = requested;

    stopRequested.store(false);

    invalidateSnapshot();

    {
      std::lock_guard<std::mutex> errorGuard {errorMutex};
      error.clear();
    }

    worker =
        std::thread {
            [this]
            {
              run();
            }};

    return true;
  }


  void stop()
  {
    {
      std::lock_guard<std::mutex> guard {stateMutex};

      stopRequested.store(true);

      // DEVIATION from W7PP: portability fix, same class as
      // FlexVitaReceiver::Impl::stop(). Calling closeSockets() here
      // ran ::closesocket() on the caller thread while the worker
      // could be sitting in ::select() on the very same descriptors.
      // On Windows that closesocket() wakes a blocked select(); on
      // POSIX it does not, and the freed descriptor numbers become
      // immediately reusable, so within the <=100 ms select timeout
      // the worker could FD_ISSET()/recv() on a descriptor that now
      // belongs to something else entirely (plausibly another
      // Native FLEX socket opened by a restart). Shut the sockets
      // down from here instead -- shutdown() reliably unblocks
      // select() on both platforms without invalidating the
      // descriptor -- and leave the actual close() to the worker
      // thread itself. Both branches of the select loop in run()
      // now break out to the shared tail cleanup instead of
      // returning early, so the worker always reaches the
      // closeSockets() call sites already in fail() and at the end
      // of run() -- exactly one close() per descriptor, whether the
      // worker exits via an error or via this stop() request.
      SOCKET const tcp = tcpSocket.load();
      SOCKET const udp = udpSocket.load();

      if (INVALID_SOCKET != tcp)
        {
          ::shutdown(tcp, SD_BOTH);
        }

      if (INVALID_SOCKET != udp)
        {
          ::shutdown(udp, SD_BOTH);
        }
    }

    if (worker.joinable())
      worker.join();

    running.store(false);

    invalidateSnapshot();
  }


  void run()
  {
    running.store(true);

    meterKinds.clear();
    responses.clear();
    pendingTcp.clear();

    haveClientHandle = false;

    WSADATA winsockData {};

    bool winsockStarted = false;

    auto fail =
        [this, &winsockStarted] (
            std::string const& text)
        {
          setError(text);

          closeSockets();

          if (winsockStarted)
            {
              WSACleanup();
              winsockStarted = false;
            }

          running.store(false);

          invalidateSnapshot();
        };


    if (0 != WSAStartup(MAKEWORD(2, 2), &winsockData))
      {
        fail("Independent safety monitor WSAStartup failed.");
        return;
      }

    winsockStarted = true;


    // --------------------------------------------------------
    // OWN UDP SOCKET / OWN PORT
    // --------------------------------------------------------

    SOCKET udp =
        ::socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP);

    if (INVALID_SOCKET == udp)
      {
        fail("Independent safety UDP socket create failed.");
        return;
      }

    udpSocket.store(udp);


    int selectedUdpPort = 0;

    for (
        int port = configuration.first_udp_port;
        port <= configuration.last_udp_port;
        ++port)
      {
        sockaddr_in local {};

        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port =
            htons(
                static_cast<u_short>(port));

        if (
            0
            == ::bind(
                udp,
                reinterpret_cast<sockaddr *>(&local),
                sizeof(local)))
          {
            selectedUdpPort = port;
            break;
          }
      }


    if (0 == selectedUdpPort)
      {
        fail("Independent safety monitor found no free UDP port.");
        return;
      }


    int receiveBuffer =
        256 * 1024;

    setsockopt(
        udp,
        SOL_SOCKET,
        SO_RCVBUF,
        reinterpret_cast<char const *>(&receiveBuffer),
        sizeof(receiveBuffer));


    // --------------------------------------------------------
    // OWN TCP CONNECTION
    // --------------------------------------------------------

    SOCKET tcp =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP);

    if (INVALID_SOCKET == tcp)
      {
        fail("Independent safety TCP socket create failed.");
        return;
      }

    tcpSocket.store(tcp);

    // POSIX: a send to a socket the radio has closed would otherwise
    // raise SIGPIPE and terminate the process.
    flexSuppressSigPipe(tcp);

    sockaddr_in radio {};

    radio.sin_family = AF_INET;

    radio.sin_port =
        htons(configuration.tcp_port);

    radio.sin_addr.s_addr =
        inet_addr(
            configuration.radio_address.c_str());


    if (INADDR_NONE == radio.sin_addr.s_addr)
      {
        fail("Independent safety monitor radio address invalid.");
        return;
      }


    if (
        0
        != ::connect(
            tcp,
            reinterpret_cast<sockaddr *>(&radio),
            sizeof(radio)))
      {
        fail("Independent safety monitor TCP connect failed.");
        return;
      }


    if (
        !waitForHandle(
            tcp,
            std::chrono::milliseconds(2500)))
      {
        fail("Independent safety monitor client handle not received.");
        return;
      }


    setMonitorConnected(true);


    // --------------------------------------------------------
    // REGISTER ONLY THIS MONITOR'S PRIVATE UDP PORT
    // --------------------------------------------------------

    {
      std::ostringstream command;

      command
          << "client udpport "
          << selectedUdpPort;

      if (
          !sendCommand(
              tcp,
              1,
              command.str())
          || !waitForResponse(
              tcp,
              1,
              std::chrono::milliseconds(2000)))
        {
          fail("Independent safety UDP registration failed.");
          return;
        }
    }


    // --------------------------------------------------------
    // READ METER DEFINITIONS
    //
    // No RX or TX stream subscription is requested.
    // --------------------------------------------------------

    if (
        !sendCommand(
            tcp,
            2,
            "meter list")
        || !waitForResponse(
            tcp,
            2,
            std::chrono::milliseconds(2500)))
      {
        fail("Independent safety meter list failed.");
        return;
      }


    // --------------------------------------------------------
    // SUBSCRIBE ONLY TO METERS
    // --------------------------------------------------------

    if (
        !sendCommand(
            tcp,
            3,
            "sub meter all")
        || !waitForResponse(
            tcp,
            3,
            std::chrono::milliseconds(2500)))
      {
        fail("Independent safety meter subscription failed.");
        return;
      }


    setMeterStreamActive(true);

    // --------------------------------------------------------
    // W7PP ATU STATUS SUBSCRIPTION
    // --------------------------------------------------------

    if (
        sendCommand(
            tcp,
            4,
            "sub atu all"))
      {
        (void)waitForResponse(
            tcp,
            4,
            std::chrono::milliseconds(1000));
      }


    // --------------------------------------------------------
    // PRIVATE MONITOR LOOP
    //
    // This thread owns both sockets.
    // --------------------------------------------------------

    std::array<unsigned char, 65536> udpBuffer {};
    std::array<char, 65536> tcpBuffer {};


    unsigned atuCommandSequence = 100;

    while (!stopRequested.load())
      {
        // W7PP :
        // This worker already owns the FLEX TCP socket.
        // Consume at most one GUI command per loop.
        int const atuRequest =
            g_atuCommandRequest.exchange(
                0,
                std::memory_order_acq_rel);

        if (1 == atuRequest)
          {
            if (
                !sendCommand(
                    tcp,
                    atuCommandSequence++,
                    "atu start"))
              {
                fail("Native FLEX ATU start send failed.");
                return;
              }
          }
        else if (2 == atuRequest)
          {
            if (
                !sendCommand(
                    tcp,
                    atuCommandSequence++,
                    "atu bypass"))
              {
                fail("Native FLEX ATU bypass send failed.");
                return;
              }
          }

        fd_set readSet;

        FD_ZERO(&readSet);
        FD_SET(tcp, &readSet);
        FD_SET(udp, &readSet);

        timeval timeout {};

        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;


        int const ready =
            ::select(
                std::max(tcp, udp) + 1,
                &readSet,
                nullptr,
                nullptr,
                &timeout);


        if (ready < 0)
          {
            if (flexIsRetryableReceiveError(WSAGetLastError()))
              continue;

            if (!stopRequested.load())
              {
                fail("Independent safety monitor select failed.");
              }

            break;
          }


        if (0 == ready)
          continue;


        if (FD_ISSET(tcp, &readSet))
          {
            int const received =
                ::recv(
                    tcp,
                    tcpBuffer.data(),
                    static_cast<int>(tcpBuffer.size()),
                    0);

            if (received <= 0)
              {
                if (0 != received
                    && flexIsRetryableReceiveError(WSAGetLastError()))
                  continue;

                if (!stopRequested.load())
                  {
                    fail("Independent safety monitor TCP connection lost.");
                  }

                break;
              }

            consumeTcp(
                tcpBuffer.data(),
                received);
          }


        if (FD_ISSET(udp, &readSet))
          {
            sockaddr_in remote {};

            socklen_t remoteLength =
                sizeof(remote);

            int const received =
                ::recvfrom(
                    udp,
                    reinterpret_cast<char *>(udpBuffer.data()),
                    static_cast<int>(udpBuffer.size()),
                    0,
                    reinterpret_cast<sockaddr *>(&remote),
                    &remoteLength);

            if (received > 0)
              {
                captureMeterPacket(
                    udpBuffer.data(),
                    received);
              }
          }
      }


    // --------------------------------------------------------
    // NORMAL STOP
    // --------------------------------------------------------

    closeSockets();

    if (winsockStarted)
      {
        WSACleanup();
        winsockStarted = false;
      }

    running.store(false);

    invalidateSnapshot();
  }


  NativeFlexSafetyMonitor * owner {nullptr};

  NativeFlexSafetyMonitor::Configuration configuration {};

  mutable std::mutex stateMutex;
  mutable std::mutex snapshotMutex;
  mutable std::mutex errorMutex;

  NativeFlexSafetyMonitor::Snapshot snapshot {};

  std::string error {};

  std::thread worker {};

  std::atomic<bool> stopRequested {false};
  std::atomic<bool> running {false};

  std::atomic<SOCKET> tcpSocket {INVALID_SOCKET};
  std::atomic<SOCKET> udpSocket {INVALID_SOCKET};

  bool haveClientHandle {false};

  std::string pendingTcp {};

  std::unordered_map<
      unsigned,
      std::uint32_t> responses {};

  std::unordered_map<
      std::uint16_t,
      MeterKind> meterKinds {};
};


NativeFlexSafetyMonitor::GateSnapshot
NativeFlexSafetyMonitor::gateSnapshot() noexcept
{
  GateSnapshot result;

  result.monitor_connected =
      g_gateMonitorConnected.load(
          std::memory_order_acquire);

  result.meter_stream_active =
      g_gateMeterStreamActive.load(
          std::memory_order_acquire);

  result.interlock_seen =
      g_gateInterlockSeen.load(
          std::memory_order_acquire);

  result.tx_allowed =
      g_gateTxAllowed.load(
          std::memory_order_acquire);

  result.interlock_ready =
      g_gateInterlockReady.load(
          std::memory_order_acquire);

  result.all_safety_meters_seen =
      g_gateAllMetersSeen.load(
          std::memory_order_acquire);


  std::int64_t const nowNs =
      gateNowNs();


  std::int64_t const interlockNs =
      g_gateInterlockNs.load(
          std::memory_order_acquire);

  if (
      interlockNs >= 0
      && nowNs >= interlockNs)
    {
      result.interlock_age_ms =
          (nowNs - interlockNs)
          / 1000000;
    }


  std::int64_t const meterNs =
      g_gateMeterNs.load(
          std::memory_order_acquire);

  if (
      meterNs >= 0
      && nowNs >= meterNs)
    {
      result.meter_age_ms =
          (nowNs - meterNs)
          / 1000000;
    }


  return result;
}


bool NativeFlexSafetyMonitor::requestAtuStart() noexcept
{
  if (
      !g_gateMonitorConnected.load(std::memory_order_acquire)
      || !g_gateTxAllowed.load(std::memory_order_acquire)
      || !g_gateInterlockReady.load(std::memory_order_acquire))
    {
      return false;
    }

  g_atuCommandRequest.store(
      1,
      std::memory_order_release);

  return true;
}


bool NativeFlexSafetyMonitor::requestAtuBypass() noexcept
{
  if (
      !g_gateMonitorConnected.load(std::memory_order_acquire))
    {
      return false;
    }

  g_atuCommandRequest.store(
      2,
      std::memory_order_release);

  return true;
}


NativeFlexSafetyMonitor::DisplaySnapshot
NativeFlexSafetyMonitor::displaySnapshot() noexcept
{
  DisplaySnapshot result;

  result.monitor_connected =
      g_gateMonitorConnected.load(std::memory_order_acquire);

  result.meter_stream_active =
      g_gateMeterStreamActive.load(std::memory_order_acquire);

  result.level_seen =
      g_displayLevelSeen.load(std::memory_order_acquire);

  result.forward_power_seen =
      g_displayForwardPowerSeen.load(std::memory_order_acquire);

  result.swr_seen =
      g_displaySwrSeen.load(std::memory_order_acquire);

  result.voltage_b_seen =
      g_displayVoltageBSeen.load(std::memory_order_acquire);

  result.pa_temperature_seen =
      g_displayPaTemperatureSeen.load(std::memory_order_acquire);

  result.atu_seen =
      g_displayAtuSeen.load(std::memory_order_acquire);

  result.atu_state =
      static_cast<AtuState>(
          g_displayAtuState.load(
              std::memory_order_acquire));

  result.level_raw =
      g_displayLevelRaw.load(std::memory_order_acquire);

  result.forward_power_raw =
      g_displayForwardPowerRaw.load(std::memory_order_acquire);

  result.swr_raw =
      g_displaySwrRaw.load(std::memory_order_acquire);

  result.voltage_b_raw =
      g_displayVoltageBRaw.load(std::memory_order_acquire);

  result.pa_temperature_raw =
      g_displayPaTemperatureRaw.load(std::memory_order_acquire);

  std::int64_t const meterNs =
      g_gateMeterNs.load(std::memory_order_acquire);

  std::int64_t const nowNs =
      gateNowNs();

  if (meterNs >= 0 && nowNs >= meterNs)
    {
      result.meter_age_ms =
          (nowNs - meterNs) / 1000000;
    }

  return result;
}


NativeFlexSafetyMonitor::NativeFlexSafetyMonitor()
  : m_impl {new Impl {this}}
{
  /*
   * Construction performs no network activity.
   *
   * The monitor remains completely dormant until an explicit
   * future caller invokes start().
   */
}


NativeFlexSafetyMonitor::~NativeFlexSafetyMonitor()
{
  stop();
}


bool NativeFlexSafetyMonitor::start(
    Configuration const& configuration)
{
  return
      m_impl->start(configuration);
}


void NativeFlexSafetyMonitor::stop()
{
  m_impl->stop();
}


bool NativeFlexSafetyMonitor::running() const noexcept
{
  return
      m_impl->running.load();
}


NativeFlexSafetyMonitor::Snapshot
NativeFlexSafetyMonitor::snapshot() const
{
  std::lock_guard<std::mutex> guard {
      m_impl->snapshotMutex};

  return
      m_impl->snapshot;
}


std::string NativeFlexSafetyMonitor::lastError() const
{
  return
      m_impl->getError();
}


void NativeFlexSafetyMonitor::invalidate()
{
  m_impl->invalidateSnapshot();
}
