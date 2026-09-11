// W7PP modifications Copyright (C) 2026 Dick Hale / W7PP.

#include "NativeFlexTransceiver.hpp"
#include <QCoreApplication>
#include <QThread>
#include <QVariant>
#include <QDebug>

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QTcpSocket>

#include <stdexcept>

#include "NativeFlexRadioSelection.hpp"

NativeFlexTransceiver::Frequency NativeFlexTransceiver::startup_frequency_ {0};
int NativeFlexTransceiver::dax_channel_ {1};

namespace
{
  /*
   * W7PP (v1.09.1):
   *
   * Publish this session's dynamically assigned DAX-TX stream
   * route on QCoreApplication's owning thread.
   *
   * DEVIATION from W7PP: one helper for the three call sites (the
   * donor publishes on the application thread after connect but
   * still clears the route directly), and it never blocks.
   *
   * A BlockingQueuedConnection here deadlocks on exit: Configuration
   * tears the rig down with transceiver_thread_->quit() + wait() while
   * the main event loop is already stopped, so a publish landing in
   * that window has the main thread waiting on the transceiver thread
   * and the transceiver thread waiting on the main thread.
   *
   * A plain queued publish is sufficient. The route only has to be
   * visible before the audio side may transmit, and TX cannot begin
   * until the resolution/update signals reach the main thread - those
   * are posted to the same main-thread event queue AFTER this publish
   * event, so FIFO delivery guarantees the ordering. A queued clear
   * that never runs because the application is gone is harmless.
   *
   * No stream number is hard-coded here.
   */
  void publish_native_flex_tx_route(
      QString const& radio_address,
      quint32 stream_id)
  {
    auto * app = QCoreApplication::instance();

    if (!app)
      {
        return;
      }

    qulonglong const published_tx_stream_id =
        static_cast<qulonglong>(stream_id);

    auto publish =
        [radio_address, published_tx_stream_id] ()
        {
          auto * app = QCoreApplication::instance();

          if (!app)
            {
              return;
            }

          app->setProperty(
              "W7PPNativeFlexTxRadioAddress",
              radio_address);

          app->setProperty(
              "W7PPNativeFlexTxStreamId",
              QVariant::fromValue(published_tx_stream_id));
        };

    if (app->thread() == QThread::currentThread())
      {
        publish();
        return;
      }

    QMetaObject::invokeMethod(
        app,
        publish,
        Qt::QueuedConnection);
  }

