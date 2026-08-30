#ifndef W7PP_NATIVE_FLEX_SAFETY_MONITOR_HPP
#define W7PP_NATIVE_FLEX_SAFETY_MONITOR_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

/*
 * W7PP Native FLEX independent safety monitor.
 *
 *  architecture:
 *
 *   - completely separate FLEX TCP connection
 *   - completely separate UDP socket and registered UDP port
 *   - completely separate worker thread
 *   - dynamically discovers safety meter IDs by meter NAME
 *   - stores RAW meter values only
 *   - captures unsolicited FLEX interlock status
 *   - fails closed whenever its independent connection stops
 *
 * ABSOLUTE ISOLATION RULE:
 *
 * No RX audio packet and no TX audio/VITA packet is ever routed
 * through this class.
 *
 * This class does not include, call, own or modify:
 *
 *   FlexVitaReceiver
 *   NativeFlexTransceiver
 *   Modulator
 *   SoundOutput
 *   MainWindow
 *
 *  only implements the class.
 *
 * Nothing in WSJT creates or starts an instance yet.
 */
class NativeFlexSafetyMonitor final
{
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  struct Configuration
  {
    std::string radio_address {};

    std::uint16_t tcp_port {4992};

    /*
     * Deliberately separate from the proven Native FLEX RX
     * receiver default range of 4995..5010.
     */
    int first_udp_port {5011};
    int last_udp_port {5026};
  };

  struct Snapshot
  {
    /*
     * valid means the independent monitor has a live connection
     * and has received FLEX interlock status.
     *
     * A future TX gate must additionally check tx_allowed,
     * freshness and whatever numeric meter policy is eventually
     * accepted.
     */
    bool valid {false};

    bool monitor_connected {false};
    bool meter_stream_active {false};

    bool interlock_seen {false};
    bool tx_allowed {false};

    std::string interlock_state {};
    std::string interlock_reason {};
    std::string interlock_source {};

    TimePoint interlock_updated {};
    TimePoint meter_updated {};

    /*
     * Raw signed 16-bit meter values.
     *
     * Meter IDs are NOT hard-coded here.  The worker dynamically
     * associates IDs with these FLEX meter names:
     *
     *   +13.8A
     *   +13.8B
     *   FWDPWR
     *   REFPWR
     *   SWR
     *   PATEMP
     *
     * This avoids assuming one model's numerical meter IDs apply
     * to every FLEX radio.
     */
    bool voltage_a_seen {false};
    bool voltage_b_seen {false};
    bool forward_power_seen {false};
    bool reflected_power_seen {false};
    bool swr_seen {false};
    bool pa_temperature_seen {false};

    std::int16_t voltage_a_raw {0};
    std::int16_t voltage_b_raw {0};
    std::int16_t forward_power_raw {0};
    std::int16_t reflected_power_raw {0};
    std::int16_t swr_raw {0};
    std::int16_t pa_temperature_raw {0};
  };

  /*
   * :
   *
   * Compact process-wide TX gate state.
   *
   * This intentionally contains no strings, no raw meter arrays
   * and no network objects.  A future Native FLEX PTT gate can
   * read it without touching the monitor worker or taking the
   * full Snapshot mutex.
   *
   * Age values are -1 when that telemetry has never been seen.
   */
  struct GateSnapshot
  {
    bool monitor_connected {false};
    bool meter_stream_active {false};

    bool interlock_seen {false};
    bool tx_allowed {false};
    bool interlock_ready {false};

    bool all_safety_meters_seen {false};

    std::int64_t interlock_age_ms {-1};
    std::int64_t meter_age_ms {-1};
  };

  /*
   * W7PP display-only telemetry cache.
   *
   * Meter IDs remain dynamically discovered by FLEX meter NAME.
   * This data is read-only from the GUI side and is completely
   * separate from Native FLEX RX audio, TX audio and PTT.
   *
   * GUI mapping:
   *
   *   LEVEL = GAIN
   *   PWR   = FWDPWR
   *   SWR   = SWR
   *   VOLTS = +13.8B
   *   TEMP  = PATEMP
   */
  enum class AtuState
  {
    unknown,
    bypassed,
    in_progress,
    successful
  };

  struct DisplaySnapshot
  {
    bool monitor_connected {false};
    bool meter_stream_active {false};

    bool atu_seen {false};
    AtuState atu_state {AtuState::unknown};

    bool level_seen {false};
    bool forward_power_seen {false};
    bool swr_seen {false};
    bool voltage_b_seen {false};
    bool pa_temperature_seen {false};

    std::int16_t level_raw {0};
    std::int16_t forward_power_raw {0};
    std::int16_t swr_raw {0};
    std::int16_t voltage_b_raw {0};
    std::int16_t pa_temperature_raw {0};

    std::int64_t meter_age_ms {-1};
  };

  /*
   * Atomic-load-only process-wide gate read.
   *
   * No socket I/O.
   * No waiting.
   * No mutex acquisition.
   * No radio command.
   */
  static GateSnapshot gateSnapshot() noexcept;

  static DisplaySnapshot displaySnapshot() noexcept;

  // W7PP :
  // Queue only the two supported FLEX ATU commands.
  static bool requestAtuStart() noexcept;
  static bool requestAtuBypass() noexcept;

  NativeFlexSafetyMonitor();
  ~NativeFlexSafetyMonitor();

  NativeFlexSafetyMonitor(
      NativeFlexSafetyMonitor const&) = delete;

  NativeFlexSafetyMonitor& operator=(
      NativeFlexSafetyMonitor const&) = delete;

  /*
   * These methods operate only the monitor's own private
   * TCP/UDP worker.
   *
   *  DOES NOT CALL start() from anywhere.
   */
  bool start(Configuration const& configuration);
  void stop();

  bool running() const noexcept;

  Snapshot snapshot() const;

  /*
   * Last private-monitor error text only.
   */
  std::string lastError() const;

  /*
   * Clear cached safety state to fail-closed.
   *
   * No radio command is issued.
   */
  void invalidate();

private:
  struct Impl;

  std::unique_ptr<Impl> m_impl;
};

#endif
