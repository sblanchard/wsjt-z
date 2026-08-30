#include "soundout.h"
#include <QFile>
#include <QUdpSocket>
#include <QHostAddress>
#include <cstring>

#include <QDateTime>
#include <QCoreApplication>
#include <QTimer>
#include <QVariant>
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#include <QSysInfo>
#include <qmath.h>
#include <QDebug>

#include "Logger.hpp"
#include "Audio/AudioDevice.hpp"

#include "moc_soundout.cpp"

bool SoundOutput::checkStream () const
{
  bool result {false};

  Q_ASSERT_X (m_stream, "SoundOutput", "programming error");
  if (m_stream) {
    switch (m_stream->error ())
      {
      case QAudio::OpenError:
        Q_EMIT error (tr ("An error opening the audio output device has occurred."));
        break;

      case QAudio::IOError:
        Q_EMIT error (tr ("An error occurred during write to the audio output device."));
        break;

      case QAudio::UnderrunError:
        Q_EMIT error (tr ("Audio data not being fed to the audio output device fast enough."));
        break;

      case QAudio::FatalError:
        Q_EMIT error (tr ("Non-recoverable error, audio output device not usable at this time."));
        break;

      case QAudio::NoError:
        result = true;
        break;
      }
  }
  return result;
}

static QFile w7pp_native_flex_vita_dump_file;

void SoundOutput::setFormat (QAudioDeviceInfo const& device, unsigned channels, int frames_buffered)
{
  Q_ASSERT (0 < channels && channels < 3);
  m_device = device;
  m_channels = channels;
  m_framesBuffered = frames_buffered;
}

