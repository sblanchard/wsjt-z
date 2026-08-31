#include "FlexVitaReceiver.hpp"

// W7PP : portability shim, see Transceiver/FlexSocketCompat.hpp
#include "FlexSocketCompat.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace
{
  std::uint32_t readBigEndian32(unsigned char const * p)
  {
    return
        (static_cast<std::uint32_t>(p[0]) << 24)
      | (static_cast<std::uint32_t>(p[1]) << 16)
      | (static_cast<std::uint32_t>(p[2]) << 8)
      |  static_cast<std::uint32_t>(p[3]);
  }

  float readBigEndianFloat(unsigned char const * p)
  {
    std::uint32_t const bits = readBigEndian32(p);

    float value {0.0f};

    static_assert(sizeof(value) == sizeof(bits), "32-bit float required");

    std::memcpy(&value, &bits, sizeof(value));

    return value;
  }

  std::string extractValue(std::string const& line, std::string const& key)
  {
    auto pos = line.find(key);

    if (std::string::npos == pos)
      return {};

    pos += key.size();

    if (pos >= line.size())
      return {};

    if ('"' == line[pos])
      {
        ++pos;

        auto const end = line.find('"', pos);

        if (std::string::npos == end)
          return {};

        return line.substr(pos, end - pos);
      }

    auto end = line.find_first_of(" \r\n|", pos);

    if (std::string::npos == end)
      end = line.size();

    return line.substr(pos, end - pos);
  }

  std::uint32_t parseNumericId(std::string const& text)
  {
    if (text.empty())
      return 0;

    try
      {
        return static_cast<std::uint32_t>(
            std::stoul(text, nullptr, 0));
      }
    catch (...)
      {
        return 0;
      }
  }

  class Decimator48To12 final
  {
  public:
    Decimator48To12()
    {
      design();

      m_history.fill(0.0f);
    }

    void reset()
    {
      m_history.fill(0.0f);
      m_position = 0;
      m_phase = 0;
    }

    bool process(float input, float& output)
    {
      m_history[static_cast<std::size_t>(m_position)] = input;

      m_position = (m_position + 1) % FlexVitaReceiver::FirTapCount;

      ++m_phase;

      if (m_phase < FlexVitaReceiver::SampleRateRatio)
        return false;

      m_phase = 0;

      double sum = 0.0;

      int index = m_position - 1;

      if (index < 0)
        index += FlexVitaReceiver::FirTapCount;

      for (int i = 0; i < FlexVitaReceiver::FirTapCount; ++i)
        {
          sum +=
              static_cast<double>(
                  m_coefficients[static_cast<std::size_t>(i)])
            * static_cast<double>(
                  m_history[static_cast<std::size_t>(index)]);

          --index;

          if (index < 0)
            index += FlexVitaReceiver::FirTapCount;
        }

      output = static_cast<float>(sum);

      return true;
    }

  private:
    void design()
    {
      constexpr double pi = 3.14159265358979323846;
      constexpr double sampleRate = 48000.0;
      constexpr double cutoff = 5000.0;

      constexpr int middle =
          (FlexVitaReceiver::FirTapCount - 1) / 2;

      double coefficientSum = 0.0;

      for (int n = 0; n < FlexVitaReceiver::FirTapCount; ++n)
        {
          int const k = n - middle;

          double sinc = 0.0;

          if (0 == k)
            {
              sinc = 2.0 * cutoff / sampleRate;
            }
          else
            {
              sinc =
                  std::sin(
                      2.0
                      * pi
                      * cutoff
                      * static_cast<double>(k)
                      / sampleRate)
                  / (pi * static_cast<double>(k));
            }

          double const window =
              0.42
              - 0.5
                * std::cos(
                    2.0
                    * pi
                    * static_cast<double>(n)
                    / static_cast<double>(
                        FlexVitaReceiver::FirTapCount - 1))
              + 0.08
                * std::cos(
                    4.0
                    * pi
                    * static_cast<double>(n)
                    / static_cast<double>(
                        FlexVitaReceiver::FirTapCount - 1));

          double const value = sinc * window;

          m_coefficients[static_cast<std::size_t>(n)] =
              static_cast<float>(value);

          coefficientSum += value;
        }

      for (auto& coefficient : m_coefficients)
        {
          coefficient =
              static_cast<float>(
                  static_cast<double>(coefficient)
                  / coefficientSum);
        }
    }

    std::array<float, FlexVitaReceiver::FirTapCount> m_coefficients {};
    std::array<float, FlexVitaReceiver::FirTapCount> m_history {};

    int m_position {0};
    int m_phase {0};
  };

  std::int16_t floatToInt16(float value)
  {
    double scaled =
        static_cast<double>(value) * 32767.0;

    scaled =
        std::max(
            -32768.0,
            std::min(32767.0, scaled));

    return static_cast<std::int16_t>(
        std::lrint(scaled));
  }

  struct VitaMeta
  {
    bool valid {false};

    unsigned packetType {0};
    bool classIdPresent {false};
    bool trailerPresent {false};

    unsigned tsi {0};
    unsigned tsf {0};
    unsigned packetCount {0};

    std::uint16_t packetSizeWords {0};

    std::size_t payloadOffset {0};
    std::size_t payloadBytes {0};
  };

  bool parseVita(
      unsigned char const * data,
      int received,
      VitaMeta& meta)
  {
    meta = VitaMeta {};

    if (received < 8)
      return false;

    std::uint32_t const header =
        readBigEndian32(data);

    meta.packetType =
        (header >> 28) & 0x0F;

    meta.classIdPresent =
        0 != (header & 0x08000000u);

    meta.trailerPresent =
        0 != (header & 0x04000000u);

    meta.tsi =
        (header >> 22) & 0x03;

    meta.tsf =
        (header >> 20) & 0x03;

    meta.packetCount =
        (header >> 16) & 0x0F;

    meta.packetSizeWords =
        static_cast<std::uint16_t>(
            header & 0xFFFFu);

    std::size_t const packetBytes =
        static_cast<std::size_t>(
            meta.packetSizeWords) * 4u;

    if (0 == packetBytes
        || packetBytes > static_cast<std::size_t>(received))
      {
        return false;
      }

    //
    // Flex narrow DAX RX was proven as packet type 3:
    // IF Data With Stream + class ID.
    //
    if (1 != meta.packetType
        && 3 != meta.packetType)
      {
        return false;
      }

    std::size_t index = 4;

    //
    // Stream ID.
    //
    index += 4;

    if (meta.classIdPresent)
      {
        if (index + 8 > packetBytes)
          return false;

        index += 8;
      }

    if (0 != meta.tsi)
      {
        if (index + 4 > packetBytes)
          return false;

        index += 4;
      }

    if (0 != meta.tsf)
      {
        if (index + 8 > packetBytes)
          return false;

        index += 8;
      }

    std::size_t const trailerBytes =
        meta.trailerPresent ? 4u : 0u;

    if (packetBytes < index + trailerBytes)
      return false;

    meta.payloadOffset = index;

    meta.payloadBytes =
        packetBytes
        - index
        - trailerBytes;

    if (0 == meta.payloadBytes
        || 0 != (meta.payloadBytes % sizeof(float)))
      {
        return false;
      }

    meta.valid = true;

    return true;
  }
}

