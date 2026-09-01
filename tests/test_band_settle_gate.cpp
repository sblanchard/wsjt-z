#include <QObject>
#include <QTest>

#include "Transceiver/BandSettleGate.hpp"

class TestBandSettleGate final
  : public QObject
{
  Q_OBJECT

private slots:
  void starts_inactive ()
  {
    BandSettleGate gate;
    QVERIFY (!gate.active (1000));
  }

  void active_before_deadline ()
  {
    BandSettleGate gate;
    gate.arm (30000, 1000);
    QVERIFY (gate.active (1000));
    QVERIFY (gate.active (30999));
  }

  void inactive_at_and_after_deadline ()
  {
    BandSettleGate gate;
    gate.arm (30000, 1000);
    QVERIFY (!gate.active (31000));
    QVERIFY (!gate.active (99999));
  }

  void override_ends_hold_immediately ()
  {
    BandSettleGate gate;
    gate.arm (30000, 1000);
    gate.override_hold ();
    QVERIFY (!gate.active (1001));
  }

  void disarm_ends_hold_immediately ()
  {
    BandSettleGate gate;
    gate.arm (30000, 1000);
    gate.disarm ();
    QVERIFY (!gate.active (1001));
  }

  void rearm_replaces_deadline_not_extends ()
  {
    BandSettleGate gate;
    gate.arm (30000, 1000);
    // Re-arm 10s later: new deadline is 11000+30000, not 31000+30000.
    gate.arm (30000, 11000);
    QVERIFY (gate.active (40999));
    QVERIFY (!gate.active (41000));
  }

  void rearm_clears_a_previous_override ()
  {
    BandSettleGate gate;
    gate.arm (30000, 1000);
    gate.override_hold ();
    gate.arm (30000, 11000);
    QVERIFY (gate.active (12000));
  }

  void zero_and_negative_ms_never_activate ()
  {
    BandSettleGate gate;
    gate.arm (0, 1000);
    QVERIFY (!gate.active (1000));
    gate.arm (-5, 1000);
    QVERIFY (!gate.active (1000));
  }

  void remaining_seconds_counts_down_and_floors ()
  {
    BandSettleGate gate;
    gate.arm (30000, 1000);
    QCOMPARE (gate.remaining_seconds (1000), 30);
    // 23.5s left rounds up to 24 whole seconds remaining.
    QCOMPARE (gate.remaining_seconds (7500), 24);
    QCOMPARE (gate.remaining_seconds (31000), 0);
    QCOMPARE (gate.remaining_seconds (99999), 0);
  }

  void remaining_seconds_is_zero_after_override ()
  {
    BandSettleGate gate;
    gate.arm (30000, 1000);
    gate.override_hold ();
    QCOMPARE (gate.remaining_seconds (1001), 0);
  }
};

QTEST_MAIN (TestBandSettleGate)
#include "test_band_settle_gate.moc"