void SoundOutput::restart (QIODevice * source)
{
  // W7PP :
  // Exact Native FLEX operation uses a software 48-kHz
  // consumer instead of a Windows audio output device.
  bool const native_flex =
      QCoreApplication::instance()
      && QCoreApplication::instance()
             ->property("W7PPNativeFlexTxCapture")
             .toBool();

  if (native_flex)
    {
      AudioDevice * audio_source =
          dynamic_cast<AudioDevice *> (source);

      if (!audio_source)
        {
          Q_EMIT error (tr ("Native FLEX TX source is not an AudioDevice."));

          // DEVIATION from W7PP: the donor left m_native_flex_source
          // untouched on this bail, unlike its three sibling error paths
          // below which all null it explicitly. A stale non-null
          // m_native_flex_source is exactly the condition that lets a
          // later resume() restart the pump against a dead source (see
          // the DEVIATION note in resume () below), so null it here too
          // for consistency.
          m_native_flex_source = nullptr;
          return;
        }

      // DEVIATION from W7PP: nativeFlexPump () copies only the first
      // qint16 of every output frame (see below), but
      // AudioDevice::load () puts the sample in the *second* slot for
      // Channel::Right -- so a Right output-channel selection would
      // silently transmit an unmodulated carrier with no error and no
      // warning. Refuse to start instead, mirroring the mono-only check
      // guarding the offline VITA dump path further below.
      if (AudioDevice::Right == audio_source->channel ())
        {
          Q_EMIT error (
              tr ("Native FLEX TX requires the output channel to be Mono, Left or Both; select one of those instead of Right."));

          m_native_flex_source = nullptr;
          return;
        }

      if (m_native_flex_timer)
        {
          m_native_flex_timer->stop ();
        }

      m_native_flex_source = source;
      m_native_flex_bytes_per_frame =
          static_cast<qint64> (audio_source->bytesPerFrame ());

      /*
       * W7PP 
       *
       * Native FLEX only.
       *
       * Read the TX route published by NativeFlexTransceiver.
       * This step only retains the values for later transport.
       * It creates NO UDP socket and sends NO packet.
       */
      m_native_flex_tx_radio_address.clear ();
      m_native_flex_tx_stream_id = 0;

      if (QCoreApplication::instance ())
        {
          QString const tx_radio_address =
              QCoreApplication::instance ()
                  ->property (
                      "W7PPNativeFlexTxRadioAddress")
                  .toString ()
                  .trimmed ();

          bool stream_ok {false};

          qulonglong const tx_stream_id =
              QCoreApplication::instance ()
                  ->property (
                      "W7PPNativeFlexTxStreamId")
                  .toULongLong (&stream_ok);

          if (!tx_radio_address.isEmpty ()
              && stream_ok
              && tx_stream_id > 0
              && tx_stream_id <= 0xffffffffULL)
            {
              m_native_flex_tx_radio_address =
                  tx_radio_address.toLatin1 ();

              m_native_flex_tx_stream_id =
                  static_cast<quint32> (
                      tx_stream_id);
            }
        }
      // W7PP :
      // Initialize optional OFFLINE Native FLEX VITA dump.
      //
      // This only opens a file.  Audio is NOT packetized here.
      nativeFlexCloseVitaDump ();

      m_native_flex_vita_payload.clear ();
      m_native_flex_vita_sequence = 0;
      m_native_flex_decimate_phase = 0;

      // Reference stream ID from the captured SmartSDR/DAX
      // stream.  This value is only for offline comparison.
      m_native_flex_vita_dump_stream_id = 0x84000000u;

      QByteArray const vita_dump_name =
          qgetenv ("W7PP_NATIVE_FLEX_TX_VITA_DUMP_FILE");

      QByteArray const vita_stream_text =
          qgetenv ("W7PP_NATIVE_FLEX_TX_VITA_DUMP_STREAM_ID");

      if (!vita_stream_text.isEmpty ())
        {
          bool stream_ok {false};

          quint32 const stream_id =
              vita_stream_text.toUInt (
                  &stream_ok,
                  0);

          if (!stream_ok || stream_id == 0)
            {
              Q_EMIT error (
                  tr ("Invalid Native FLEX offline VITA stream ID."));

              m_native_flex_source = nullptr;
              return;
            }

          m_native_flex_vita_dump_stream_id =
              stream_id;
        }

      if (!vita_dump_name.isEmpty ())
        {
          if (m_native_flex_bytes_per_frame
              != static_cast<qint64> (sizeof (qint16)))
            {
              Q_EMIT error (
                  tr ("Native FLEX offline VITA dump requires mono qint16 input."));

              m_native_flex_source = nullptr;
              return;
            }

          w7pp_native_flex_vita_dump_file.setFileName (
              QString::fromLocal8Bit (
                  vita_dump_name));

          if (!w7pp_native_flex_vita_dump_file.open (
                  QIODevice::WriteOnly
                  | QIODevice::Truncate))
            {
              Q_EMIT error (
                  tr ("Cannot open Native FLEX offline VITA dump file."));

              m_native_flex_source = nullptr;
              return;
            }
        }
      // 10 ms at 48 kHz = 480 frames.
      m_native_flex_buffer.resize (
          static_cast<int> (480 * m_native_flex_bytes_per_frame));

      if (!m_native_flex_timer)
        {
          m_native_flex_timer = new QTimer {this};
          m_native_flex_timer->setTimerType (Qt::PreciseTimer);
          m_native_flex_timer->setInterval (10);

          connect (
              m_native_flex_timer,
              &QTimer::timeout,
              this,
              &SoundOutput::nativeFlexPump);
        }

      /*
       * W7PP 
       *
       * Independent Native FLEX DAX-TX wire pacer.
       *
       * The existing 10-ms timer remains the 48-kHz
       * Modulator/audio-source pull clock.
       *
       * The separate 1-ms precise timer supports the
       * monotonic 5333/5333/5334-us packet schedule when pacing is used.
       */
      m_native_flex_tx_packets.clear ();
      m_native_flex_tx_elapsed.invalidate ();
      m_native_flex_tx_next_packet_us = 0;
      m_native_flex_tx_pace_phase = 0;

      /*
       * W7PP 
       *
       * UDP transport object for Native FLEX DAX-TX.
       * Creating QUdpSocket alone does not transmit anything.
       */
      if (!m_native_flex_tx_socket)
        {
          m_native_flex_tx_socket =
              new QUdpSocket {this};
        }
      if (!m_native_flex_tx_timer)
        {
          m_native_flex_tx_timer = new QTimer {this};
          m_native_flex_tx_timer->setTimerType (
              Qt::PreciseTimer);
          m_native_flex_tx_timer->setInterval (1);

          connect (
              m_native_flex_tx_timer,
              &QTimer::timeout,
              this,
              &SoundOutput::nativeFlexTxPace);
        }
      error_ = false;

      /*
       * W7PP 
       *
       * Start the independent DAX-TX wire pacer only when
       * the Native FLEX backend supplied a complete live route.
       *
       * This starts UDP AUDIO pacing only.
       * It does NOT key the radio and sends no xmit command.
       */
      if (m_native_flex_tx_timer
          && m_native_flex_tx_socket
          && !m_native_flex_tx_radio_address.isEmpty ()
          && m_native_flex_tx_stream_id != 0)
        {
          m_native_flex_tx_packets.clear ();
          m_native_flex_tx_elapsed.invalidate ();
          m_native_flex_tx_next_packet_us = 0;
          m_native_flex_tx_pace_phase = 0;
          /* Direct packet send is used; the separate TX pacer remains dormant. */
        }

      m_native_flex_timer->start ();

      // QAudioOutput normally pulls immediately after start().
      // Do the same for the software consumer.
      nativeFlexPump ();
      Q_EMIT status (tr ("Sending"));
      return;
    }

  // DEVIATION from W7PP: the donor never cleared Native FLEX TX state
  // on a rig change away from "Flex Native VITA-49"; this replaces that
  // absent cleanup. A rig change always goes through restart() (the
  // property above is recomputed fresh on every call), so this is the
  // correct single place to drop stale flex routing state -- not
  // suspend()/resume(), whose semantics must stay about pausing and
  // resuming an in-progress Native FLEX session, not about rig
  // switching.
  //
  // m_native_flex_timer is not stopped here: it is unconditionally
  // stopped by SoundOutput::stop()/reset() whenever the previous TX
  // was still active at the top of this call (Modulator::start()
  // calls stop() when m_state != Idle before ever reaching this
  // restart()), and otherwise it self-stops inside nativeFlexPump()
  // within one 10 ms tick of the previous session reaching Idle --
  // always well before a human-timescale rig change and the next
  // restart() call. m_native_flex_tx_timer is never started anywhere
  // in this file (the pacer stays dormant by design; direct send is
  // used instead), so it can never be active here. Neither timer
  // needs a defensive stop.
  m_native_flex_source = nullptr;
  m_native_flex_tx_radio_address.clear ();
  m_native_flex_tx_stream_id = 0;

  if (!m_device.isNull ())
    {
      QAudioFormat format (m_device.preferredFormat ());
      //  qDebug () << "Preferred audio output format:" << format;
      format.setChannelCount (m_channels);
      format.setCodec ("audio/pcm");
      format.setSampleRate (48000);
      format.setSampleType (QAudioFormat::SignedInt);
      format.setSampleSize (16);
      format.setByteOrder (QAudioFormat::Endian (QSysInfo::ByteOrder));
      if (!format.isValid ())
        {
          Q_EMIT error (tr ("Requested output audio format is not valid."));
        }
      else if (!m_device.isFormatSupported (format))
        {
          Q_EMIT error (tr ("Requested output audio format is not supported on device."));
        }
      else
        {
          // qDebug () << "Selected audio output format:" << format;
          m_stream.reset (new QAudioOutput (m_device, format));
          checkStream ();
          m_stream->setVolume (m_volume);
          m_stream->setNotifyInterval(1000);
          error_ = false;

          connect (m_stream.data(), &QAudioOutput::stateChanged, this, &SoundOutput::handleStateChanged);
          connect (m_stream.data(), &QAudioOutput::notify, [this] () {checkStream ();});

          //      qDebug() << "A" << m_volume << m_stream->notifyInterval();
        }
    }
  if (!m_stream)
    {
      if (!error_)
        {
          error_ = true;        // only signal error once
          Q_EMIT error (tr ("No audio output device configured."));
        }
      return;
    }
  else
    {
      error_ = false;
    }

  // we have to set this before every start on the stream because the
  // Windows implementation seems to forget the buffer size after a
  // stop.
  //qDebug () << "SoundOut default buffer size (bytes):" << m_stream->bufferSize () << "period size:" << m_stream->periodSize ();
  if (m_framesBuffered > 0)
    {
      m_stream->setBufferSize (m_stream->format().bytesForFrames (m_framesBuffered));
    }
  m_stream->setCategory ("production");
  m_stream->start (source);
//  LOG_DEBUG ("Selected buffer size (bytes): " << m_stream->bufferSize () << " period size: " << m_stream->periodSize ());
}

