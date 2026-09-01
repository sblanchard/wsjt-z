#include "NativeFlexTransceiver.hpp"
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>
#include <QVariant>
#include <QDebug>

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QTcpSocket>

#include <stdexcept>

#include "NativeFlexRadioSelection.hpp"

NativeFlexTransceiver::Frequency NativeFlexTransceiver::startup_frequency_ {0};
int NativeFlexTransceiver::dax_channel_ {1};

void NativeFlexTransceiver::set_startup_frequency(
    Frequency frequency)
{
  startup_frequency_ = frequency;
}

void NativeFlexTransceiver::set_dax_channel(
    int channel)
{
  if (channel < 1 || channel > 8)
    {
      channel = 1;
    }

  dax_channel_ = channel;
}

NativeFlexTransceiver::NativeFlexTransceiver(
    logger_type * logger,
    QObject * parent)
  : TransceiverBase {logger, parent}
  , control_socket_ {new QTcpSocket {this}}
{
}

void NativeFlexTransceiver::do_tx_rf_power_level(int level)
{
  if (level < 0 || level > 100)
    {
      throw error {"Native FLEX rfpower level out of range"};
    }

  send_command(
      QStringLiteral("transmit set rfpower=%1").arg(level));
}

void NativeFlexTransceiver::do_slice_af_gain(int gain)
{
  if (gain < 0 || gain > 100)
    {
      throw error {"Native FLEX slice audio gain out of range"};
    }

  if (slice_id_ < 0)
    {
      // No slice yet: the level is re-pushed on the next band arrival.
      return;
    }

  // Unverified against hardware: command shape mirrors the working
  // "transmit set rfpower=" command above, but has not been checked
  // against a real radio. Needs confirming that "slice s <id>
  // audio_gain=<n>" is the correct SmartSDR command for slice RX
  // audio gain.
  send_command(
      QStringLiteral("slice s %1 audio_gain=%2")
          .arg(slice_id_)
          .arg(gain));
}

void NativeFlexTransceiver::do_dax_gain(int gain, bool tx)
{
  if (gain < 0 || gain > 100)
    {
      throw error {"Native FLEX DAX gain out of range"};
    }

  if (tx)
    {
      if (!dax_tx_stream_id_)
        {
          return;
        }

      // Unverified against hardware: needs confirming that "dax tx
      // <n>" is the correct SmartSDR command for DAX TX gain, and
      // whether it should instead be addressed by dax_tx_stream_id_.
      send_command(
          QStringLiteral("dax tx %1")
              .arg(gain));
      return;
    }

  if (slice_id_ < 0)
    {
      // No slice yet: slice 0 is a real, commonly-first-created FLEX
      // slice, not a "no slice" sentinel, so this must not fall back
      // to it -- doing so could mutate a slice owned by another
      // client. The level is re-pushed on the next band arrival.
      return;
    }

  // Unverified against hardware, and likely wrong: dax_channel_ is a
  // DAX channel number (1-8), not an audio stream handle, so
  // addressing it as "audio stream 0x<dax_channel_>" is very
  // probably not the correct SmartSDR stream identifier. Needs
  // checking against a real radio which command (and which
  // identifier) actually sets DAX RX gain for a slice.
  send_command(
      QStringLiteral("audio stream 0x%1 slice %2 gain %3")
          .arg(dax_channel_, 0, 16)
          .arg(slice_id_)
          .arg(gain));
}

void NativeFlexTransceiver::capture_owned_slice(
    QByteArray const& line)
{
  /*
   * FLEX identifies the slice in status messages rather than
   * in the successful slice-create response.
   *
   * Example:
   *
   *   S<handle>|slice 1 ... in_use=1
   *       client_handle=0x<our handle> ...
   *
   * Only accept an in-use slice explicitly owned by our
   * current WSJT TCP client handle.
   */

  if (!line.startsWith('S'))
    {
      return;
    }

  int const slice_marker =
      line.indexOf("|slice ");

  if (slice_marker < 0)
    {
      return;
    }

  if (!line.contains("in_use=1"))
    {
      return;
    }

  QByteArray const handle_marker {
      "client_handle=0x"
  };

  int const handle_marker_pos =
      line.indexOf(handle_marker);

  if (handle_marker_pos < 0)
    {
      return;
    }

  int const handle_start =
      handle_marker_pos
      + handle_marker.size();

  int handle_end =
      line.indexOf(
          ' ',
          handle_start);

  if (handle_end < 0)
    {
      handle_end =
          line.size();
    }

  bool handle_ok = false;

  quint32 const status_handle =
      QString::fromLatin1(
          line.mid(
              handle_start,
              handle_end - handle_start))
          .toUInt(
              &handle_ok,
              16);

  if (
      !handle_ok
      || status_handle != client_handle_)
    {
      return;
    }

  int const slice_start =
      slice_marker
      + QByteArray {"|slice "}.size();

  int slice_end =
      line.indexOf(
          ' ',
          slice_start);

  if (slice_end < 0)
    {
      slice_end =
          line.size();
    }

  bool slice_ok = false;

  int const status_slice =
      QString::fromLatin1(
          line.mid(
              slice_start,
              slice_end - slice_start))
          .toInt(
              &slice_ok,
              10);

  if (
      slice_ok
      && status_slice >= 0)
    {
      slice_id_ =
          status_slice;

      /*
       * TransceiverBase::startup() does not automatically call
       * do_frequency() after do_start().
       *
       * Report the FLEX slice's actual starting frequency so
       * WSJT has a valid dial-frequency state immediately.
       */
      QByteArray const frequency_marker {
          "RF_frequency="
      };

      int const frequency_marker_pos =
          line.indexOf(frequency_marker);

      if (frequency_marker_pos >= 0)
        {
          int const frequency_start =
              frequency_marker_pos
              + frequency_marker.size();

          int frequency_end =
              line.indexOf(
                  ' ',
                  frequency_start);

          if (frequency_end < 0)
            {
              frequency_end =
                  line.size();
            }

          bool frequency_ok = false;

          double const frequency_mhz =
              QString::fromLatin1(
                  line.mid(
                      frequency_start,
                      frequency_end - frequency_start))
                  .toDouble(
                      &frequency_ok);

          if (
              frequency_ok
              && frequency_mhz > 0.0)
            {
              Frequency const frequency_hz =
                  static_cast<Frequency>(
                      frequency_mhz
                      * 1000000.0
                      + 0.5);

              update_rx_frequency(
                  frequency_hz);
            }
        }
    }
}