  /*
   * Read the "pan=0x...." field of a FLEX slice status line.
   *
   * Shared by the two places that need it so the pre-existing
   * panadapter snapshot and the owned-slice capture cannot drift
   * apart in how they parse the same field.
   */
  bool parse_panadapter_id(
      QByteArray const& line,
      quint32 & panadapter_id)
  {
    QByteArray const pan_marker {
        "pan=0x"
    };

    int const pan_marker_pos =
        line.indexOf(pan_marker);

    if (pan_marker_pos < 0)
      {
        return false;
      }

    int const pan_start =
        pan_marker_pos
        + pan_marker.size();

    int pan_end =
        line.indexOf(
            ' ',
            pan_start);

    if (pan_end < 0)
      {
        pan_end =
            line.size();
      }

    bool pan_ok = false;

    quint32 const pan =
        QString::fromLatin1(
            line.mid(
                pan_start,
                pan_end - pan_start))
            .toUInt(
                &pan_ok,
                16);

    if (!pan_ok)
      {
        return false;
      }

    panadapter_id = pan;

    return true;
  }
}

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
  cached_tx_rf_power_level_ = level;
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

  /*
   * DELIBERATELY DISABLED pending verification against a real radio.
   *
   * Neither command below has ever been confirmed against hardware,
   * and unlike the unverified *status* field names -- which merely
   * fail to populate, and so fail closed -- a wrong write fails OPEN
   * and changes the radio.
   *
   * TX: "dax tx <n>" is, as far as is known, SmartSDR's enable/disable
   * boolean for DAX transmit, not a gain. If that is right, arriving at
   * a band whose stored DaxTxGain is 0 would send "dax tx 0", disable
   * DAX transmit, and put WSJT-Z silently off the air -- automatically,
   * driven by the band hopper, with no error anywhere.
   *
   * RX: "audio stream 0x<dax_channel_> ..." addresses a resource by a
   * guessed identifier. dax_channel_ is a DAX channel number (1-8), not
   * an audio stream handle, so this very probably names some other
   * client's stream. Commit 5efff7b removed a slice-0 fallback for
   * exactly this reason.
   *
   * To re-enable, BOTH must be settled in a radio session:
   *   - the correct SmartSDR command form for DAX TX gain (and whether
   *     "dax tx <n>" really is the enable/disable boolean);
   *   - the correct SmartSDR command form for DAX RX gain, plus a real
   *     RX stream handle captured from the status stream the way
   *     dax_tx_stream_id_ already is, with the command gated on it
   *     being non-zero -- mirroring the TX branch below.
   *
   * Everything around this stays live: the per-band store, capture,
   * persistence, migration and the whole antenna settle feature. Only
   * these two pushes are dark.
   */

  QString would_send;

  if (tx)
    {
      if (!dax_tx_stream_id_)
        {
          return;
        }

      would_send =
          QStringLiteral("dax tx %1")
              .arg(gain);
    }
  else
    {
      if (slice_id_ < 0)
        {
          // No slice yet: slice 0 is a real, commonly-first-created
          // FLEX slice, not a "no slice" sentinel, so this must not
          // fall back to it -- doing so could mutate a slice owned by
          // another client. The level is re-pushed on the next band
          // arrival.
          return;
        }

      would_send =
          QStringLiteral("audio stream 0x%1 slice %2 gain %3")
              .arg(dax_channel_, 0, 16)
              .arg(slice_id_)
              .arg(gain);
    }

  // qWarning() rather than CAT_TRACE: this is what the first radio
  // session needs to see in the log to confirm or correct the strings.
  qWarning()
      << "Native FLEX: DAX gain command suppressed pending hardware"
         " verification, would have sent:"
      << would_send;
}

void NativeFlexTransceiver::capture_smart_sdr_client(
    QByteArray const& line)
{
  /*
   * Read-only detection of an already-connected SmartSDR GUI client.
   *
   * This helper changes no radio state and does not bind clients.
   *
   * DEVIATION from W7PP: latch the decision to the mode-decision
   * window only.
   *
   * The donor runs this on every status line for the life of the
   * session, so a SmartSDR-Win started AFTER a headless WSJT-Z flips
   * smart_sdr_present_ mid-session. Everything downstream then
   * switches path against a session that already took the headless
   * route: do_ptt() starts writing "slice s <n> tx=1 mode=digu" and
   * do_stop() starts restoring a previous_tx_slice_id_ that was never
   * captured. The mode is decided once, around "sub client all".
   */
  if (!collecting_clients_)
    {
      return;
    }

  if (
      line.startsWith('S')
      && line.contains("|client ")
      && line.contains(" connected")
      && line.contains("program=SmartSDR-Win"))
    {
      smart_sdr_present_ = true;

      QByteArray const marker {"client_id="};

      int const marker_start =
          line.indexOf(marker);

      if (marker_start >= 0)
        {
          int const value_start =
              marker_start + marker.size();

          int value_end =
              line.indexOf(' ', value_start);

          if (value_end < 0)
            {
              value_end = line.size();
            }

          smart_sdr_client_id_ =
              QString::fromLatin1(
                  line.mid(
                      value_start,
                      value_end - value_start))
                  .trimmed();
        }
    }
}