void SoundOutput::nativeFlexWriteVitaPacket ()
{
  if (m_native_flex_vita_payload.size () != 256)
    {
      return;
    }

  QByteArray packet;
  packet.reserve (284);

  auto append_word =
      [&packet] (quint32 word)
      {
        packet.append (
            static_cast<char> ((word >> 24) & 0xffu));

        packet.append (
            static_cast<char> ((word >> 16) & 0xffu));

        packet.append (
            static_cast<char> ((word >> 8) & 0xffu));

        packet.append (
            static_cast<char> (word & 0xffu));
      };

  // FLEX reduced-bandwidth DAX-TX VITA header:
  //
  //   0x18D?0047
  //
  // TSI=3 and TSF=1 remain fixed in bits 20-23.
  // Packet-count nibble occupies bits 16-19, modulo 16.
  quint32 const header =
      0x18d00047u
      | ((static_cast<quint32> (
              m_native_flex_vita_sequence) & 0x0fu) << 16);

  append_word (header);

  /*
   * W7PP 
   *
   * Construct the proven reference form first.
   *
   * Offline comparison keeps the captured reference stream ID.
   * A queued live copy receives this Native FLEX session's
   * dynamically learned DAX-TX stream ID below.
   */
  append_word (
      m_native_flex_vita_dump_stream_id);

  append_word (0x00001c2du);
  append_word (0x534c0123u);

  // Integer timestamp.
  append_word (0u);

  // Fractional timestamp.
  append_word (0u);
  append_word (0u);

  packet.append (
      m_native_flex_vita_payload);

  if (packet.size () != 284)
    {
      Q_EMIT error (
          tr ("Native FLEX VITA packet construction failed."));

      m_native_flex_vita_payload.clear ();
      return;
    }

  /*
   * Optional offline reference capture.
   */
  if (w7pp_native_flex_vita_dump_file.isOpen ())
    {
      if (w7pp_native_flex_vita_dump_file.write (packet)
          != packet.size ())
        {
          Q_EMIT error (
              tr ("Native FLEX offline VITA packet dump write failed."));

          w7pp_native_flex_vita_dump_file.close ();
        }
    }

  /*
   * Native FLEX live queue.
   *
   * No UDP transmission occurs here.
   */
  if (!m_native_flex_tx_radio_address.isEmpty ()
      && m_native_flex_tx_stream_id != 0)
    {
      QByteArray live_packet {packet};

      quint32 const stream_id =
          m_native_flex_tx_stream_id;

      live_packet[4] =
          static_cast<char> (
              (stream_id >> 24) & 0xffu);

      live_packet[5] =
          static_cast<char> (
              (stream_id >> 16) & 0xffu);

      live_packet[6] =
          static_cast<char> (
              (stream_id >> 8) & 0xffu);

      live_packet[7] =
          static_cast<char> (
              stream_id & 0xffu);

      /*
       * W7PP 
       *
       * AetherSDR-style producer-driven DAX-TX:
       * when a complete 128-sample VITA packet exists,
       * send it immediately instead of placing it on an
       * independent wall-clock pacing queue.
       *
       * FLEX reduced-bandwidth DAX-TX destination:
       * UDP 4991.
       */
      if (!m_native_flex_tx_socket)
        {
          Q_EMIT error (
              tr ("Native FLEX TX UDP socket is unavailable."));

          // DEVIATION from W7PP: the donor returned here leaving
          // m_native_flex_vita_payload at exactly 256 bytes. The pump's
          // trigger is size()==256 (see nativeFlexPump () below), so
          // the next appended sample takes it to 258 and that
          // condition can never match again -- one transient error
          // would wedge the packetiser for the rest of the
          // transmission. Clear it so the stream resynchronises on the
          // next full 256-byte block.
          m_native_flex_vita_payload.clear ();
          return;
        }

      QHostAddress destination;

      if (!destination.setAddress (
              QString::fromLatin1 (
                  m_native_flex_tx_radio_address)))
        {
          Q_EMIT error (
              tr ("Native FLEX TX radio address is invalid."));

          // DEVIATION from W7PP: see the clear () note above -- same
          // wedge risk on this error path.
          m_native_flex_vita_payload.clear ();
          return;
        }

      qint64 const written =
          m_native_flex_tx_socket->writeDatagram (
              live_packet,
              destination,
              static_cast<quint16> (4991));

      if (written != live_packet.size ())
        {
          Q_EMIT error (
              tr ("Native FLEX TX UDP datagram send failed."));

          // DEVIATION from W7PP: see the clear () note above -- same
          // wedge risk on this error path (e.g. a transient
          // ENOBUFS/EWOULDBLOCK on a busy socket).
          m_native_flex_vita_payload.clear ();
          return;
        }
    }

  m_native_flex_vita_sequence =
      static_cast<quint8> (
          (m_native_flex_vita_sequence + 1u) & 0x0fu);

  m_native_flex_vita_payload.clear ();
}
void SoundOutput::nativeFlexCloseVitaDump ()
{
  if (!w7pp_native_flex_vita_dump_file.isOpen ())
    {
      m_native_flex_vita_payload.clear ();
      return;
    }

  if (!m_native_flex_vita_payload.isEmpty ())
    {
      int const missing =
          256 - m_native_flex_vita_payload.size ();

      if (missing > 0)
        {
          m_native_flex_vita_payload.append (
              QByteArray (missing, '\0'));
        }

      nativeFlexWriteVitaPacket ();
    }

  w7pp_native_flex_vita_dump_file.flush ();
  w7pp_native_flex_vita_dump_file.close ();

  m_native_flex_vita_payload.clear ();
}
void SoundOutput::nativeFlexTxPace ()
{
  /*
   * W7PP 
   *
   * 128 samples / 24000 Hz =
   * 5333.333333... microseconds per VITA packet.
   *
   * Integer schedule:
   *
   *   5333
   *   5333
   *   5334
   *
   * Exactly 16000 us for every three packets.
   *
   * This function sends AT MOST ONE packet per timer callback.
   */
  if (m_native_flex_tx_packets.isEmpty ())
    {
      return;
    }

  if (!m_native_flex_tx_socket)
    {
      Q_EMIT error (
          tr ("Native FLEX TX UDP socket is unavailable."));

      return;
    }

  if (!m_native_flex_tx_elapsed.isValid ())
    {
      m_native_flex_tx_elapsed.start ();
      m_native_flex_tx_next_packet_us = 0;
      m_native_flex_tx_pace_phase = 0;
    }

  qint64 const now_us =
      m_native_flex_tx_elapsed.nsecsElapsed () / 1000;

  if (now_us < m_native_flex_tx_next_packet_us)
    {
      return;
    }

  QByteArray const packet =
      m_native_flex_tx_packets.head ();

  if (packet.size () != 284)
    {
      Q_EMIT error (
          tr ("Native FLEX TX VITA packet has invalid size."));

      if (m_native_flex_tx_timer)
        {
          m_native_flex_tx_timer->stop ();
        }

      return;
    }

  QHostAddress destination;

  if (!destination.setAddress (
          QString::fromLatin1 (
              m_native_flex_tx_radio_address)))
    {
      Q_EMIT error (
          tr ("Native FLEX TX radio address is invalid."));

      if (m_native_flex_tx_timer)
        {
          m_native_flex_tx_timer->stop ();
        }

      return;
    }

  /*
   * FLEX DAX-TX destination captured from SmartSDR/DAX:
   *
   *   UDP destination port 4991
   *
   * No explicit local bind is used here. Windows may select
   * the local UDP source port; the FLEX destination and
   * dynamically learned VITA stream ID identify this TX path.
   */
  qint64 const written =
      m_native_flex_tx_socket->writeDatagram (
          packet,
          destination,
          static_cast<quint16> (4991));

  if (written != packet.size ())
    {
      Q_EMIT error (
          tr ("Native FLEX TX UDP datagram send failed."));

      if (m_native_flex_tx_timer)
        {
          m_native_flex_tx_timer->stop ();
        }

      return;
    }

  /*
   * Remove the packet only after the complete datagram was
   * supported by QUdpSocket.
   */
  m_native_flex_tx_packets.dequeue ();

  qint64 increment_us {5333};

  ++m_native_flex_tx_pace_phase;

  if (m_native_flex_tx_pace_phase >= 3)
    {
      increment_us = 5334;
      m_native_flex_tx_pace_phase = 0;
    }

  qint64 const scheduled_next =
      m_native_flex_tx_next_packet_us + increment_us;

  /*
   * Never burst packets to catch up after a late callback.
   * If we are already behind the next deadline, restart the
   * interval from the current monotonic time.
   */
  if (scheduled_next <= now_us)
    {
      m_native_flex_tx_next_packet_us =
          now_us + increment_us;
    }
  else
    {
      m_native_flex_tx_next_packet_us =
          scheduled_next;
    }
}
void SoundOutput::nativeFlexPump ()
{
  if (!m_native_flex_source
      || !m_native_flex_source->isOpen ()
      || m_native_flex_bytes_per_frame <= 0)
    {
      if (m_native_flex_timer)
        {
          m_native_flex_timer->stop ();
        }

      m_native_flex_source = nullptr;
      Q_EMIT status (tr ("Idle"));
      return;
    }

  qint64 const requested =
      480 * m_native_flex_bytes_per_frame;

  if (m_native_flex_buffer.size () != requested)
    {
      m_native_flex_buffer.resize (
          static_cast<int> (requested));
    }

  qint64 const received =
      m_native_flex_source->read (
          m_native_flex_buffer.data (),
          requested);

  // W7PP :
  // Feed the existing Native FLEX software audio clock into
  // the OFFLINE VITA packetizer only.
  //
  // Modulator source:
  //   48 kHz mono signed qint16
  //
  // Captured FLEX DAX-TX wire:
  //   24 kHz mono signed qint16 big-endian
  //
  // Match the stock SoundOutput Pwr behavior by applying
  // the existing m_volume attenuation before packetization.
  if (received > 0
      && (w7pp_native_flex_vita_dump_file.isOpen ()
          || (!m_native_flex_tx_radio_address.isEmpty ()
              && m_native_flex_tx_stream_id != 0)))
    {
      qint64 const frames =
          received / m_native_flex_bytes_per_frame;

      for (qint64 frame = 0;
           frame < frames;
           ++frame)
        {
          char const * sample_address =
              m_native_flex_buffer.constData ()
              + frame * m_native_flex_bytes_per_frame;

          qint16 sample {0};

          std::memcpy (
              &sample,
              sample_address,
              sizeof (sample));

          // Exact 2:1 rate conversion for this offline capture path:
          // retain every other 48-kHz input sample.
          if (m_native_flex_decimate_phase == 0)
            {
              int scaled =
                  qRound (
                      static_cast<qreal> (sample)
                      * m_volume);

              if (scaled > 32767)
                {
                  scaled = 32767;
                }

              if (scaled < -32768)
                {
                  scaled = -32768;
                }

              quint16 const wire_sample =
                  static_cast<quint16> (
                      static_cast<qint16> (scaled));

              // FLEX DAX-TX network PCM is big-endian.
              m_native_flex_vita_payload.append (
                  static_cast<char> (
                      (wire_sample >> 8) & 0xffu));

              m_native_flex_vita_payload.append (
                  static_cast<char> (
                      wire_sample & 0xffu));

              if (m_native_flex_vita_payload.size () == 256)
                {
                  nativeFlexWriteVitaPacket ();
                }
            }

          m_native_flex_decimate_phase ^= 1;
        }
    }
  if (received <= 0)
    {
      if (m_native_flex_timer)
        {
          m_native_flex_timer->stop ();
        }

      nativeFlexCloseVitaDump ();
      m_native_flex_source = nullptr;
      Q_EMIT status (tr ("Idle"));
    }
}