void NativeFlexTransceiver::capture_dax_tx_stream(
    QByteArray const& line)
{
  if (!line.startsWith('S'))
    {
      return;
    }

  int const pipe = line.indexOf('|');

  if (pipe <= 1)
    {
      return;
    }

  bool status_handle_ok = false;

  quint32 const status_handle =
      QString::fromLatin1(line.mid(1, pipe - 1))
          .toUInt(&status_handle_ok, 16);

  if (!status_handle_ok
      || status_handle != client_handle_)
    {
      return;
    }

  if (!line.contains("dax_tx"))
    {
      return;
    }

  /*
   * DEVIATION from W7PP: only accept a dax_tx stream that belongs to
   * THIS API client.
   *
   * The status-handle test above is not an ownership test: the "S<handle>|"
   * prefix is the handle of the subscriber being notified - i.e. always our
   * own - so it matches for every client's stream. Without the check below
   * we latch the FIRST dax_tx stream the radio mentions, which on a radio
   * that also has SmartSDR (or any other client) connected is somebody
   * else's stream. We then send our TX audio into their stream while our
   * own transmit slice receives nothing: the radio keys and emits an
   * unmodulated carrier, so the signal looks present on a waterfall but is
   * undecodable. Observed on a FLEX-8400M with SmartSDR-Mac connected.
   *
   * FlexVitaReceiver already does exactly this on the RX side ("Accept only
   * the stream belonging to THIS API client handle"); the TX side was
   * missing the equivalent guard.
   *
   * DEVIATION from W7PP: fail closed when "client_handle=" itself is
   * absent from the line. The original form of this guard only checked
   * ownership when the field was present ("if (owner_pos >= 0)"), so a
   * dax_tx status line missing that field skipped the ownership test
   * entirely and was latched anyway -- precisely the mis-route this
   * deviation exists to prevent. Treat a missing field as "not ours"
   * and return; wait_for_dax_tx_stream() already fails cleanly with a
   * clear "no WSJT-owned DAX-TX stream" error if no stream is ever
   * captured, so the worst case is a startup error, not a silent
   * mis-route. Log the skipped line with qWarning() (captured by the
   * app's log handler) rather than CAT_TRACE (filtered out and
   * invisible in practice) so the case is diagnosable.
   */
  {
    int const owner_pos = line.indexOf("client_handle=");

    if (owner_pos < 0)
      {
        qWarning()
            << "NativeFlexTransceiver: dax_tx status line has no"
               " client_handle= field, treating as not ours:"
            << line;

        return;
      }

    int const owner_start =
        owner_pos + static_cast<int>(qstrlen("client_handle="));

    int owner_end =
        line.indexOf(' ', owner_start);

    if (owner_end < 0)
      {
        owner_end = line.size();
      }

    bool owner_ok = false;

    quint32 const owner_handle =
        QString::fromLatin1(
            line.mid(
                owner_start,
                owner_end - owner_start))
            .trimmed()
            .toUInt(&owner_ok, 16);

    if (!owner_ok
        || owner_handle != client_handle_)
      {
        return;
      }
  }

  QList<QByteArray> const markers {
      "|audio_stream ",
      "|stream ",
      "|dax_tx "
  };

  for (auto const& marker : markers)
    {
      int const marker_pos = line.indexOf(marker);

      if (marker_pos < 0)
        {
          continue;
        }

      int const id_start =
          marker_pos + marker.size();

      int id_end = line.indexOf(' ', id_start);

      if (id_end < 0)
        {
          id_end = line.size();
        }

      QByteArray id_text =
          line.mid(id_start, id_end - id_start).trimmed();

      if (id_text.startsWith("0x"))
        {
          id_text.remove(0, 2);
        }

      bool id_ok = false;

      quint32 const stream_id =
          QString::fromLatin1(id_text)
              .toUInt(&id_ok, 16);

      if (id_ok && stream_id != 0)
        {
          dax_tx_stream_id_ = stream_id;
          return;
        }
    }
}

