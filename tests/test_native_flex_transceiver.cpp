#include <QtTest>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QHash>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include "Transceiver/NativeFlexRadioSelection.hpp"
#include "Transceiver/NativeFlexTransceiver.hpp"
#include "Transceiver/Transceiver.hpp"

namespace
{
  quint32 const FirstHandle = 0x2B0A1F00u;
  char const * const SmartSdrHandle = "0x1AB2C3D4";
  char const * const SmartSdrClientId = "8ED2C0A5-2D50-4B33-9A5C-2E1F8F6D0C11";
  char const * const GuiClientId = "C0FFEE00-0000-4000-8000-000000000001";

  enum class Mode
  {
    Headless,                 // no SmartSDR client connected
    SmartSdr,                 // SmartSDR-Win connected, client_id present
    SmartSdrWithoutClientId,  // SmartSDR-Win connected, client_id missing
    SmartSdrSharedPan         // as SmartSdr, but the new slice lands on
                              // SmartSDR's existing panadapter
  };

  // Minimal scripted SmartSDR TCP API endpoint on loopback.
  //
  // Records every command text it receives, in order, across all
  // connections. Status lines that the real radio would push are
  // written before the R response of the command that triggers them.
  // That is deliberately the easy ordering: the transceiver's
  // drain_control_lines () windows exist for the opposite one, where
  // the radio trails its status lines after the R response, so this
  // fake does not exercise them.
  class FakeRadio final : public QObject
  {
    Q_OBJECT

  public:
    explicit FakeRadio (Mode mode, QObject * parent = nullptr)
      : QObject {parent}
      , mode_ {mode}
    {
      QVERIFY2 (server_.listen (QHostAddress::LocalHost),
                qPrintable (server_.errorString ()));

      connect (&server_, &QTcpServer::newConnection, this, [this] ()
        {
          while (auto * client = server_.nextPendingConnection ())
            {
              quint32 const handle = FirstHandle + connections_++;
              handles_.insert (client, handle);

              client->write ("V1.4.0.0\r\n");
              client->write ("H" + QByteArray::number (handle, 16) + "\r\n");

              connect (client, &QTcpSocket::readyRead, this, [this, client] ()
                {
                  pending_[client] += client->readAll ();

                  for (;;)
                    {
                      int const end = pending_[client].indexOf ('\n');
                      if (end < 0) break;

                      QByteArray line = pending_[client].left (end).trimmed ();
                      pending_[client].remove (0, end + 1);

                      if (!line.isEmpty ()) on_line (client, line);
                    }
                });
            }
        });
    }

    quint16 port () const { return server_.serverPort (); }
    QStringList const& commands () const { return commands_; }

  private:
    void on_line (QTcpSocket * client, QByteArray const& line)
    {
      if (!line.startsWith ('C')) return;

      int const pipe = line.indexOf ('|');
      if (pipe < 2) return;

      QByteArray const seq = line.mid (1, pipe - 1);
      QByteArray const cmd = line.mid (pipe + 1);
      commands_ << QString::fromLatin1 (cmd);

      quint32 const handle = handles_.value (client);
      QByteArray const prefix = "S" + QByteArray::number (handle, 16) + "|";
      QByteArray const ours = "0x" + QByteArray::number (handle, 16);

      QByteArray status;
      QByteArray data;

      if (cmd == "sub client all")
        {
          if (Mode::SmartSdr == mode_ || Mode::SmartSdrSharedPan == mode_)
            {
              status += prefix + "client " + SmartSdrHandle
                + " connected client_id=" + SmartSdrClientId
                + " program=SmartSDR-Win station=BENCH local_ptt=1\r\n";
            }
          else if (Mode::SmartSdrWithoutClientId == mode_)
            {
              status += prefix + "client " + SmartSdrHandle
                + " connected program=SmartSDR-Win station=BENCH local_ptt=1\r\n";
            }
        }
      else if (cmd == "sub slice all")
        {
          if (Mode::Headless != mode_)
            {
              // SmartSDR's own slice, currently the TX slice, on 40 m.
              status += prefix + "slice 0 in_use=1 client_handle=" + SmartSdrHandle
                + " tx=1 pan=0x40000000 RF_frequency=7.074000 mode=USB\r\n";
            }
        }
      else if (cmd == "slice create mode=digu")
        {
          if (Mode::Headless == mode_)
            {
              status += prefix + "slice 0 in_use=1 client_handle=" + ours
                + " tx=0 pan=0x40000000 RF_frequency=14.074000 mode=DIGU\r\n";
            }
          else
            {
              // The radio hands the new slice to SmartSDR's GUI handle.
              //
              // With both panadapters already in use the radio has none
              // left to create, so it attaches the new slice to the one
              // SmartSDR is already displaying.
              char const * const pan =
                Mode::SmartSdrSharedPan == mode_ ? "0x40000000" : "0x40000001";

              status += prefix + "slice 1 in_use=1 client_handle=" + SmartSdrHandle
                + " tx=0 pan=" + pan + " RF_frequency=14.074000 mode=DIGU\r\n";
            }
        }
      else if (cmd == "stream create type=dax_tx")
        {
          status += prefix + "stream 0x84000001 type=dax_tx client_handle=" + ours
            + " tx=0\r\n";
        }
      else if (cmd == "client gui")
        {
          data = GuiClientId;
        }

      client->write (status);
      client->write ("R" + seq + "|0|" + data + "\r\n");
    }