struct FlexVitaReceiver::Impl
{
  explicit Impl(FlexVitaReceiver * ownerIn)
    : owner {ownerIn}
  {
  }

  ~Impl()
  {
    stop();
  }

  bool start(Configuration const& requested)
  {
    std::lock_guard<std::mutex> guard {stateMutex};

    if (running.load())
      return false;

    configuration = requested;

    stopRequested.store(false);
    streaming.store(false);

    clearStatistics();

    {
      std::lock_guard<std::mutex> errorGuard {errorMutex};
      error.clear();
    }

    worker =
        std::thread(
            [this]
            {
              run();
            });

    return true;
  }

  void stop()
  {
    stopRequested.store(true);

    //
    // Break blocking socket calls immediately.
    //
    SOCKET const localTcp = tcpSocket.load();
    SOCKET const localUdp = udpSocket.load();

    if (INVALID_SOCKET != localTcp)
      {
        shutdown(localTcp, SD_BOTH);
      }

    if (INVALID_SOCKET != localUdp)
      {
        shutdown(localUdp, SD_BOTH);
      }

    if (worker.joinable())
      {
        worker.join();
      }

    running.store(false);
    streaming.store(false);
  }

  void reset()
  {
    stop();

    clearStatistics();

    std::lock_guard<std::mutex> errorGuard {errorMutex};
    error.clear();
  }