void NativeFlexTransceiver::capture_transmit_status(
    QByteArray const& line)
{
  /*
   * W7PP 
   *
   * Capture transmitter capability/status reported
   * by the selected Native FLEX radio.
   *
   * This function is read-only with respect to the radio.
   */
  if (!line.startsWith('S'))
    {
      return;
    }

  int const pipe =
      line.indexOf('|');

  if (pipe <= 1)
    {
      return;
    }

  QByteArray const payload =
      line.mid(pipe + 1).trimmed();

  QByteArray const prefix {
      "transmit "
  };

  if (!payload.startsWith(prefix))
    {
      return;
    }

  QCoreApplication * const app =
      QCoreApplication::instance();

  if (!app)
    {
      return;
    }

  QByteArray const values =
      payload.mid(prefix.size());

  for (QByteArray const& field : values.split(' '))
    {
      QByteArray property;
      QByteArray marker;

      if (field.startsWith("max_power_level="))
        {
          marker = "max_power_level=";
          property = "W7PPNativeFlexMaxPowerLevel";
        }
      else if (field.startsWith("max_internal_pa_power="))
        {
          marker = "max_internal_pa_power=";
          property = "W7PPNativeFlexMaxInternalPaPower";
        }
      else if (field.startsWith("rfpower="))
        {
          marker = "rfpower=";
          property = "W7PPNativeFlexRfPower";
        }
      else if (field.startsWith(
                   "tx_rf_power_changes_allowed="))
        {
          marker = "tx_rf_power_changes_allowed=";
          property =
              "W7PPNativeFlexRfPowerChangesAllowed";
        }
      else
        {
          continue;
        }

      bool ok = false;

      int const value =
          QString::fromLatin1(
              field.mid(marker.size()))
              .toInt(&ok);

      if (!ok || value < 0)
        {
          continue;
        }

      app->setProperty(
          property.constData(),
          QVariant::fromValue(value));
    }

  /*
   * W7PP 
   *
   * Evidence only.
   *
   * Write the most recent FLEX transmitter status
   * received by this Native FLEX client.
   *
   * This sends NO command to the radio.
   */
  QFile evidence {
      QCoreApplication::applicationDirPath() +
      QStringLiteral("/W7PP_NATIVE_FLEX_TX_STATUS.txt")
  };

  if (evidence.open(
          QIODevice::WriteOnly
          | QIODevice::Truncate
          | QIODevice::Text))
    {
      QTextStream out {&evidence};

      out << "raw="
          << QString::fromLatin1(payload)
          << '\n';

      out << "max_internal_pa_power="
          << app->property(
                 "W7PPNativeFlexMaxInternalPaPower")
                 .toString()
          << '\n';

      out << "max_power_level="
          << app->property(
                 "W7PPNativeFlexMaxPowerLevel")
                 .toString()
          << '\n';

      out << "rfpower="
          << app->property(
                 "W7PPNativeFlexRfPower")
                 .toString()
          << '\n';

      out << "tx_rf_power_changes_allowed="
          << app->property(
                 "W7PPNativeFlexRfPowerChangesAllowed")
                 .toString()
          << '\n';

      evidence.close();
    }
}

/*
 * DEVIATION from W7PP: wait for the slice we just asked FLEX to create.
 *
 * The slice index arrives in the slice status stream, not in the
 * "slice create" response, so W7PP's test of slice_id_ on the line
 * after the create raced the radio. Losing that race aborted a connect
 * that was about to succeed, and - worse - left slice_id_ holding an
 * index from a previous session, so every later "slice t <n> ..."
 * addressed a slice that no longer existed and came back
 * 0x5000000D "Invalid slice receiver".
 */
void NativeFlexTransceiver::wait_for_owned_slice()
{
  QElapsedTimer timer;
  timer.start();

  while (timer.elapsed() < 3000
      && slice_id_ < 0)
    {
      if (control_socket_->bytesAvailable() == 0)
        {
          control_socket_->waitForReadyRead(250);
        }

      if (QAbstractSocket::ConnectedState
          != control_socket_->state())
        {
          throw std::runtime_error {
              "Native FLEX TCP closed while waiting for the WSJT slice."
          };
        }

      if (control_socket_->bytesAvailable() > 0)
        {
          pending_control_ += control_socket_->readAll();
        }

      while (true)
        {
          int const cr = pending_control_.indexOf('\r');
          int const lf = pending_control_.indexOf('\n');
          int end = -1;

          if (cr >= 0)
            {
              end = cr;
            }

          if (lf >= 0 && (end < 0 || lf < end))
            {
              end = lf;
            }

          if (end < 0)
            {
              break;
            }

          QByteArray line =
              pending_control_.left(end);

          pending_control_.remove(0, end + 1);

          while (!pending_control_.isEmpty()
              && ('\r' == pending_control_.at(0)
                  || '\n' == pending_control_.at(0)))
            {
              pending_control_.remove(0, 1);
            }

          line = line.trimmed();

          capture_owned_slice(line);
          capture_dax_tx_stream(line);
          capture_transmit_status(line);
        }
    }

  if (slice_id_ < 0)
    {
      throw std::runtime_error {
          "Native FLEX reported no WSJT-owned slice."
      };
    }
}

