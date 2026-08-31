#include <QHostAddress>
#include <QObject>
#include <QSignalSpy>
#include <QTest>
#include <QUdpSocket>

#include "Transceiver/NativeFlexDiscovery.hpp"
#include "Transceiver/NativeFlexRadioSelection.hpp"

class TestNativeFlexSelection final
  : public QObject
{
  Q_OBJECT

private slots:
  void init ()
  {
    NativeFlexRadioSelection::clear ();
  }

  void restore_round_trips ()
  {
    NativeFlexRadioSelection::Radio radio;
    radio.model = "FLEX-8400M";
    radio.serial = "1234-5678-9012-3456";
    radio.address = "192.168.1.100";
    radio.port = 4992;

    QVERIFY (!NativeFlexRadioSelection::hasSelection ());
    NativeFlexRadioSelection::restore (radio);
    QVERIFY (NativeFlexRadioSelection::hasSelection ());

    auto const got = NativeFlexRadioSelection::selected ();
    QCOMPARE (got.model, radio.model);
    QCOMPARE (got.serial, radio.serial);
    QCOMPARE (got.address, radio.address);
    QCOMPARE (got.port, radio.port);
  }

  void invalid_radio_restores_but_reports_no_selection ()
  {
    NativeFlexRadioSelection::Radio radio;   // empty address
    NativeFlexRadioSelection::restore (radio);
    QVERIFY (!NativeFlexRadioSelection::hasSelection ());
  }

  void clear_removes_selection ()
  {
    NativeFlexRadioSelection::Radio radio;
    radio.address = "10.0.0.5";
    NativeFlexRadioSelection::restore (radio);
    QVERIFY (NativeFlexRadioSelection::hasSelection ());
    NativeFlexRadioSelection::clear ();
    QVERIFY (!NativeFlexRadioSelection::hasSelection ());
  }

  void parse_extracts_radio_fields ()
  {
    QByteArray const datagram =
        QByteArray ("\x38\x00\x00\x00", 4)
        + "discovery_protocol_version=3.0.0.2 model=FLEX-8400M "
          "serial=1234-5678-9012-3456 ip=192.168.1.100 port=4992 "
          "nickname=Shack";

    auto const radio =
        NativeFlexDiscovery::parse (datagram, "192.168.1.50");

    QVERIFY (radio.valid ());
    QCOMPARE (radio.model, QString {"FLEX-8400M"});
    QCOMPARE (radio.serial, QString {"1234-5678-9012-3456"});
    QCOMPARE (radio.address, QString {"192.168.1.100"});
    QCOMPARE (radio.port, quint16 {4992});
  }

  void parse_rejects_non_flex_models ()
  {
    QByteArray const datagram =
        "discovery_protocol_version=3.0.0.2 model=IC-7300 "
        "serial=X ip=192.168.1.7";
    QVERIFY (!NativeFlexDiscovery::parse (datagram, "192.168.1.7").valid ());
  }

  void parse_falls_back_to_sender_address ()
  {
    QByteArray const datagram =
        "discovery_protocol_version=3.0.0.2 model=FLEX-6600 serial=S1";
    auto const radio =
        NativeFlexDiscovery::parse (datagram, "192.168.1.60");
    QVERIFY (radio.valid ());
    QCOMPARE (radio.address, QString {"192.168.1.60"});
  }

  void engine_collects_and_deduplicates ()
  {
    NativeFlexDiscovery discovery {0};   // ephemeral test port
    QVERIFY (discovery.start ());

    QSignalSpy spy {&discovery, &NativeFlexDiscovery::radios_changed};

    QUdpSocket sender;
    QByteArray const announce =
        "discovery_protocol_version=3.0.0.2 model=FLEX-8400M "
        "serial=AAAA ip=192.168.1.100 port=4992";

    sender.writeDatagram (
        announce, QHostAddress::LocalHost, discovery.bound_port ());
    QTRY_COMPARE (discovery.radios ().size (), 1);
    QVERIFY (spy.count () >= 1);

    // Same radio again: no new entry.
    sender.writeDatagram (
        announce, QHostAddress::LocalHost, discovery.bound_port ());
    QTest::qWait (100);
    QCOMPARE (discovery.radios ().size (), 1);

    // Same serial, new address (DHCP move): entry updated in place.
    QByteArray const moved =
        "discovery_protocol_version=3.0.0.2 model=FLEX-8400M "
        "serial=AAAA ip=192.168.1.222 port=4992";
    sender.writeDatagram (
        moved, QHostAddress::LocalHost, discovery.bound_port ());
    QTRY_COMPARE (discovery.radios ().first ().address,
                  QString {"192.168.1.222"});
    QCOMPARE (discovery.radios ().size (), 1);

    // Second radio: second entry.
    QByteArray const other =
        "discovery_protocol_version=3.0.0.2 model=FLEX-6600 "
        "serial=BBBB ip=192.168.1.101 port=4992";
    sender.writeDatagram (
        other, QHostAddress::LocalHost, discovery.bound_port ());
    QTRY_COMPARE (discovery.radios ().size (), 2);

    discovery.stop ();
  }
};

QTEST_MAIN (TestNativeFlexSelection)
#include "test_native_flex_selection.moc"