    Mode mode_;
    QTcpServer server_;
    int connections_ {0};
    QHash<QTcpSocket *, quint32> handles_;
    QHash<QTcpSocket *, QByteArray> pending_;
    QStringList commands_;
  };

  // The transceiver on its own thread, driven exactly as Configuration
  // drives it: start () and stop () are queued slot invocations.
  class Rig final
  {
  public:
    Rig ()
      : rig_ {new NativeFlexTransceiver {&logger_}}
      , resolution_ {rig_, &Transceiver::resolution}
      , failure_ {rig_, &Transceiver::failure}
      , finished_ {rig_, &Transceiver::finished}
    {
      rig_->moveToThread (&thread_);
      thread_.start ();
    }

    ~Rig ()
    {
      QMetaObject::invokeMethod (rig_, "deleteLater", Qt::QueuedConnection);
      thread_.quit ();

      // When the test asserts and returns without an explicit stop (),
      // the transceiver's own automatic failure cleanup is still in
      // flight on the worker thread, and that cleanup can still need a
      // reply from the fake radio, which lives on this (the test's)
      // main thread. A plain thread_.wait () would block this thread's
      // event loop and starve the fake radio of the readyRead events
      // it needs to answer, wedging the worker thread for the whole
      // wait and aborting the process on teardown. Pump events while
      // waiting so the fake radio keeps responding.
      QDeadlineTimer deadline {15000};
      while (thread_.isRunning () && !deadline.hasExpired ())
        {
          QCoreApplication::processEvents (QEventLoop::AllEvents, 50);
          thread_.wait (50);
        }
    }

    void start ()
    {
      QMetaObject::invokeMethod (rig_, [this] () { rig_->start (1); }, Qt::QueuedConnection);
    }

    void stop ()
    {
      QMetaObject::invokeMethod (rig_, [this] () { rig_->stop (); }, Qt::QueuedConnection);
    }

    QSignalSpy & resolution () { return resolution_; }
    QSignalSpy & failure () { return failure_; }
    QSignalSpy & finished () { return finished_; }

  private:
    // CAT_TRACE dereferences logger (), so the transceiver needs a real
    // one; tests/test_tx_rf_power_plumbing.cpp does the same. Declared
    // before rig_ so it is constructed first.
    Transceiver::logger_type logger_;
    QThread thread_;
    NativeFlexTransceiver * rig_;
    QSignalSpy resolution_;
    QSignalSpy failure_;
    QSignalSpy finished_;
  };

  void select_radio (quint16 port)
  {
    NativeFlexRadioSelection::Radio radio;
    radio.model = "FLEX-8400M";
    radio.serial = "0000-0000-0000-0000";
    radio.address = "127.0.0.1";
    radio.port = port;
    NativeFlexRadioSelection::restore (radio);
  }

  QString failure_text (QSignalSpy & spy)
  {
    return spy.isEmpty () ? QString {} : spy.first ().first ().toString ();
  }

  int index_of (QStringList const& commands, QString const& command)
  {
    return commands.indexOf (command);
  }
}