void SoundOutput::suspend ()
{
  // W7PP : stop wire pacing before suspending TX audio.
  if (m_native_flex_tx_timer)
    {
      m_native_flex_tx_timer->stop ();
    }

  m_native_flex_tx_packets.clear ();
  m_native_flex_tx_elapsed.invalidate ();
  m_native_flex_tx_next_packet_us = 0;
  m_native_flex_tx_pace_phase = 0;

  if (m_native_flex_timer
      && m_native_flex_timer->isActive ())
    {
      m_native_flex_timer->stop ();
      Q_EMIT status (tr ("Suspended"));
      return;
    }

  if (m_stream && QAudio::ActiveState == m_stream->state ())
    {
      m_stream->suspend ();
      checkStream ();
    }
}

void SoundOutput::resume ()
{
  // DEVIATION from W7PP: the donor's early-return guard below was
  // pointer-based only. Belt and braces: also require the live
  // W7PPNativeFlexTxCapture property, so that even if
  // m_native_flex_source/m_native_flex_timer were ever left non-null
  // while a non-Flex rig is selected, resume () still cannot restart
  // the Native FLEX pump. See the rig-change DEVIATION note in the
  // non-flex path of restart () above.
  bool const native_flex =
      QCoreApplication::instance()
      && QCoreApplication::instance()
             ->property("W7PPNativeFlexTxCapture")
             .toBool();

  if (native_flex
      && m_native_flex_source
      && m_native_flex_timer
      && !m_native_flex_timer->isActive ())
    {
      if (m_native_flex_tx_timer
          && m_native_flex_tx_socket
          && !m_native_flex_tx_radio_address.isEmpty ()
          && m_native_flex_tx_stream_id != 0)
        {
          m_native_flex_tx_packets.clear ();
          m_native_flex_tx_elapsed.invalidate ();
          m_native_flex_tx_next_packet_us = 0;
          m_native_flex_tx_pace_phase = 0;
          /* Direct packet send is used; the separate TX pacer remains dormant. */
        }

      m_native_flex_timer->start ();
      Q_EMIT status (tr ("Sending"));
      return;
    }

  if (m_stream && QAudio::SuspendedState == m_stream->state ())
    {
      m_stream->resume ();
      checkStream ();
    }
}