void NativeFlexTransceiver::wait_for_dax_tx_stream()
{
  QElapsedTimer timer;
  timer.start();

  while (timer.elapsed() < 3000
      && dax_tx_stream_id_ == 0)
    {
      if (control_socket_->bytesAvailable() == 0)
        {
          control_socket_->waitForReadyRead(250);
        }

      if (QAbstractSocket::ConnectedState
          != control_socket_->state())
        {
          throw std::runtime_error {
              "Native FLEX TCP closed while waiting for DAX-TX stream."
          };
        }

      if (control_socket_->bytesAvailable() > 0)
        {
          pending_control_ += control_socket_->readAll();
        }

      while (true)
        {
          int const cr = pending_control_.indexOf('\r');
          int const lf = pending_control_.indexOf('\n');
          int end = -1;

          if (cr >= 0)
            {
              end = cr;
            }

          if (lf >= 0 && (end < 0 || lf < end))
            {
              end = lf;
            }

          if (end < 0)
            {
              break;
            }

          QByteArray line =
              pending_control_.left(end);

          pending_control_.remove(0, end + 1);

          while (!pending_control_.isEmpty()
              && ('\r' == pending_control_.at(0)
                  || '\n' == pending_control_.at(0)))
            {
              pending_control_.remove(0, 1);
            }

          line = line.trimmed();

          capture_owned_slice(line);
          capture_dax_tx_stream(line);
          capture_dax_tx_stream(line);
          capture_transmit_status(line);
        }
    }

  if (dax_tx_stream_id_ == 0)
    {
      throw std::runtime_error {
          "Native FLEX reported no WSJT-owned DAX-TX stream."
      };
    }
}

QByteArray NativeFlexTransceiver::send_command(
    QString const& command)
{
  if (
      !control_socket_
      || QAbstractSocket::ConnectedState
          != control_socket_->state())
    {
      throw std::runtime_error {
          "Native FLEX control socket is not connected."
      };
    }

  quint32 const sequence =
      next_sequence_++;

  QByteArray wire =
      QByteArray {"C"}
      + QByteArray::number(sequence)
      + "|"
      + command.toLatin1()
      + "\r\n";

  qint64 const queued =
      control_socket_->write(wire);

  if (queued != wire.size())
    {
      throw std::runtime_error {
          "Native FLEX failed to queue radio command."
      };
    }

  if (!control_socket_->waitForBytesWritten(2000))
    {
      throw std::runtime_error {
          "Native FLEX failed to send radio command."
      };
    }

  QByteArray const response_prefix =
      QByteArray {"R"}
      + QByteArray::number(sequence)
      + "|";

  QElapsedTimer timer;
  timer.start();

  while (timer.elapsed() < 5000)
    {
      if (control_socket_->bytesAvailable() == 0)
        {
          control_socket_->waitForReadyRead(250);
        }

      if (
          QAbstractSocket::ConnectedState
          != control_socket_->state())
        {
          throw std::runtime_error {
              "Native FLEX TCP closed while waiting for command response."
          };
        }

      if (control_socket_->bytesAvailable() > 0)
        {
          pending_control_ +=
              control_socket_->readAll();
        }

      while (true)
        {
          int const cr =
              pending_control_.indexOf('\r');

          int const lf =
              pending_control_.indexOf('\n');

          int end = -1;

          if (cr >= 0)
            {
              end = cr;
            }

          if (lf >= 0)
            {
              if (end < 0 || lf < end)
                {
                  end = lf;
                }
            }

          if (end < 0)
            {
              break;
            }

          QByteArray line =
              pending_control_.left(end);

          pending_control_.remove(
              0,
              end + 1);

          while (
              !pending_control_.isEmpty()
              && ('\r' == pending_control_.at(0)
                  || '\n' == pending_control_.at(0)))
            {
              pending_control_.remove(0, 1);
            }

          line = line.trimmed();

          capture_owned_slice(line);
          capture_dax_tx_stream(line);
          capture_transmit_status(line);

          if (!line.startsWith(response_prefix))
            {
              continue;
            }

          auto const fields =
              line.split('|');

          if (fields.size() < 3)
            {
              throw std::runtime_error {
                  "Native FLEX command response is malformed."
              };
            }

          bool response_ok = false;

          quint32 const response_code =
              QString::fromLatin1(
                  fields.at(1))
                  .toUInt(
                      &response_ok,
                      16);

          if (!response_ok)
            {
              throw std::runtime_error {
                  "Native FLEX command response code is malformed."
              };
            }

          if (0 != response_code)
            {
              /*
               * DEVIATION from W7PP: include the radio's own
               * explanation. FLEX returns it after the status code
               * (e.g. "The maximum number of connected clients has
               * been reached"); reporting only the hex leaves the
               * operator with nothing actionable. The text may itself
               * contain '|', so rejoin every remaining field.
               */
              QString const detail =
                  QString::fromLatin1(
                      fields.mid(2).join('|')).trimmed();

              QString const message =
                  QString {
                      "Native FLEX command failed: %1 response 0x%2%3"
                  }
                  .arg(command)
                  .arg(
                      response_code,
                      8,
                      16,
                      QLatin1Char('0'))
                  .arg(
                      detail.isEmpty()
                      ? QString {}
                      : QString {" - "} + detail);

              throw std::runtime_error {
                  message.toStdString()
              };
            }

          return fields.at(2).trimmed();
        }
    }

  QString const message =
      QString {
          "Native FLEX command timed out: %1"
      }
      .arg(command);

  throw std::runtime_error {
      message.toStdString()
  };
}