void NativeFlexTransceiver::capture_owned_slice(
    QByteArray const& line)
{
  /*
   * FLEX identifies slices in the status stream.
   *
   * Headless mode:
   *   preserve the accepted ownership rule and accept only a
   *   slice explicitly owned by this W7PP TCP client handle.
   *
   * SmartSDR coexistence mode:
   *   first collect every slice number that already exists.
   *   While W7PP is issuing its own slice-create command,
   *   accept only a slice number that was not in that snapshot.
   *
   * FLEX may assign that new coexistence slice to the active
   * SmartSDR GUI handle.  The exact new slice number, rather
   * than GUI handle ownership, identifies W7PP's slice.
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
      !slice_ok
      || status_slice < 0)
    {
      return;
    }

  if (smart_sdr_present_)
    {
      if (slice_id_ >= 0)
        {
          if (status_slice != slice_id_)
            {
              return;
            }
        }
      else if (collecting_existing_slices_)
        {
          existing_slice_ids_.insert(
              status_slice);

          /*
           * DEVIATION from W7PP: snapshot the panadapters too.
           *
           * do_stop() removes whatever pan the new slice reported. On
           * a radio whose panadapters are all already in use - two
           * slices and two pans, both SmartSDR's - FLEX may attach
           * WSJT's new slice to an EXISTING pan rather than create
           * one, and the donor's cleanup then deletes the SmartSDR
           * operator's display. Anything seen here predates WSJT's
           * slice, so it is never ours to remove.
           */
          {
            quint32 pan = 0;

            if (parse_panadapter_id(line, pan))
              {
                existing_panadapter_ids_.insert(pan);
              }
          }

          /*
           * DEVIATION from W7PP: match " tx=1" with its leading
           * space.
           *
           * Slice status fields are space-separated, and a slice line
           * also carries dax_tx=1. The donor's bare "tx=1" match
           * therefore latches a slice that is NOT the TX slice, and
           * that id is later sent back to the radio as
           * "slice s <n> tx=1" by do_stop() and by the un-key path -
           * an active mutation of SmartSDR's state, not a read.
           */
          if (line.contains(" tx=1"))
            {
              previous_tx_slice_id_ =
                  status_slice;
            }

          return;
        }
      else if (creating_slice_)
        {
          if (existing_slice_ids_.contains(
                  status_slice))
            {
              return;
            }

          slice_id_ =
              status_slice;
        }
      else
        {
          return;
        }
    }
  else
    {
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

      slice_id_ =
          status_slice;
    }

  /*
   * In SmartSDR coexistence mode, remember the panadapter
   * attached to W7PP's exact newly-created slice so shutdown
   * can remove the extra SmartSDR display cleanly.
   */
  {
    quint32 pan = 0;

    if (parse_panadapter_id(line, pan))
      {
        slice_panadapter_id_ =
            pan;
      }
  }
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
   * DEVIATION from W7PP (v1.05.8; adopted upstream in v1.09.1): only
   * accept a dax_tx stream that belongs to THIS API client.
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

}