void SoundOutput::reset ()
{
  // W7PP : terminate Native FLEX wire pacing first.
  if (m_native_flex_tx_timer)
    {
      m_native_flex_tx_timer->stop ();
    }

  m_native_flex_tx_packets.clear ();
  m_native_flex_tx_elapsed.invalidate ();
  m_native_flex_tx_next_packet_us = 0;
  m_native_flex_tx_pace_phase = 0;
  if (m_native_flex_timer)
    {
      m_native_flex_timer->stop ();
    }

  m_native_flex_source = nullptr;

  if (m_stream)
    {
      m_stream->reset ();
      checkStream ();
    }
}

void SoundOutput::stop ()
{
  // W7PP : terminate Native FLEX wire pacing first.
  if (m_native_flex_tx_timer)
    {
      m_native_flex_tx_timer->stop ();
    }

  m_native_flex_tx_packets.clear ();
  m_native_flex_tx_elapsed.invalidate ();
  m_native_flex_tx_next_packet_us = 0;
  m_native_flex_tx_pace_phase = 0;
  if (m_native_flex_timer)
    {
      m_native_flex_timer->stop ();
    }

  m_native_flex_source = nullptr;

  if (m_stream)
    {
      m_stream->reset ();
      m_stream->stop ();
    }
}

qreal SoundOutput::attenuation () const
{
  return -(20. * qLn (m_volume) / qLn (10.));
}