int NativeFlexTransceiver::do_start()
{
  /*
   * W7PP 
   *
   * Native FLEX only.
   * Clear any stale TX route before establishing this session.
   */
  if (QCoreApplication::instance())
    {
      QCoreApplication::instance()->setProperty(
          "W7PPNativeFlexTxRadioAddress",
          QString {});

      QCoreApplication::instance()->setProperty(
          "W7PPNativeFlexTxStreamId",
          QVariant::fromValue(
              static_cast<qulonglong>(0)));
    }
  /*
   * 
   *
   * Establish our own direct TCP connection to the selected
   * FLEX radio.
   *
   * We send NO radio commands in this step.
   */

  auto const radio =
      NativeFlexRadioSelection::selected();

  if (!radio.valid())
    {
      throw std::runtime_error {
          "No FLEX radio is selected for Flex Native VITA-49."
      };
    }

  control_socket_->abort();

  pending_control_.clear();
  api_version_.clear();
  gui_client_id_.clear();

  client_handle_ = 0;
  have_client_handle_ = false;

  next_sequence_ = 2;
  slice_id_ = -1;
  dax_tx_stream_id_ = 0;

  control_socket_->connectToHost(
      radio.address,
      radio.port);

  if (!control_socket_->waitForConnected(5000))
    {
      QString const message =
          QString {
              "Native FLEX TCP connection to %1:%2 failed: %3"
          }
          .arg(radio.address)
          .arg(radio.port)
          .arg(control_socket_->errorString());

      control_socket_->abort();

      throw std::runtime_error {
          message.toStdString()
      };
    }

  QElapsedTimer timer;
  timer.start();

  while (
      timer.elapsed() < 5000
      && (api_version_.isEmpty()
          || !have_client_handle_))
    {
      if (control_socket_->bytesAvailable() == 0)
        {
          control_socket_->waitForReadyRead(250);
        }

      if (
          QAbstractSocket::ConnectedState
          != control_socket_->state())
        {
          QString const message =
              QString {
                  "Native FLEX TCP connection closed: %1"
              }
              .arg(control_socket_->errorString());

          control_socket_->abort();

          throw std::runtime_error {
              message.toStdString()
          };
        }

      if (control_socket_->bytesAvailable() == 0)
        {
          continue;
        }

      pending_control_ +=
          control_socket_->readAll();

      while (true)
        {
          int const cr =
              pending_control_.indexOf('\r');

          int const lf =
              pending_control_.indexOf('\n');

          int end = -1;

          if (cr >= 0)
            {
              end = cr;
            }

          if (lf >= 0)
            {
              if (end < 0 || lf < end)
                {
                  end = lf;
                }
            }

          if (end < 0)
            {
              break;
            }

          QByteArray line =
              pending_control_.left(end);

          pending_control_.remove(
              0,
              end + 1);

          while (
              !pending_control_.isEmpty()
              && ('\r' == pending_control_.at(0)
                  || '\n' == pending_control_.at(0)))
            {
              pending_control_.remove(0, 1);
            }

          line = line.trimmed();

          if (line.size() < 2)
            {
              continue;
            }

          if (
              'V' == line.at(0)
              && api_version_.isEmpty())
            {
              api_version_ =
                  QString::fromLatin1(
                      line.mid(1))
                      .trimmed();

              continue;
            }

          if (
              'H' == line.at(0)
              && !have_client_handle_)
            {
              bool ok = false;

              quint32 const handle =
                  QString::fromLatin1(
                      line.mid(1))
                      .trimmed()
                      .toUInt(
                          &ok,
                          16);

              if (ok)
                {
                  client_handle_ = handle;
                  have_client_handle_ = true;
                }
            }
        }
    }

  if (api_version_.isEmpty())
    {
      control_socket_->abort();

      throw std::runtime_error {
          "Native FLEX TCP did not receive protocol version."
      };
    }

  if (!have_client_handle_)
    {
      control_socket_->abort();

      throw std::runtime_error {
          "Native FLEX TCP did not receive client handle."
      };
    }

  /*
   * Headless Native FLEX ownership.
   *
   * Register WSJT-X itself as a FLEX GUI/API client.
   *
   * This does NOT involve SmartSDR.
   *
   * FLEX resources that we create later, including our
   * operating slice, will belong to this WSJT-X client.
   */
  QByteArray const gui_command {
      "C1|client gui\r\n"
  };

  qint64 const queued =
      control_socket_->write(
          gui_command);

  if (queued != gui_command.size())
    {
      control_socket_->abort();

      throw std::runtime_error {
          "Native FLEX failed to queue client gui command."
      };
    }

  if (!control_socket_->waitForBytesWritten(2000))
    {
      control_socket_->abort();

      throw std::runtime_error {
          "Native FLEX failed to send client gui command."
      };
    }

  bool gui_response_received = false;

  QElapsedTimer gui_timer;
  gui_timer.start();

  while (
      gui_timer.elapsed() < 5000
      && !gui_response_received)
    {
      if (control_socket_->bytesAvailable() == 0)
        {
          control_socket_->waitForReadyRead(250);
        }

      if (
          QAbstractSocket::ConnectedState
          != control_socket_->state())
        {
          control_socket_->abort();

          throw std::runtime_error {
              "Native FLEX TCP closed while registering GUI client."
          };
        }

      if (control_socket_->bytesAvailable() > 0)
        {
          pending_control_ +=
              control_socket_->readAll();
        }

      while (true)
        {
          int const cr =
              pending_control_.indexOf('\r');

          int const lf =
              pending_control_.indexOf('\n');

          int end = -1;

          if (cr >= 0)
            {
              end = cr;
            }

          if (lf >= 0)
            {
              if (end < 0 || lf < end)
                {
                  end = lf;
                }
            }

          if (end < 0)
            {
              break;
            }

          QByteArray line =
              pending_control_.left(end);

          pending_control_.remove(
              0,
              end + 1);

          while (
              !pending_control_.isEmpty()
              && ('\r' == pending_control_.at(0)
                  || '\n' == pending_control_.at(0)))
            {
              pending_control_.remove(0, 1);
            }

          line = line.trimmed();

          capture_owned_slice(line);
          capture_dax_tx_stream(line);
          capture_transmit_status(line);

          if (!line.startsWith("R1|"))
            {
              continue;
            }

          auto const fields =
              line.split('|');

          if (fields.size() < 3)
            {
              control_socket_->abort();

              throw std::runtime_error {
                  "Native FLEX client gui response is malformed."
              };
            }

          bool response_ok = false;

          quint32 const response_code =
              QString::fromLatin1(
                  fields.at(1))
                  .toUInt(
                      &response_ok,
                      16);

          if (!response_ok)
            {
              control_socket_->abort();

              throw std::runtime_error {
                  "Native FLEX client gui response code is malformed."
              };
            }

          if (0 != response_code)
            {
              /*
               * DEVIATION from W7PP: surface the radio's explanation.
               * 0xF3000001 is "The maximum number of connected clients
               * has been reached" - the operator needs that, not a bare
               * hex code.
               */
              QString const detail =
                  QString::fromLatin1(
                      fields.mid(2).join('|')).trimmed();

              QString const message =
                  QString {
                      "Native FLEX client gui failed: response 0x%1%2"
                  }
                  .arg(
                      response_code,
                      8,
                      16,
                      QLatin1Char('0'))
                  .arg(
                      detail.isEmpty()
                      ? QString {}
                      : QString {" - "} + detail);

              control_socket_->abort();

              throw std::runtime_error {
                  message.toStdString()
              };
            }

          gui_client_id_ =
              QString::fromLatin1(
                  fields.at(2))
                  .trimmed();

          if (gui_client_id_.isEmpty())
            {
              control_socket_->abort();

              throw std::runtime_error {
                  "Native FLEX client gui returned no client_id."
              };
            }

          gui_response_received = true;
          break;
        }
    }

  if (!gui_response_received)
    {
      control_socket_->abort();

      throw std::runtime_error {
          "Native FLEX client gui response timed out."
      };
    }

  /*
   * client gui may restore an existing slice for this client.
   *
   * If FLEX already reported one owned by our client handle,
   * use it.  Do NOT create an unnecessary second slice.
   */
  if (slice_id_ < 0)
    {
      /*
       * No restored slice was reported.
       *
       * Request one.  The successful command response may
       * contain no data; the assigned slice number arrives in
       * the FLEX slice status stream and capture_owned_slice()
       * records it.
       */
      send_command(
          QString {
              "slice create mode=digu"
          });
    }

  wait_for_owned_slice();

  /*
   * Native WSJT operation always uses DIGU.
   */
  send_command(
      QString {
          "slice s %1 mode=digu"
      }
      .arg(slice_id_));

  /*
   * DEVIATION from W7PP: route the WSJT-owned slice into the DAX RX
   * channel FlexVitaReceiver streams from.
   *
   * W7PP leaves this to the operator, who has to open the SmartSDR
   * DAX panel and set the slice's DAX channel by hand. Miss it and
   * the radio still creates the dax_rx stream and still sends VITA-49
   * packets - they just carry silence, so the radio reads as
   * connected with a dead RX level meter.
   *
   * This is the authoritative binding: it names the slice WSJT owns,
   * so it wins over the receiver's own fallback routing, which only
   * fires when nothing at all feeds the channel.
   */
  send_command(
      QString {
          "slice s %1 dax=%2"
      }
      .arg(slice_id_)
      .arg(dax_channel_));

  /*
   * W7PP :
   *
   * Native FLEX TX routing only.
   *
   * The WSJT-owned slice becomes the FLEX TX slice and
   * DAX becomes the radio's primary transmit audio source.
   *
   * IMPORTANT:
   * No xmit command is sent here.
   * No VITA TX packets are sent here.
   */
  send_command(
      QString {
          "slice s %1 tx=1 mode=digu"
      }
      .arg(slice_id_));

  send_command(
      QString {
          "transmit set dax=1"
      });

  /*
   * W7PP :
   * Register THIS WSJT Native FLEX client as the
   * transmit-sample source for the selected DAX channel.
   * No PTT and no TX packets are generated here.
   */
  send_command(
      QString {
          "dax audio set %1 slice=%2 tx=1"
      }
      .arg(dax_channel_)
      .arg(slice_id_));

  /*
   * W7PP :
   *
   * Subscribe before creating the client-owned DAX-TX stream.
   * The create response may contain no stream ID, so wait
   * for the corresponding FLEX status line and capture it.
   *
   * No VITA packets and no PTT are sent here.
   */
  /*
   * W7PP 
   *
   * Subscribe to transmitter capability/status.
   * This changes no RF-power setting and does not key.
   */
  send_command(
      QString {
          "sub tx all"
      });

  send_command(
      QString {
          "sub audio_stream all"
      });

  dax_tx_stream_id_ = 0;

  send_command(
      QString {
          "stream create type=dax_tx"
      });

  wait_for_dax_tx_stream();
  /*
   * W7PP 
   *
   * Publish the exact route learned by this Native FLEX
   * session for the later TX transport:
   *
   *   radio.address      = FLEX destination IP
   *   dax_tx_stream_id_  = this WSJT client's DAX-TX stream
   *
   * No UDP socket or packet transmission is created here.
   */
  if (QCoreApplication::instance())
    {
      QCoreApplication::instance()->setProperty(
          "W7PPNativeFlexTxRadioAddress",
          radio.address);

      QCoreApplication::instance()->setProperty(
          "W7PPNativeFlexTxStreamId",
          QVariant::fromValue(
              static_cast<qulonglong>(
                  dax_tx_stream_id_)));
    }

  /*
   * W7PP :
   *
   * Native FLEX only.
   *
   * If MainWindow supplies a saved Native FLEX startup
   * frequency, command the WSJT-owned slice now that the
   * TCP session and slice are fully established.
   *
   * Reuse the already-proven do_frequency() path:
   *
   *   slice t <slice> <MHz>
   *
   * Default startup_frequency_ is zero, so  by
   * itself changes no live behavior.
   */
  if (startup_frequency_)
    {
      do_frequency(
          startup_frequency_,
          UNK,
          true);
    }

  /*
   * PTT and VITA TX remain inactive.
   */
  /*
   * W7PP 
   *
   * Start the independent safety telemetry client only
   * after the Native FLEX control session is established.
   *
   * Failure to start this monitor does not damage RX.
   * The cached TX safety gate simply remains fail-closed.
   */
  safety_monitor_.stop();

  NativeFlexSafetyMonitor::Configuration safetyConfiguration;
  safetyConfiguration.radio_address = radio.address.toStdString();
  safetyConfiguration.tcp_port = static_cast<int>(radio.port);
  safetyConfiguration.first_udp_port = 5011;
  safetyConfiguration.last_udp_port = 5026;
  (void)safety_monitor_.start(safetyConfiguration);
  return 0;
}

