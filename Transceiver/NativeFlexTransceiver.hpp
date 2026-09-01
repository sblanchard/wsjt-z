#ifndef W7PP_NATIVE_FLEX_TRANSCEIVER_HPP
#define W7PP_NATIVE_FLEX_TRANSCEIVER_HPP

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include "TransceiverBase.hpp"
#include "NativeFlexSafetyMonitor.hpp"

class QTcpSocket;

/*
 * W7PP Native Flex transceiver backend.
 *
 * Native radio control exists only when the user selects:
 *
 *   Flex Native VITA-49
 *
 * Normal WSJT-X radio backends remain independent.
 *
 * The Native VITA RX implementation is not owned
 * or modified by this class.
 */
class NativeFlexTransceiver final
  : public TransceiverBase
{
public:
  explicit NativeFlexTransceiver(
      logger_type * logger,
      QObject * parent = nullptr);

  ~NativeFlexTransceiver() override = default;

  // W7PP :
  // FLEX-NATIVE-ONLY startup dial frequency.
  // Zero means "do not issue a startup tune command."
  static void set_startup_frequency(Frequency);
  static void set_dax_channel(int);

private:
  int do_start() override;
  void do_stop() override;

  void do_frequency(
      Frequency,
      MODE,
      bool no_ignore) override;

  void do_tx_frequency(
      Frequency,
      MODE,
      bool no_ignore) override;

  void do_mode(MODE) override;
  void do_ptt(bool) override;
  void do_tx_rf_power_level(int) override;
  void do_slice_af_gain(int) override;
  void do_dax_gain(int, bool) override;

  void capture_owned_slice(QByteArray const& line);
  void capture_dax_tx_stream(QByteArray const& line);
  void capture_transmit_status(QByteArray const& line);
  void capture_gain_status(QByteArray const& line);
  void wait_for_dax_tx_stream();
  void wait_for_owned_slice();
  QByteArray send_command(QString const& command);

  static Frequency startup_frequency_;
  static int dax_channel_;

  /*
   * W7PP 
   *
   * Independent FLEX safety telemetry transport.
   * It does not participate in RX or TX VITA/audio.
   */
  NativeFlexSafetyMonitor safety_monitor_;
  QTcpSocket * control_socket_ {nullptr};

  QByteArray pending_control_ {};

  QString api_version_ {};
  QString gui_client_id_ {};

  quint32 client_handle_ {0};
  bool have_client_handle_ {false};

  quint32 next_sequence_ {2};
  int slice_id_ {-1};
  quint32 dax_tx_stream_id_ {0};
};

#endif
