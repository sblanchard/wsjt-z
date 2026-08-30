// -*- Mode: C++ -*-
#ifndef SOUNDOUT_H__
#define SOUNDOUT_H__

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QQueue>
#include <QElapsedTimer>
#include <QAudioOutput>
#include <QAudioDeviceInfo>

class QIODevice;
class QTimer;
class QUdpSocket;
class QAudioDeviceInfo;

// An instance of this sends audio data to a specified soundcard.

class SoundOutput
  : public QObject
{
  Q_OBJECT;
  
public:
  SoundOutput ()
    : m_framesBuffered {0}
    , m_volume {1.0}
    , error_ {false}
    , m_native_flex_source {nullptr}
    , m_native_flex_timer {nullptr}
    , m_native_flex_bytes_per_frame {0}
  {
  }

  qreal attenuation () const;

public Q_SLOTS:
  void setFormat (QAudioDeviceInfo const& device, unsigned channels, int frames_buffered = 0);
  void restart (QIODevice *);
  void suspend ();
  void resume ();
  void reset ();
  void stop ();
  void setAttenuation (qreal);	/* unsigned */
  void resetAttenuation ();	/* to zero */
  
Q_SIGNALS:
  void error (QString message) const;
  void status (QString message) const;

private:
  bool checkStream () const;

private Q_SLOTS:
  void handleStateChanged (QAudio::State);
  void nativeFlexPump ();
  void nativeFlexTxPace ();
  void nativeFlexWriteVitaPacket ();
  void nativeFlexCloseVitaDump ();

private:
  QAudioDeviceInfo m_device;
  unsigned m_channels;
  QScopedPointer<QAudioOutput> m_stream;
  int m_framesBuffered;
  qreal m_volume;
  bool error_;

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
};

#endif
