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

  // Every slot uses this instead of touching FlexVitaReceiver::Configuration's
  // default UDP port range (4995-5010) directly: that range is the real
  // SmartSDR DAX range, so running this test on a machine with SmartSDR
  // installed, or running it in parallel with another instance of itself,
  // would otherwise fail with "No available UDP VITA port" for reasons
  // that have nothing to do with what the test is checking. The fake radio
  // in init() learns whichever port the receiver actually picked from the
  // `client udpport` command, so nothing else needs to change.
  FlexVitaReceiver::Configuration makeConfiguration (quint16 tcpPort)
  {
    FlexVitaReceiver::Configuration configuration;
    configuration.radioAddress = "127.0.0.1";
    configuration.tcpPort      = tcpPort;
    configuration.daxChannel   = FakeDaxChannel;
    configuration.firstUdpPort = 45995;
    configuration.lastUdpPort  = 46010;

    return configuration;
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
  void routes_a_slice_into_the_dax_channel ();
  void leaves_existing_dax_routing_alone ();
  void reasserts_routing_while_the_stream_is_unbound ();

private:
  bool waitForStreaming (FlexVitaReceiver&, int timeoutMs = 8000);

  QTcpServer *   server_ {nullptr};
  QTcpSocket *   client_ {nullptr};
  int            udpPort_ {0};

  // Fake radio slice state, replayed when the receiver subscribes.
  bool           announceSlice_ {false};
  int            sliceDaxChannel_ {0};

  // When true the fake radio reports our dax_rx stream with an empty
  // "slice=" field, i.e. attached to no slice - the state a real radio
  // reports when nothing routes a slice into the DAX channel.
  bool           streamUnbound_ {false};

  int sliceDaxCommandCount (int slice, int channel) const;

  QByteArrayList commands_;

  bool sentSliceDaxCommand (int slice, int channel) const;
};

int TestFlexVitaReceiver::sliceDaxCommandCount (int slice, int channel) const
{
  QByteArray const expected =
      "slice s " + QByteArray::number (slice) +
      " dax=" + QByteArray::number (channel);

  int count = 0;

  for (auto const& command : commands_)
    {
      if (command.contains (expected))
        ++count;
    }

  return count;
}

bool TestFlexVitaReceiver::sentSliceDaxCommand (int slice, int channel) const
{
  QByteArray const expected =
      "slice s " + QByteArray::number (slice) +
      " dax=" + QByteArray::number (channel);

  for (auto const& command : commands_)
    {
      if (command.contains (expected))
        return true;
    }

  return false;
}