  Statistics statistics() const
  {
    Statistics result;

    result.vitaPackets = vitaPackets.load();
    result.audioFloats = audioFloats.load();
    result.decoderSamples = decoderSamples.load();
    result.sequenceErrors = sequenceErrors.load();
    result.malformedPackets = malformedPackets.load();
    result.invalidFloats = invalidFloats.load();

    return result;
  }

  void clearStatistics()
  {
    vitaPackets.store(0);
    audioFloats.store(0);
    decoderSamples.store(0);
    sequenceErrors.store(0);
    malformedPackets.store(0);
    invalidFloats.store(0);
  }

  void setError(std::string const& text)
  {
    std::lock_guard<std::mutex> guard {errorMutex};
    error = text;
  }

  std::string lastError() const
  {
    std::lock_guard<std::mutex> guard {errorMutex};
    return error;
  }

  bool sendAll(SOCKET socket, std::string const& text)
  {
    char const * current = text.data();

    int remaining =
        static_cast<int>(text.size());

    while (remaining > 0)
      {
        int const sent =
            static_cast<int>(
                ::send(
                    socket,
                    current,
                    remaining,
                    flexSendFlags()));

        if (SOCKET_ERROR == sent || 0 == sent)
          return false;

        current += sent;
        remaining -= sent;
      }

    return true;
  }

  bool sendCommand(
      SOCKET socket,
      int sequence,
      std::string const& command)
  {
    std::ostringstream message;

    message
        << 'C'
        << sequence
        << '|'
        << command
        << '\n';

    return sendAll(
        socket,
        message.str());
  }