void SoundOutput::setAttenuation (qreal a)
{
  Q_ASSERT (0. <= a && a <= 999.);
  m_volume = qPow(10.0, -a/20.0);
  //  qDebug () << "SoundOut: attn = " << a << ", vol = " << m_volume;
  if (m_stream)
    {
      m_stream->setVolume (m_volume);
    }
}

void SoundOutput::resetAttenuation ()
{
  m_volume = 1.;
  if (m_stream)
    {
      m_stream->setVolume (m_volume);
    }
}

void SoundOutput::handleStateChanged (QAudio::State newState)
{
  switch (newState)
    {
    case QAudio::IdleState:
      Q_EMIT status (tr ("Idle"));
      break;

    case QAudio::ActiveState:
      Q_EMIT status (tr ("Sending"));
      break;

    case QAudio::SuspendedState:
      Q_EMIT status (tr ("Suspended"));
      break;

#if QT_VERSION >= QT_VERSION_CHECK (5, 10, 0)
    case QAudio::InterruptedState:
      Q_EMIT status (tr ("Interrupted"));
      break;
#endif

    case QAudio::StoppedState:
      if (!checkStream ())
        {
          Q_EMIT status (tr ("Error"));
        }
      else
        {
          Q_EMIT status (tr ("Stopped"));
        }
      break;
    }
}