class TestNativeFlexTransceiver final : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase ()
  {
    qRegisterMetaType<Transceiver::TransceiverState> ();
  }

  void init ()
  {
    NativeFlexRadioSelection::clear ();
  }

  void headless_registers_gui_client_and_owns_its_slice ()
  {
    FakeRadio radio {Mode::Headless};
    select_radio (radio.port ());

    Rig rig;
    rig.start ();

    QTRY_VERIFY_WITH_TIMEOUT (rig.resolution ().count () + rig.failure ().count () > 0, 15000);
    QVERIFY2 (rig.failure ().isEmpty (), qPrintable (failure_text (rig.failure ())));

    auto const& c = radio.commands ();

    // The mode decision is made first, then the accepted headless path.
    QVERIFY (index_of (c, "sub client all") >= 0);
    QVERIFY (index_of (c, "client gui") > index_of (c, "sub client all"));
    QVERIFY (index_of (c, "slice create mode=digu") > index_of (c, "client gui"));

    // Headless: the slice is made the TX slice at startup.
    QVERIFY (c.contains ("slice s 0 mode=digu"));
    QVERIFY (c.contains ("slice s 0 dax=1"));
    QVERIFY (c.contains ("slice s 0 tx=1 mode=digu"));

    // Our DAX-TX stream is explicitly selected as the TX sample source.
    QVERIFY (index_of (c, "stream set 0x84000001 tx=1") > index_of (c, "stream create type=dax_tx"));

    // Nothing from the coexistence path.
    QVERIFY (!c.filter (QRegularExpression {"^client bind"}).size ());

    rig.stop ();
    QTRY_VERIFY_WITH_TIMEOUT (rig.finished ().count () > 0, 15000);

    auto const& s = radio.commands ();
    QVERIFY (s.contains ("xmit 0"));
    QVERIFY (index_of (s, "stream remove 0x84000001") > index_of (s, "xmit 0"));
    QVERIFY (index_of (s, "slice r 0") > index_of (s, "stream remove 0x84000001"));
    QVERIFY (!s.filter (QRegularExpression {"^display pan remove"}).size ());
  }

  void coexistence_binds_to_smartsdr_and_takes_only_the_new_slice ()
  {
    FakeRadio radio {Mode::SmartSdr};
    select_radio (radio.port ());

    Rig rig;
    rig.start ();

    QTRY_VERIFY_WITH_TIMEOUT (rig.resolution ().count () + rig.failure ().count () > 0, 15000);
    QVERIFY2 (rig.failure ().isEmpty (), qPrintable (failure_text (rig.failure ())));

    auto const& c = radio.commands ();

    QVERIFY (!c.contains ("client gui"));
    QVERIFY (index_of (c, QString {"client bind client_id=%1"}.arg (SmartSdrClientId))
             > index_of (c, "sub client all"));
    QVERIFY (index_of (c, "sub slice all") > index_of (c, QString {"client bind client_id=%1"}.arg (SmartSdrClientId)));
    QVERIFY (index_of (c, "slice create mode=digu") > index_of (c, "sub slice all"));

    // WSJT's slice is the NEW one (1), not SmartSDR's snapshot slice (0).
    QVERIFY (c.contains ("slice s 1 mode=digu"));
    QVERIFY (c.contains ("slice s 1 dax=1"));
    QVERIFY (!c.filter (QRegularExpression {"^slice s 0 "}).size ());

    // Coexistence: the TX slice is NOT taken at startup.
    QVERIFY (!c.contains ("slice s 1 tx=1 mode=digu"));

    QVERIFY (c.contains ("transmit set dax=1"));
    QVERIFY (c.contains ("dax audio set 1 slice=1 tx=1"));
    QVERIFY (index_of (c, "stream set 0x84000001 tx=1") > index_of (c, "stream create type=dax_tx"));

    rig.stop ();
    QTRY_VERIFY_WITH_TIMEOUT (rig.finished ().count () > 0, 15000);

    auto const& s = radio.commands ();

    // "xmit" is radio-global, so coexistence must not unkey a session
    // that never keyed: that would drop the SmartSDR operator's carrier.
    QVERIFY (!s.contains ("xmit 0"));

    // Teardown order, anchored on the last startup command instead.
    QVERIFY (index_of (s, "stream remove 0x84000001")
             > index_of (s, "stream set 0x84000001 tx=1"));
    QVERIFY (index_of (s, "slice r 1") > index_of (s, "stream remove 0x84000001"));

    // SmartSDR's TX slice is restored and the extra panadapter removed.
    QVERIFY (index_of (s, "slice s 0 tx=1") > index_of (s, "slice r 1"));
    QVERIFY (index_of (s, "display pan remove 0x40000001") > index_of (s, "slice s 0 tx=1"));
  }

  // A 2-slice/2-pan radio with SmartSDR holding both panadapters has
  // none left to create, so WSJT's new slice lands on SmartSDR's.
  // Removing it at shutdown would delete the other operator's display.
  void coexistence_keeps_a_panadapter_that_predates_our_slice ()
  {
    FakeRadio radio {Mode::SmartSdrSharedPan};
    select_radio (radio.port ());

    Rig rig;
    rig.start ();

    QTRY_VERIFY_WITH_TIMEOUT (rig.resolution ().count () + rig.failure ().count () > 0, 15000);
    QVERIFY2 (rig.failure ().isEmpty (), qPrintable (failure_text (rig.failure ())));

    rig.stop ();
    QTRY_VERIFY_WITH_TIMEOUT (rig.finished ().count () > 0, 15000);

    auto const& s = radio.commands ();

    // WSJT's own slice still goes.
    QVERIFY (s.contains ("slice r 1"));

    // SmartSDR's panadapter stays.
    QVERIFY2 (!s.filter (QRegularExpression {"^display pan remove"}).size (),
              qPrintable (s.join (" / ")));
  }

  void coexistence_without_client_id_fails_closed ()
  {
    FakeRadio radio {Mode::SmartSdrWithoutClientId};
    select_radio (radio.port ());

    Rig rig;
    rig.start ();

    QTRY_VERIFY_WITH_TIMEOUT (rig.resolution ().count () + rig.failure ().count () > 0, 15000);
    QVERIFY (rig.resolution ().isEmpty ());
    QVERIFY2 (failure_text (rig.failure ()).contains ("no SmartSDR client_id"),
              qPrintable (failure_text (rig.failure ())));

    auto const& c = radio.commands ();
    QVERIFY (!c.contains ("client gui"));
    QVERIFY (!c.filter (QRegularExpression {"^client bind"}).size ());
    QVERIFY (!c.contains ("slice create mode=digu"));
  }
};

QTEST_MAIN (TestNativeFlexTransceiver)
#include "test_native_flex_transceiver.moc"