void NativeFlexTransceiver::do_stop()
{
  /*
   * W7PP 
   *
   * Best-effort radio unkey before control-socket teardown.
   * Safety telemetry is then stopped independently.
   */
  if (
      control_socket_
      && QAbstractSocket::ConnectedState
          == control_socket_->state())
    {
      try
        {
          send_command(
              QString {
                  "xmit 0"
              });
        }
      catch (...)
        {
          // Best effort shutdown continues.
        }
    }

  update_PTT(false);
  safety_monitor_.stop();
  /*
   * W7PP 
   *
   * This Native FLEX session no longer owns a TX route.
   */
  if (QCoreApplication::instance())
    {
      QCoreApplication::instance()->setProperty(
          "W7PPNativeFlexTxRadioAddress",
          QString {});

      QCoreApplication::instance()->setProperty(
          "W7PPNativeFlexTxStreamId",
          QVariant::fromValue(
              static_cast<qulonglong>(0)));
    }
  /*
   * Remove the slice created and owned by this WSJT session.
   */
  if (
      slice_id_ >= 0
      && control_socket_
      && QAbstractSocket::ConnectedState
          == control_socket_->state())
    {
      try
        {
          send_command(
              QString {
                  "slice r %1"
              }
              .arg(slice_id_));
        }
      catch (...)
        {
          // Best effort cleanup. Socket shutdown still proceeds.
        }
    }

  slice_id_ = -1;

  if (control_socket_)
    {
      if (
          QAbstractSocket::UnconnectedState
          != control_socket_->state())
        {
          control_socket_->disconnectFromHost();

          if (
              QAbstractSocket::UnconnectedState
              != control_socket_->state())
            {
              control_socket_->waitForDisconnected(1000);
            }

          control_socket_->abort();
        }
    }

  pending_control_.clear();
  api_version_.clear();
  gui_client_id_.clear();

  client_handle_ = 0;
  have_client_handle_ = false;

  next_sequence_ = 2;
  slice_id_ = -1;
}

