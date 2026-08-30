#ifndef W7PP_FLEX_VITA_RECEIVER_HPP
#define W7PP_FLEX_VITA_RECEIVER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

/*
 * W7PP native Flex receive engine.
 *
 * This is an ADDITIONAL receive source.
 *
 * Existing WSJT-X receive choices remain separate:
 *
 *   Standard audio:
 *     SoundInput -> Detector -> dec_data.d2
 *
 *   TCI:
 *     TCI network audio -> dec_data.d2
 *
 *   W7PP native Flex:
 *     SmartSDR TCP + DAX/VITA UDP
 *       -> 48 kHz float
 *       -> anti-alias FIR
 *       -> 12 kHz signed int16
 *       -> MainWindow integration (later step)
 *
 * This class does not transmit.
 * It does not use FlexLib, .NET, or a Windows DAX audio device.
 */

class FlexVitaReceiver final
{
public:
  static constexpr int RadioAudioSampleRate = 48000;
  static constexpr int DecoderAudioSampleRate = 12000;
  static constexpr int SampleRateRatio = 4;
  static constexpr int FirTapCount = 127;

  using DecoderBlock = std::vector<std::int16_t>;
  using AudioCallback = std::function<void(DecoderBlock const&)>;

  struct Configuration
  {
    std::string radioAddress {"192.168.0.246"};
    unsigned short tcpPort {4992};
    int daxChannel {1};

    int firstUdpPort {4995};
    int lastUdpPort {5010};
  };

  struct Statistics
  {
    std::uint64_t vitaPackets {0};
    std::uint64_t audioFloats {0};
    std::uint64_t decoderSamples {0};
    std::uint64_t sequenceErrors {0};
    std::uint64_t malformedPackets {0};
    std::uint64_t invalidFloats {0};
  };

  FlexVitaReceiver();
  ~FlexVitaReceiver();

  FlexVitaReceiver(FlexVitaReceiver const&) = delete;
  FlexVitaReceiver& operator=(FlexVitaReceiver const&) = delete;

  void setAudioCallback(AudioCallback callback);

  bool start(Configuration const& configuration);
  void stop();
  void reset();

  bool running() const noexcept;
  bool streaming() const noexcept;

  Statistics statistics() const noexcept;

  std::string lastError() const;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

#endif