void TestFlexVitaReceiver::init ()
{
  server_ = new QTcpServer {this};
  QVERIFY (server_->listen (QHostAddress::LocalHost, 0));

  client_  = nullptr;
  udpPort_ = 0;

  commands_.clear ();

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

              commands_.append (line);

              if (announceSlice_ && line.contains ("sub slice all"))
                {
                  client_->write ("S" +
                      QByteArray::number (FakeClientHandle, 16) +
                      "|slice 0 in_use=1 mode=DIGU dax=" +
                      QByteArray::number (sliceDaxChannel_) +
                      " RF_frequency=14.074000\n");
                }

              if (line.contains ("client udpport"))
                {
                  udpPort_ = line.mid (line.lastIndexOf (' ') + 1).toInt ();
                }
              else if (line.contains ("stream create")
                       && line.contains ("type=dax_rx"))
                {
                  // Step 3: report the stream we just "created".
                  //
                  // The 0x prefixes are load-bearing: the receiver parses
                  // these with std::stoul(text, nullptr, 0), so an unprefixed
                  // value is read as decimal and the stream is rejected.
                  QByteArray reply = "S" +
                      QByteArray::number (FakeClientHandle, 16) +
                      "|stream 0x" + QByteArray::number (FakeStreamId, 16) +
                      " type=dax_rx dax_channel=" +
                      QByteArray::number (FakeDaxChannel) +
                      (streamUnbound_ ? " slice= " : "") +
                      " client_handle=0x" +
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

  announceSlice_   = false;
  sliceDaxChannel_ = 0;
  streamUnbound_   = false;
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

  FlexVitaReceiver::Configuration const configuration =
      makeConfiguration (server_->serverPort ());

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
  // Declared before `receiver` so that they are destroyed *after* it.
  // FlexVitaReceiver's destructor joins the worker thread, which calls
  // the audio callback below by reference into these atomics. If the
  // atomics were declared after `receiver`, a QVERIFY/QCOMPARE failure
  // between here and receiver.stop() would `return` out of this function
  // early, unwinding in reverse declaration order: the atomics would be
  // destroyed first, while the worker thread was still alive and still
  // writing through references to that reclaimed stack space.
  std::atomic<int> delivered {0};
  std::atomic<int> nonZero   {0};

  FlexVitaReceiver receiver;

  receiver.setAudioCallback (
      [&delivered, &nonZero] (FlexVitaReceiver::DecoderBlock const& block)
      {
        delivered.fetch_add (static_cast<int> (block.size ()));

        for (std::int16_t const sample : block)
          if (0 != sample)
            nonZero.fetch_add (1);
      });

  FlexVitaReceiver::Configuration const configuration =
      makeConfiguration (server_->serverPort ());

  QVERIFY (receiver.start (configuration));
  QVERIFY (waitForStreaming (receiver));

  // A 1 kHz tone at 48 kHz, well below the 6 kHz Nyquist of the decimated
  // stream, so the anti-alias FIR should pass it through.
  QUdpSocket sender;

  int const samplesPerPacket = 256;
  int const packetCount      = 40;      // 10240 input samples, 4:1 decimated
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

  // 40 packets * 256 samples = 10240 input samples at 48 kHz. 4:1
  // decimation yields exactly 10240 / 4 = 2560 decoded samples, delivered
  // in complete 1200-sample blocks: exactly two of them (2400), with the
  // remaining 160 samples held back (a partial final block is never
  // emitted) until receiver.stop() discards them.
  //
  // Sending is asynchronous with respect to the receiver's worker thread:
  // by the time all 40 packets have been handed to the kernel, the worker
  // may only have drained and decoded a handful of them so far. Waiting
  // only for the first block (`delivered >= 1200`) would race ahead and
  // read the statistics after just ~19 of the 40 packets were processed,
  // making this test fail deterministically rather than intermittently.
  // So wait for both: all expected audio delivered, AND every packet that
  // was actually sent accounted for by the receiver.
  int const expectedDelivered = 2400;

  QDeadlineTimer deadline {5000};

  while (!deadline.hasExpired ()
         && (delivered.load () < expectedDelivered
             || receiver.statistics ().vitaPackets
                    < static_cast<quint64> (packetCount)))
    QCoreApplication::processEvents (QEventLoop::AllEvents, 20);

  auto const statistics = receiver.statistics ();

  QVERIFY2 (statistics.vitaPackets >= 30,
            qPrintable (QString ("only %1 VITA packets accepted")
                        .arg (statistics.vitaPackets)));
  QCOMPARE (statistics.malformedPackets, quint64 (0));
  QCOMPARE (statistics.invalidFloats,    quint64 (0));

  // Exact count, not just a lower bound: a receiver that decimated at the
  // wrong ratio (say 2:1, or not at all) would still clear `>= 1200` and
  // `% 1200 == 0` (deliverDecoderSample only ever emits whole 1200-sample
  // blocks), so neither of those actually verifies the 4:1 conversion this
  // test is named for. The exact figure does.
  QCOMPARE (delivered.load (), expectedDelivered);

  QVERIFY2 (nonZero.load () > delivered.load () / 4,
            "decimated tone should be mostly non-zero");

  receiver.stop ();
}

void TestFlexVitaReceiver::rejects_foreign_stream_id ()
{
  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration const configuration =
      makeConfiguration (server_->serverPort ());

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
  {
    auto const statistics = receiver.statistics ();
    QCOMPARE (statistics.vitaPackets,      quint64 (0));
    QCOMPARE (statistics.malformedPackets, quint64 (0));
  }

  // Positive control: without this, the zero counts above would also
  // pass vacuously if the datagrams never reached the socket at all (for
  // instance a wrong port, or a bind collision). Sending one genuine
  // packet on the correct stream and requiring it to be accepted proves
  // datagrams really do arrive here, so the zeros above mean "rejected",
  // not "never delivered".
  QByteArray const validPacket =
      buildVitaPacket (1, std::vector<float> (64, 0.25f));

  sender.writeDatagram (validPacket, QHostAddress::LocalHost,
                        static_cast<quint16> (udpPort_));

  QDeadlineTimer deadline {2000};

  while (!deadline.hasExpired ()
         && receiver.statistics ().vitaPackets < 1)
    QCoreApplication::processEvents (QEventLoop::AllEvents, 20);

  QVERIFY2 (receiver.statistics ().vitaPackets >= 1,
            "a genuine packet on the correct stream must be accepted");

  receiver.stop ();
}

void TestFlexVitaReceiver::counts_malformed_packets ()
{
  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration const configuration =
      makeConfiguration (server_->serverPort ());

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

  FlexVitaReceiver::Configuration configuration =
      makeConfiguration (server_->serverPort ());
  configuration.radioAddress = "not-an-ip-address";

  receiver.start (configuration);

  QDeadlineTimer deadline {3000};

  while (!deadline.hasExpired () && receiver.lastError ().empty ())
    QCoreApplication::processEvents (QEventLoop::AllEvents, 20);

  QVERIFY2 (!receiver.lastError ().empty (),
            "an invalid IPv4 address must produce an error");
  QVERIFY (!receiver.streaming ());

  receiver.stop ();
}


//
// An operator who never opens the SmartSDR DAX panel leaves every slice
// routed to DAX channel 0, so the radio sends VITA-49 packets carrying
// silence. The receiver must route a slice into its own channel.
//
void TestFlexVitaReceiver::routes_a_slice_into_the_dax_channel ()
{
  announceSlice_   = true;
  sliceDaxChannel_ = 0;

  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration const configuration =
      makeConfiguration (server_->serverPort ());

  QVERIFY2 (receiver.start (configuration),
            qPrintable (QString::fromStdString (receiver.lastError ())));

  QVERIFY2 (waitForStreaming (receiver),
            qPrintable (QString::fromStdString (receiver.lastError ())));

  QDeadlineTimer deadline {3000};

  while (!deadline.hasExpired ()
         && !sentSliceDaxCommand (0, FakeDaxChannel))
    {
      QCoreApplication::processEvents (QEventLoop::AllEvents, 20);
    }

  QVERIFY2 (sentSliceDaxCommand (0, FakeDaxChannel),
            qPrintable ("commands seen: " +
                        QString::fromLatin1 (commands_.join (" / "))));

  receiver.stop ();
}

//
// Routing the operator already has - set by hand in SmartSDR, or by the
// Native FLEX CAT backend binding its own slice - must be left alone.
//
void TestFlexVitaReceiver::leaves_existing_dax_routing_alone ()
{
  announceSlice_   = true;
  sliceDaxChannel_ = FakeDaxChannel;

  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration const configuration =
      makeConfiguration (server_->serverPort ());

  QVERIFY2 (receiver.start (configuration),
            qPrintable (QString::fromStdString (receiver.lastError ())));

  QVERIFY2 (waitForStreaming (receiver),
            qPrintable (QString::fromStdString (receiver.lastError ())));

  QDeadlineTimer settle {1500};

  while (!settle.hasExpired ())
    QCoreApplication::processEvents (QEventLoop::AllEvents, 20);

  QVERIFY (!sentSliceDaxCommand (0, FakeDaxChannel));

  receiver.stop ();
}


//
// The WSJT slice is created by the CAT backend on a different TCP
// connection, so it can appear - or be re-created after a radio-side
// failure - long after the receiver started. While the radio reports
// our stream attached to no slice, the receiver must keep asking.
//
void TestFlexVitaReceiver::reasserts_routing_while_the_stream_is_unbound ()
{
  announceSlice_   = true;
  sliceDaxChannel_ = 0;
  streamUnbound_   = true;

  FlexVitaReceiver receiver;

  FlexVitaReceiver::Configuration const configuration =
      makeConfiguration (server_->serverPort ());

  QVERIFY2 (receiver.start (configuration),
            qPrintable (QString::fromStdString (receiver.lastError ())));

  QVERIFY2 (waitForStreaming (receiver),
            qPrintable (QString::fromStdString (receiver.lastError ())));

  QDeadlineTimer deadline {6000};

  while (!deadline.hasExpired ()
         && sliceDaxCommandCount (0, FakeDaxChannel) < 2)
    {
      QCoreApplication::processEvents (QEventLoop::AllEvents, 20);
    }

  QVERIFY2 (sliceDaxCommandCount (0, FakeDaxChannel) >= 2,
            qPrintable (QString ("only %1 routing attempts; commands: %2")
                        .arg (sliceDaxCommandCount (0, FakeDaxChannel))
                        .arg (QString::fromLatin1 (commands_.join (" / ")))));

  receiver.stop ();
}

QTEST_MAIN (TestFlexVitaReceiver)
#include "test_flex_vita_receiver.moc"