  //
  // Route a slice into our DAX channel so the operator does not
  // have to do it by hand in the SmartSDR DAX panel.
  //
  // Deliberately conservative: if any in-use slice already feeds
  // the channel - because the Native FLEX CAT backend bound its
  // own slice, or the operator set it in SmartSDR - nothing is
  // sent and the operator's routing is left alone.
  //
  void ensureDaxRouting(SOCKET tcp)
  {
    if (daxRouteRequested.load()
        || daxChannelFed.load())
      return;

    int const slice = firstInUseSlice.load();

    if (slice < 0)
      return;

    std::ostringstream command;

    command
        << "slice s "
        << slice
        << " dax="
        << configuration.daxChannel;

    daxRouteRequested.store(true);

    sendCommand(
        tcp,
        nextCommandSequence++,
        command.str());
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
        auto const pos =
            pendingTcp.find('\n');

        if (std::string::npos == pos)
          break;

        std::string line =
            pendingTcp.substr(0, pos);

        pendingTcp.erase(0, pos + 1);

        if (!line.empty() && '\r' == line.back())
          line.pop_back();

        processTcpLine(line);
      }
  }

  void processTcpLine(std::string const& line)
  {
    if (line.empty())
      return;

    //
    // FLEX identifies THIS TCP API connection with:
    //
    //   Hxxxxxxxx
    //
    // Save our own client handle.  No SmartSDR GUI client
    // or station name is involved.
    //
    if (0 == apiClientHandle
        && line.size() > 1
        && 'H' == line.front())
      {
        apiClientHandle =
            parseNumericId(
                "0x" + line.substr(1));
      }

    //
    // Slice status, e.g.
    //
    //   S1A2B3C4|slice 0 in_use=1 mode=DIGU dax=2 ...
    //
    // Track the lowest in-use slice and whether anything is
    // already routed to our DAX channel.  Without a slice routed
    // to it the radio still creates the dax_rx stream and still
    // sends VITA-49 packets - they just carry silence, which
    // presents as a connected radio with a dead RX level meter.
    //
    {
      auto const slicePos = line.find("|slice ");

      if (std::string::npos != slicePos)
        {
          auto const start = slicePos + 7;

          auto end = line.find(' ', start);

          if (std::string::npos == end)
            end = line.size();

          int sliceIndex = -1;

          try
            {
              sliceIndex =
                  std::stoi(
                      line.substr(
                          start,
                          end - start));
            }
          catch (...)
            {
              sliceIndex = -1;
            }

          if (sliceIndex < 0)
            return;

          bool const gone =
              std::string::npos != line.find(" removed")
              || "0" == extractValue(line, " in_use=");

          if (gone)
            {
              if (firstInUseSlice.load() == sliceIndex)
                firstInUseSlice.store(-1);

              return;
            }

          //
          // A slice status line only carries the fields that
          // changed, so absence of " dax=" says nothing.
          //
          auto const daxText =
              extractValue(line, " dax=");

          if (!daxText.empty())
            {
              bool const feedsUs =
                  daxText ==
                  std::to_string(configuration.daxChannel);

              if (feedsUs)
                daxChannelFed.store(true);
              else if (firstInUseSlice.load() == sliceIndex
                       && daxRouteRequested.load())
                daxChannelFed.store(false);
            }

          int expected = firstInUseSlice.load();

          while (expected < 0 || sliceIndex < expected)
            {
              if (firstInUseSlice.compare_exchange_weak(
                      expected,
                      sliceIndex))
                break;
            }

          return;
        }
    }

    //
    // There may already be another DAX RX stream on the same
    // channel.  Accept only the stream belonging to THIS API
    // client handle.
    //
    if (std::string::npos != line.find("|stream ")
        && std::string::npos != line.find("type=dax_rx")
        && std::string::npos != line.find(
            "dax_channel="
            + std::to_string(configuration.daxChannel)))
      {
        auto const streamClientHandle =
            parseNumericId(
                extractValue(
                    line,
                    "client_handle="));

        if (0 == apiClientHandle
            || streamClientHandle != apiClientHandle)
          {
            return;
          }

        auto start =
            line.find("|stream ");

        start += 8;

        auto end =
            line.find(' ', start);

        if (std::string::npos == end)
          end = line.size();

        std::string const idText =
            line.substr(
                start,
                end - start);

        auto const id =
            parseNumericId(idText);

        if (0 != id)
          {
            daxStreamId.store(id);
            streaming.store(true);
          }
      }
  }

  bool listenTcpFor(
      SOCKET socket,
      std::chrono::milliseconds duration)
  {
    std::array<char, 16384> buffer {};

    auto const end =
        std::chrono::steady_clock::now()
        + duration;

    while (!stopRequested.load()
           && std::chrono::steady_clock::now() < end)
      {
        int const received =
            ::recv(
                socket,
                buffer.data(),
                static_cast<int>(buffer.size()),
                0);

        if (received > 0)
          {
            consumeTcp(
                buffer.data(),
                received);

            continue;
          }

        if (0 == received)
          return false;

        int const socketError =
            WSAGetLastError();

        if (flexIsRetryableReceiveError(socketError))
          {
            continue;
          }

        if (stopRequested.load())
          return false;

        return false;
      }

    return !stopRequested.load();
  }

  void deliverDecoderSample(std::int16_t sample)
  {
    outputBlock.push_back(sample);

    //
    // 1200 samples = 100 ms at 12 kHz.
    // This keeps GUI-thread integration lightweight while
    // preserving the continuous decoder sample stream.
    //
    if (outputBlock.size() < 1200)
      return;

    AudioCallback callbackCopy;

    {
      std::lock_guard<std::mutex> guard {callbackMutex};
      callbackCopy = callback;
    }

    if (callbackCopy)
      callbackCopy(outputBlock);

    decoderSamples.fetch_add(
        static_cast<std::uint64_t>(
            outputBlock.size()));

    outputBlock.clear();
  }

  void handleVitaPacket(
      unsigned char const * data,
      int received)
  {
    if (received < 8)
      {
        malformedPackets.fetch_add(1);
        return;
      }

    std::uint32_t const streamId =
        readBigEndian32(data + 4);

    if (streamId != daxStreamId.load())
      return;

    VitaMeta meta;

    if (!parseVita(data, received, meta))
      {
        malformedPackets.fetch_add(1);
        return;
      }

    vitaPackets.fetch_add(1);

    if (haveSequence)
      {
        unsigned const expected =
            (lastPacketCount + 1u) & 0x0Fu;

        if (meta.packetCount != expected)
          sequenceErrors.fetch_add(1);
      }

    lastPacketCount = meta.packetCount;
    haveSequence = true;

    auto const floatCount =
        meta.payloadBytes
        / sizeof(float);

    unsigned char const * payload =
        data
        + meta.payloadOffset;

    for (std::size_t i = 0; i < floatCount; ++i)
      {
        float const sample =
            readBigEndianFloat(
                payload
                + i * sizeof(float));

        if (!std::isfinite(sample))
          {
            invalidFloats.fetch_add(1);
            continue;
          }

        audioFloats.fetch_add(1);

        float filtered = 0.0f;

        if (decimator.process(sample, filtered))
          {
            deliverDecoderSample(
                floatToInt16(filtered));
          }
      }
  }

  void closeSockets()
  {
    SOCKET const tcp = tcpSocket.exchange(INVALID_SOCKET);

    if (INVALID_SOCKET != tcp)
      {
        shutdown(tcp, SD_BOTH);
        closesocket(tcp);
      }

    SOCKET const udp = udpSocket.exchange(INVALID_SOCKET);

    if (INVALID_SOCKET != udp)
      {
        closesocket(udp);
      }
  }

  void run()
  {
    running.store(true);
    streaming.store(false);

    apiClientHandle = 0;
    pendingTcp.clear();

    daxStreamId.store(0);

    firstInUseSlice.store(-1);
    daxChannelFed.store(false);
    daxRouteRequested.store(false);
    nextCommandSequence = 8;

    haveSequence = false;
    lastPacketCount = 0;

    outputBlock.clear();
    outputBlock.reserve(1200);

    decimator.reset();

    WSADATA winsockData {};

    bool winsockStarted = false;

    auto fail =
        [this, &winsockStarted] (std::string const& text)
        {
          setError(text);

          closeSockets();

          if (winsockStarted)
            {
              WSACleanup();
              winsockStarted = false;
            }

          streaming.store(false);
          running.store(false);
        };

    if (0 != WSAStartup(MAKEWORD(2, 2), &winsockData))
      {
        fail("WSAStartup failed");
        return;
      }

    winsockStarted = true;

    SOCKET udp =
        ::socket(
            AF_INET,
            SOCK_DGRAM,
            IPPROTO_UDP);

    if (INVALID_SOCKET == udp)
      {
        fail("UDP socket create failed");
        return;
      }

    udpSocket.store(udp);

    int selectedUdpPort = 0;

    for (int port = configuration.firstUdpPort;
         port <= configuration.lastUdpPort;
         ++port)
      {
        sockaddr_in local {};

        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port =
            htons(
                static_cast<unsigned short>(port));

        if (0 == ::bind(
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
        fail("No available UDP VITA port");
        return;
      }

    flexSetReceiveTimeout(udp, 200);

    // Linux doubles the request and clamps against net.core.rmem_max, so
    // the value actually granted can be smaller than requested. That is
    // not treated as fatal: an undersized buffer only means VITA packets
    // are more likely to be dropped under load, not that reception fails
    // outright, so RX startup proceeds regardless of what was granted.
    flexSetReceiveBuffer(udp, 1024 * 1024);

    SOCKET tcp =
        ::socket(
            AF_INET,
            SOCK_STREAM,
            IPPROTO_TCP);

    if (INVALID_SOCKET == tcp)
      {
        fail("TCP socket create failed");
        return;
      }

    tcpSocket.store(tcp);

    flexSetReceiveTimeout(tcp, 250);

    // POSIX: a send to a socket the radio has closed would otherwise
    // raise SIGPIPE and terminate the process.
    flexSuppressSigPipe(tcp);

    sockaddr_in radio {};

    radio.sin_family = AF_INET;
    radio.sin_port =
        htons(configuration.tcpPort);

    radio.sin_addr.s_addr =
        inet_addr(
            configuration.radioAddress.c_str());

    if (INADDR_NONE == radio.sin_addr.s_addr)
      {
        fail("Invalid Flex radio IPv4 address");
        return;
      }

    if (SOCKET_ERROR ==
        ::connect(
            tcp,
            reinterpret_cast<sockaddr *>(&radio),
            sizeof(radio)))
      {
        fail("Flex TCP connect failed");
        return;
      }

    //
    // Wait for the FLEX Hxxxxxxxx message for THIS TCP
    // connection.  That handle identifies ownership of the
    // DAX RX stream we create below.
    //
    listenTcpFor(
        tcp,
        std::chrono::milliseconds(1000));

    if (stopRequested.load())
      {
        fail("Stopped");
        return;
      }

    if (0 == apiClientHandle)
      {
        fail("Flex API client handle not received");
        return;
      }

    if (!sendCommand(tcp, 3, "sub slice all")
        || !sendCommand(tcp, 4, "sub audio_stream all")
        || !sendCommand(tcp, 5, "sub dax all"))
      {
        fail("Flex RX subscription failed");
        return;
      }

    {
      std::ostringstream command;

      command
          << "client udpport "
          << selectedUdpPort;

      if (!sendCommand(tcp, 6, command.str()))
        {
          fail("SmartSDR UDP port registration failed");
          return;
        }
    }

    listenTcpFor(
        tcp,
        std::chrono::milliseconds(1500));

    {
      std::ostringstream command;

      command
          << "stream create type=dax_rx dax_channel="
          << configuration.daxChannel;

      if (!sendCommand(tcp, 7, command.str()))
        {
          fail("DAX RX stream request failed");
          return;
        }
    }

    auto const streamDeadline =
        std::chrono::steady_clock::now()
        + std::chrono::seconds(5);

    while (!stopRequested.load()
           && 0 == daxStreamId.load()
           && std::chrono::steady_clock::now() < streamDeadline)
      {
        listenTcpFor(
            tcp,
            std::chrono::milliseconds(250));
      }

    if (0 == daxStreamId.load())
      {
        fail("DAX RX stream ID not received");
        return;
      }

    streaming.store(true);

    //
    // The slice may not exist yet - the Native FLEX CAT backend
    // creates its own after the receiver starts - so this is
    // retried from the RX loop until a slice appears.
    //
    ensureDaxRouting(tcp);

    std::array<unsigned char, 65536> packet {};

    while (!stopRequested.load())
      {
        sockaddr_in remote {};
        socklen_t remoteLength = sizeof(remote);

        int const received =
            ::recvfrom(
                udp,
                reinterpret_cast<char *>(packet.data()),
                static_cast<int>(packet.size()),
                0,
                reinterpret_cast<sockaddr *>(&remote),
                &remoteLength);

        if (received > 0)
          {
            handleVitaPacket(
                packet.data(),
                received);

            continue;
          }

        int const socketError =
            WSAGetLastError();

        if (flexIsRetryableReceiveError(socketError))
          {
            //
            // Keep TCP status/replies drained while RX runs.
            //
            listenTcpFor(
                tcp,
                std::chrono::milliseconds(1));

            ensureDaxRouting(tcp);

            continue;
          }

        if (!stopRequested.load())
          setError("VITA UDP receive failed");

        break;
      }

    //
    // Do not emit a partial final block.
    // MainWindow must only consume complete continuous blocks.
    //
    outputBlock.clear();

    closeSockets();

    if (winsockStarted)
      {
        WSACleanup();
        winsockStarted = false;
      }

    streaming.store(false);
    running.store(false);
  }

  FlexVitaReceiver * owner {nullptr};

  Configuration configuration {};

  mutable std::mutex stateMutex;
  mutable std::mutex callbackMutex;
  mutable std::mutex errorMutex;

  AudioCallback callback {};
  std::string error {};

  std::thread worker {};

  std::atomic<bool> stopRequested {false};
  std::atomic<bool> running {false};
  std::atomic<bool> streaming {false};

  std::atomic<SOCKET> tcpSocket {INVALID_SOCKET};
  std::atomic<SOCKET> udpSocket {INVALID_SOCKET};

  std::uint32_t apiClientHandle {0};
  std::string pendingTcp {};

  std::atomic<std::uint32_t> daxStreamId {0};

  //
  // Slice -> DAX routing state, learned from "sub slice all".
  //
  // firstInUseSlice   lowest in-use slice the radio has reported
  // daxChannelFed     some in-use slice already routes to our channel
  // daxRouteRequested we have asked the radio to route one
  //
  std::atomic<int> firstInUseSlice {-1};
  std::atomic<bool> daxChannelFed {false};
  std::atomic<bool> daxRouteRequested {false};

  int nextCommandSequence {8};

  Decimator48To12 decimator {};

  bool haveSequence {false};
  unsigned lastPacketCount {0};

  DecoderBlock outputBlock {};

  std::atomic<std::uint64_t> vitaPackets {0};
  std::atomic<std::uint64_t> audioFloats {0};
  std::atomic<std::uint64_t> decoderSamples {0};
  std::atomic<std::uint64_t> sequenceErrors {0};
  std::atomic<std::uint64_t> malformedPackets {0};
  std::atomic<std::uint64_t> invalidFloats {0};
};

FlexVitaReceiver::FlexVitaReceiver()
  : m_impl {new Impl {this}}
{
}

FlexVitaReceiver::~FlexVitaReceiver()
{
  stop();
}

void FlexVitaReceiver::setAudioCallback(AudioCallback callback)
{
  std::lock_guard<std::mutex> guard {m_impl->callbackMutex};

  m_impl->callback =
      std::move(callback);
}

bool FlexVitaReceiver::start(Configuration const& configuration)
{
  return m_impl->start(configuration);
}

void FlexVitaReceiver::stop()
{
  m_impl->stop();
}

void FlexVitaReceiver::reset()
{
  m_impl->reset();
}

bool FlexVitaReceiver::running() const noexcept
{
  return m_impl->running.load();
}

bool FlexVitaReceiver::streaming() const noexcept
{
  return m_impl->streaming.load();
}

FlexVitaReceiver::Statistics
FlexVitaReceiver::statistics() const noexcept
{
  return m_impl->statistics();
}

std::string FlexVitaReceiver::lastError() const
{
  return m_impl->lastError();
}