void NativeFlexTransceiver::do_frequency(
    Frequency frequency,
    MODE,
    bool)
{
  if (slice_id_ < 0)
    {
      throw std::runtime_error {
          "Native FLEX has no WSJT-owned slice."
      };
    }

  QString const mhz =
      QString::number(
          static_cast<double>(frequency)
              / 1000000.0,
          'f',
          6);

  send_command(
      QString {
          "slice t %1 %2"
      }
      .arg(slice_id_)
      .arg(mhz));

  /*
   * Native FLEX operation is always DIGU.
   */
  send_command(
      QString {
          "slice s %1 mode=digu"
      }
      .arg(slice_id_));

  update_rx_frequency(frequency);
}

void NativeFlexTransceiver::do_tx_frequency(
    Frequency frequency,
    MODE mode,
    bool)
{
  /* Maintain WSJT-X transmit-frequency state; Native FLEX slice/transmit setup handles radio frequency control. */

  update_other_frequency(frequency);
  update_split(0 != frequency);

  if (UNK != mode)
    {
      update_mode(mode);
    }
}

void NativeFlexTransceiver::do_mode(MODE mode)
{
  /*
   * WSJT Native FLEX always operates the radio slice in DIGU.
   *
   * WSJT's own FT8/FT4/etc mode remains a WSJT application
   * mode; the FLEX demodulator is DIGU.
   */
  if (slice_id_ >= 0)
    {
      send_command(
          QString {
              "slice s %1 mode=digu"
          }
          .arg(slice_id_));
    }

  if (UNK != mode)
    {
      update_mode(mode);
    }
}

