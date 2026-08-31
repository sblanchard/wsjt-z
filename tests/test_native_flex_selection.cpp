#include <QObject>
#include <QTest>

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
};

QTEST_MAIN (TestNativeFlexSelection)
#include "test_native_flex_selection.moc"
