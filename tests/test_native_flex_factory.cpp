#include <QtTest>

#include "Transceiver/TransceiverFactory.hpp"

#include "commons.h"
#include "widgets/itoneAndicw.h"

// TransceiverFactory::TransceiverFactory () registers TCITransceiver,
// whose translation unit (linked in via libwsjt_qt.a) references these
// globals. In the full application they are defined in mainwindow.cpp /
// getfile.cpp; this unit test does not link those, so provide minimal
// stand-ins purely to satisfy the linker. None of TCITransceiver's code
// that uses them executes in this test (we never construct a
// TCITransceiver or call TransceiverFactory::create for it).
dec_data dec_data;
int volatile itone[MAX_NUM_SYMBOLS];
int volatile icw[NUM_CW_SYMBOLS];
float gran () { return 0.0f; }

class TestNativeFlexFactory : public QObject
{
  Q_OBJECT

private slots:
  void native_flex_is_registered ();
  void native_flex_capabilities ();
  void other_rigs_still_registered ();
};

void TestNativeFlexFactory::native_flex_is_registered ()
{
  TransceiverFactory factory;

  QVERIFY2 (factory.supported_transceivers ().contains ("Flex Native VITA-49"),
            "the Native FLEX rig must be selectable by exact name");
}

void TestNativeFlexFactory::native_flex_capabilities ()
{
  TransceiverFactory factory;

  auto const capabilities =
      factory.supported_transceivers ().value ("Flex Native VITA-49");

  // Port type none: the radio is found over the network, so no serial
  // port or TCI endpoint is offered in the settings UI.
  QCOMPARE (capabilities.port_type_, TransceiverFactory::Capabilities::none);
  QVERIFY  (capabilities.has_CAT_PTT_);
  QVERIFY  (!capabilities.has_CAT_PTT_mic_data_);
  QVERIFY  (!capabilities.has_CAT_indirect_serial_PTT_);
  QVERIFY  (capabilities.asynchronous_);
}

void TestNativeFlexFactory::other_rigs_still_registered ()
{
  TransceiverFactory factory;

  // Adding a backend must not disturb the existing ones.
  QVERIFY (factory.supported_transceivers ().contains ("None"));
  QVERIFY (factory.supported_transceivers ().size () > 10);
}

QTEST_MAIN (TestNativeFlexFactory)
#include "test_native_flex_factory.moc"