void NativeFlexTransceiver::do_ptt(bool on)
{
  /*
   * W7PP 
   *
   * Native FLEX PTT uses the existing W7PP control
   * command/response path.
   *
   * WSJT state is asserted only after FLEX accepts
   * the transmit-on command.
   */
  if (on)
    {
      /*
       * W7PP 
       *
       * Cached, nonblocking, fail-closed safety gate.
       * No network I/O, meter parsing, waits, or VITA work
       * occurs on this PTT path.
       */
      auto const safety =
          NativeFlexSafetyMonitor::gateSnapshot();

      if (!safety.monitor_connected)
        {
          update_PTT(false);
          throw error {"Native FLEX TX INHIBITED: safety monitor not connected"};
        }

      if (!safety.meter_stream_active)
        {
          update_PTT(false);
          throw error {"Native FLEX TX INHIBITED: safety meter stream inactive"};
        }

      if (!safety.interlock_seen)
        {
          update_PTT(false);
          throw error {"Native FLEX TX INHIBITED: no FLEX interlock status"};
        }

      /*
       * DEVIATION from W7PP: attach the interlock detail the monitor
       * has already captured (state / reason / source).
       *
       * The gate itself still reads only the lock-free GateSnapshot, so
       * the success path stays nonblocking as designed. The full
       * snapshot is taken ONLY when we are already aborting the
       * transmission, where a brief mutex is free.
       *
       * Without this the operator sees "FLEX reports TX not allowed"
       * and has nothing to act on; the radio has usually said why.
       *
       * The "Native FLEX TX INHIBITED:" prefix must remain FIRST -
       * TransceiverBase matches on it to treat the inhibit as
       * recoverable rather than taking the rig offline.
       */
      auto const interlock_detail =
          [this] () -> QString
          {
            auto const full = safety_monitor_.snapshot();

            if (full.interlock_state.empty()
                && full.interlock_reason.empty()
                && full.interlock_source.empty())
              {
                return {};
              }

            return
                QString {" (interlock state=%1 reason=%2 source=%3)"}
                .arg(QString::fromStdString(full.interlock_state))
                .arg(QString::fromStdString(full.interlock_reason))
                .arg(QString::fromStdString(full.interlock_source));
          };

      if (!safety.tx_allowed)
        {
          update_PTT(false);
          throw error {
              QString {"Native FLEX TX INHIBITED: FLEX reports TX not allowed"}
              + interlock_detail()};
        }

      if (!safety.interlock_ready)
        {
          update_PTT(false);
          throw error {
              QString {"Native FLEX TX INHIBITED: FLEX interlock is not READY"}
              + interlock_detail()};
        }

      if (!safety.all_safety_meters_seen)
        {
          update_PTT(false);
          throw error {"Native FLEX TX INHIBITED: safety meters incomplete"};
        }

      if (
          safety.meter_age_ms < 0
          || safety.meter_age_ms > 2000)
        {
          update_PTT(false);
          throw error {"Native FLEX TX INHIBITED: safety telemetry is stale"};
        }
      try
        {
          send_command(
              QString {
                  "xmit 1"
              });
        }
      catch (...)
        {
          /*
           * A failed or lost response does not prove that
           * FLEX failed to receive the key command.
           *
           * Make one best-effort unkey attempt before
           * propagating the original failure.
           */
          try
            {
              send_command(
                  QString {
                      "xmit 0"
                  });
            }
          catch (...)
            {
              /*
               * Preserve the original key-up failure.
               */
            }

          update_PTT(false);
          throw;
        }

      update_PTT(true);
      return;
    }

  /*
   * Unkey first; then publish the local WSJT state.
   *
   * If the radio acknowledgement fails, local PTT is
   * still cleared before the failure is propagated.
   */
  try
    {
      send_command(
          QString {
              "xmit 0"
          });
    }
  catch (...)
    {
      update_PTT(false);
      throw;
    }

  update_PTT(false);
}