void NativeFlexTransceiver::capture_gain_status(
    QByteArray const& line)
{
  //
  // Publish the receive and transmit audio gains the radio reports,
  // so a level the operator changes in SmartSDR is seen here exactly
  // like one changed in WSJT-Z.
  //
  // Read-only with respect to the radio.
  //
  // The field names below (audio_gain= on slice status, rx_gain=/
  // tx_gain= on dax status) are unverified against hardware; no
  // radio was attached when this was written. If a property below
  // never populates against a live radio, capture the raw status
  // line and check whether the radio actually uses these names.
  //
  if (!line.startsWith('S'))
    {
      return;
    }

  int const pipe = line.indexOf('|');

  if (pipe <= 1)
    {
      return;
    }

  QByteArray const payload = line.mid(pipe + 1).trimmed();

  QCoreApplication * const app =
      QCoreApplication::instance();

  if (!app)
    {
      return;
    }

  if (payload.startsWith("slice "))
    {
      // Only our own slice speaks for the current band.
      if (slice_id_ < 0
          || !payload.startsWith(
                 QStringLiteral("slice %1 ")
                     .arg(slice_id_)
                     .toLatin1()))
        {
          return;
        }

      QByteArray const marker {"audio_gain="};

      for (QByteArray const& field : payload.split(' '))
        {
          if (!field.startsWith(marker))
            {
              continue;
            }

          bool ok = false;

          int const value =
              QString::fromLatin1(
                  field.mid(marker.size()))
                  .toInt(&ok);

          if (!ok || value < 0 || value > 100)
            {
              continue;
            }

          app->setProperty(
              "W7PPNativeFlexSliceAfGain",
              QVariant::fromValue(value));
          return;
        }

      return;
    }

  if (payload.startsWith("dax "))
    {
      // Fail closed: only publish DAX gains when the payload names
      // this client's own DAX channel. Neither of us has confirmed
      // the real DAX status payload shape, so this guesses that it
      // repeats the channel number the same way slice status repeats
      // the slice id, e.g. "dax 1 ...". THIS TOKEN IS PROVISIONAL and
      // is the first thing to suspect if W7PPNativeFlexDaxRxGain /
      // W7PPNativeFlexDaxTxGain never populate against a live radio
      // -- the payload may not carry a channel number in this
      // position at all, or may use a different form (hex, a
      // "channel=" field, a stream handle, etc). If this guess is
      // wrong the properties stay unset rather than being filled from
      // another client's DAX channel.
      if (!payload.startsWith(
              QStringLiteral("dax %1 ")
                  .arg(dax_channel_)
                  .toLatin1()))
        {
          return;
        }

      QByteArray const rx_marker {"rx_gain="};
      QByteArray const tx_marker {"tx_gain="};

      // A single status line may carry both rx_gain= and tx_gain=,
      // so both are extracted independently here rather than
      // stopping at the first match.
      for (QByteArray const& field : payload.split(' '))
        {
          QByteArray marker;
          QByteArray property;

          if (field.startsWith(rx_marker))
            {
              marker = rx_marker;
              property = "W7PPNativeFlexDaxRxGain";
            }
          else if (field.startsWith(tx_marker))
            {
              marker = tx_marker;
              property = "W7PPNativeFlexDaxTxGain";
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

          if (!ok || value < 0 || value > 100)
            {
              continue;
            }

          app->setProperty(
              property.constData(),
              QVariant::fromValue(value));
        }

      return;
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
/*
 * DEVIATION from W7PP: bounded status-stream drain.
 *
 * send_command() returns as soon as it sees the "R<seq>|" line for
 * the command it sent. FLEX does not guarantee that a subscription's
 * status dump precedes that response, so a caller that reads state
 * captured from the status stream the instant send_command() returns
 * can read it too early. This helper gives the radio a bounded window
 * to deliver those lines, feeding each one to the same capture set the
 * three wait loops use.
 *
 * FlexVitaReceiver does exactly this on the RX side: it calls
 * listenTcpFor(tcp, 1500ms) after its own "sub ..." subscriptions
 * before relying on what they reported.
 */
void NativeFlexTransceiver::drain_control_lines(int ms)
{
  QElapsedTimer timer;
  timer.start();

  while (timer.elapsed() < ms)
    {
      if (control_socket_->bytesAvailable() == 0)
        {
          control_socket_->waitForReadyRead(250);
        }

      if (QAbstractSocket::ConnectedState
          != control_socket_->state())
        {
          throw std::runtime_error {
              "Native FLEX TCP closed while draining status."
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

          capture_smart_sdr_client(line);
          capture_owned_slice(line);
          capture_dax_tx_stream(line);
          capture_transmit_status(line);
          capture_gain_status(line);
        }
    }
}

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

          capture_smart_sdr_client(line);
          capture_owned_slice(line);
          capture_dax_tx_stream(line);
          capture_transmit_status(line);
          capture_gain_status(line);
        }
    }

  if (slice_id_ < 0)
    {
      throw std::runtime_error {
          smart_sdr_present_
              ? "Native FLEX reported no new W7PP SmartSDR coexistence slice."
              : "Native FLEX reported no WSJT-owned slice."
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

          capture_smart_sdr_client(line);
          capture_owned_slice(line);
          capture_dax_tx_stream(line);
          capture_transmit_status(line);
          capture_gain_status(line);
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

          capture_smart_sdr_client(line);
          capture_owned_slice(line);
          capture_dax_tx_stream(line);
          capture_transmit_status(line);
          capture_gain_status(line);

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
  publish_native_flex_tx_route(QString {}, 0);
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
  smart_sdr_client_id_.clear();
  cached_tx_rf_power_level_ = -1;
  smart_sdr_present_ = false;
  collecting_clients_ = false;
  collecting_existing_slices_ = false;
  creating_slice_ = false;
  existing_slice_ids_.clear();
  existing_panadapter_ids_.clear();

  client_handle_ = 0;
  have_client_handle_ = false;

  next_sequence_ = 1;
  slice_id_ = -1;
  previous_tx_slice_id_ = -1;
  slice_panadapter_id_ = 0;
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
   * SmartSDR coexistence decision.
   *
   * First inspect the clients already attached to the radio.
   *
   * If SmartSDR-Win is present, W7PP remains a non-GUI API
   * client.  It snapshots all existing slices, creates one
   * additional slice, and captures only that new slice number.
   *
   * If SmartSDR-Win is absent, preserve the accepted headless
   * client-gui ownership path.
   */
  collecting_clients_ = true;

  try
    {
      send_command(
          QString {
              "sub client all"
          });

      /*
       * DEVIATION from W7PP: let the client dump arrive before
       * deciding.
       *
       * The donor reads smart_sdr_present_ the instant send_command()
       * returns, i.e. on the "R<seq>|" line. If the radio trails its
       * client status lines after that response, SmartSDR goes
       * undetected and the headless "client gui" path runs against a
       * radio that has SmartSDR up. Drain first (see
       * drain_control_lines()).
       */
      drain_control_lines(750);
    }
  catch (...)
    {
      collecting_clients_ = false;
      throw;
    }

  collecting_clients_ = false;

  if (smart_sdr_present_)
    {
      if (smart_sdr_client_id_.isEmpty())
        {
          throw std::runtime_error {
              "Native FLEX found SmartSDR but received no SmartSDR client_id."
          };
        }

      send_command(
          QString {
              "client bind client_id=%1"
          }
          .arg(smart_sdr_client_id_));

      existing_slice_ids_.clear();
      collecting_existing_slices_ = true;

      try
        {
          send_command(
              QString {
                  "sub slice all"
              });

          /*
           * DEVIATION from W7PP: let the slice dump arrive before
           * closing the snapshot.
           *
           * Same response-ordering race as above, and worse here: if
           * existing_slice_ids_ is still empty when creating_slice_
           * opens, the first PRE-EXISTING slice status consumed inside
           * the create window is mistaken for W7PP's new slice - and
           * do_stop() then destroys SmartSDR's slice with "slice r".
           */
          drain_control_lines(750);
        }
      catch (...)
        {
          collecting_existing_slices_ = false;
          throw;
        }

      collecting_existing_slices_ = false;
      creating_slice_ = true;

      try
        {
          send_command(
              QString {
                  "slice create mode=digu"
              });

          /*
           * DEVIATION from W7PP: the donor re-issues "sub slice all"
           * once if the create response beat the slice status. This
           * fork's wait_for_owned_slice() already waits on the status
           * stream for that race (see its comment), so use it here
           * too; the create gate stays open for the whole wait.
           */
          wait_for_owned_slice();
        }
      catch (...)
        {
          creating_slice_ = false;
          throw;
        }

      creating_slice_ = false;
    }
  else
    {
      /*
       * Headless Native FLEX ownership.
       *
       * Register WSJT-X itself as a FLEX GUI/API client.
       */
      gui_client_id_ =
          QString::fromLatin1(
              send_command(
                  QString {
                      "client gui"
                  }))
              .trimmed();

      if (gui_client_id_.isEmpty())
        {
          control_socket_->abort();

          throw std::runtime_error {
              "Native FLEX client gui returned no client_id."
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
          send_command(
              QString {
                  "slice create mode=digu"
              });
        }

      wait_for_owned_slice();
    }

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
  if (!smart_sdr_present_)
    {
      send_command(
          QString {
              "slice s %1 tx=1 mode=digu"
          }
          .arg(slice_id_));
    }

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
   * W7PP SmartSDR coexistence:
   *
   * The DAX-TX stream above is owned by THIS Native FLEX
   * API client even when its slice is bound to SmartSDR's
   * GUI client. Explicitly select this exact stream as
   * the radio TX sample source.
   *
   * This changes only FLEX stream attachment/state.
   * Native VITA packet/sample transport is unchanged.
   */
  send_command(
      QStringLiteral("stream set 0x%1 tx=1")
          .arg(dax_tx_stream_id_, 0, 16));
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
  publish_native_flex_tx_route(radio.address, dax_tx_stream_id_);

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
   *
   * DEVIATION from W7PP: in coexistence, unkey only if WSJT is the
   * one keyed.
   *
   * "xmit" is radio-global, not per-client: a bound client's "xmit 0"
   * cuts whatever the SmartSDR operator is transmitting. The donor's
   * unconditional form therefore drops the other operator's carrier
   * every time WSJT-Z exits, restarts the rig, or fails a start after
   * "client bind". state().ptt() is set true only by the key-up path
   * in do_ptt(), so it marks the shutdowns that have something of our
   * own to unkey. Headless is unchanged: there nobody else can be on
   * the air, and the unconditional unkey is the safer default.
   */
  if (
      control_socket_
      && QAbstractSocket::ConnectedState
          == control_socket_->state()
      && (!smart_sdr_present_ || state().ptt()))
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

  /*
   * W7PP SmartSDR coexistence cleanup.
   *
   * Remove only the DAX-TX stream created by THIS
   * Native FLEX session while its control socket is
   * still connected.  The existing slice cleanup below
   * remains authoritative for the W7PP-owned slice.
   *
   * Native VITA RX/TX transport is not changed here.
   */
  if (
      dax_tx_stream_id_ != 0
      && control_socket_
      && QAbstractSocket::ConnectedState
          == control_socket_->state())
    {
      try
        {
          send_command(
              QStringLiteral("stream remove 0x%1")
                  .arg(dax_tx_stream_id_, 0, 16));
        }
      catch (...)
        {
          // Best effort cleanup. Shutdown still proceeds.
        }
    }

  dax_tx_stream_id_ = 0;

  update_PTT(false);
  safety_monitor_.stop();
  /*
   * W7PP 
   *
   * This Native FLEX session no longer owns a TX route.
   */
  publish_native_flex_tx_route(QString {}, 0);
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

  /*
   * SmartSDR coexistence:
   *
   * W7PP temporarily made its own slice the radio TX slice.
   * If a different TX slice existed before W7PP started,
   * restore that exact slice before disconnecting.
   *
   * This restores pre-W7PP radio state only.  It does not
   * arbitrate TX ownership or key the transmitter.
   */
  if (
      smart_sdr_present_
      && previous_tx_slice_id_ >= 0
      && control_socket_
      && QAbstractSocket::ConnectedState
          == control_socket_->state())
    {
      try
        {
          send_command(
              QString {
                  "slice s %1 tx=1"
              }
              .arg(previous_tx_slice_id_));
        }
      catch (...)
        {
          // Best effort state restoration. Shutdown continues.
        }
    }

  previous_tx_slice_id_ = -1;

  /*
   * SmartSDR can retain the extra panadapter after W7PP's
   * coexistence slice is removed.  Remove only the panadapter
   * captured from W7PP's exact slice.
   *
   * DEVIATION from W7PP: never remove a panadapter that already
   * existed when the slice snapshot was taken. FLEX can attach the
   * new coexistence slice to a pre-existing pan instead of creating
   * one - on a 2-slice/2-pan radio with SmartSDR holding both, it
   * must - and removing that one deletes the SmartSDR operator's
   * display.
   */
  if (
      smart_sdr_present_
      && slice_panadapter_id_ != 0
      && !existing_panadapter_ids_.contains(slice_panadapter_id_)
      && control_socket_
      && QAbstractSocket::ConnectedState
          == control_socket_->state())
    {
      try
        {
          send_command(
              QString {
                  "display pan remove 0x%1"
              }
              .arg(
                  slice_panadapter_id_,
                  0,
                  16));
        }
      catch (...)
        {
          // Best effort cleanup. Socket shutdown still proceeds.
        }
    }

  slice_panadapter_id_ = 0;

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
  smart_sdr_client_id_.clear();
  cached_tx_rf_power_level_ = -1;
  smart_sdr_present_ = false;
  collecting_clients_ = false;
  collecting_existing_slices_ = false;
  creating_slice_ = false;
  existing_slice_ids_.clear();
  existing_panadapter_ids_.clear();

  client_handle_ = 0;
  have_client_handle_ = false;

  next_sequence_ = 1;
  slice_id_ = -1;
  previous_tx_slice_id_ = -1;
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
          if (smart_sdr_present_)
            {
              send_command(
                  QString {
                      "slice s %1 tx=1 mode=digu"
                  }
                  .arg(slice_id_));

              if (cached_tx_rf_power_level_ >= 0)
                {
                  send_command(
                      QStringLiteral(
                          "transmit set rfpower=%1")
                          .arg(cached_tx_rf_power_level_));
                }
            }

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

          if (smart_sdr_present_ && previous_tx_slice_id_ >= 0)
            {
              try { send_command(QString {"slice s %1 tx=1"}.arg(previous_tx_slice_id_)); }
              catch (...) {}
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
   *
   * DEVIATION from W7PP: in coexistence, unkey only when this un-key
   * actually follows a key-up - the same conservatism as the TX-slice
   * restore below.
   *
   * "xmit" is radio-global, so a bound client's "xmit 0" cuts whatever
   * the SmartSDR operator is transmitting. TransceiverBase::shutdown()
   * calls do_ptt(false) unconditionally whenever the rig is online,
   * keyed or not, so the donor's unguarded form takes the other
   * operator off the air on every WSJT-Z exit. A session that never
   * keyed has nothing of its own to unkey. Headless is unchanged.
   */
  if (!smart_sdr_present_ || state().ptt())
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
          update_PTT(false);
          throw;
        }
    }

  /*
   * DEVIATION from W7PP: restore only when this un-key actually
   * follows a key-up.
   *
   * TransceiverBase::shutdown() calls do_ptt(false) unconditionally
   * whenever the rig is online, keyed or not. A session that never
   * keyed never took SmartSDR's TX slice, so the donor's unguarded
   * form writes "slice s <n> tx=1" to a radio whose TX slice this
   * session never touched: a mutation of somebody else's state with
   * nothing to undo. state().ptt() is set true only by the key-up
   * path above, so it marks the un-keys that have something to give
   * back. When the shutdown does happen while keyed, the restore
   * still runs here and do_stop() repeats it - restoring a TX slice
   * that is already the TX slice is a no-op.
   */
  if (state().ptt() && smart_sdr_present_ && previous_tx_slice_id_ >= 0)
    {
      try { send_command(QString {"slice s %1 tx=1"}.arg(previous_tx_slice_id_)); }
      catch (...) {}
    }

  update_PTT(false);
}
