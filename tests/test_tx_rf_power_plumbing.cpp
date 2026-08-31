#include <QList>
#include <QObject>
#include <QTest>

#include "Transceiver/TransceiverBase.hpp"

namespace
{
  class RecordingTransceiver final
    : public TransceiverBase
  {
    Q_OBJECT

  public:
    RecordingTransceiver (logger_type * logger)
      : TransceiverBase {logger, nullptr}
    {
    }

    QList<int> power_calls;

  protected:
    int do_start () override {return 1;}
    void do_stop () override {}
    void do_frequency (Frequency, MODE, bool) override {}
    void do_tx_frequency (Frequency, MODE, bool) override {}
    void do_mode (MODE) override {}
    void do_ptt (bool) override {}
    void do_tx_rf_power_level (int level) override
    {
      power_calls.append (level);
    }
  };
}

class TestTxRfPowerPlumbing final
  : public QObject
{
  Q_OBJECT

private slots:
  void state_defaults_to_unset ()
  {
    Transceiver::TransceiverState state;
    QCOMPARE (state.tx_rf_power_level (), -1);
    state.tx_rf_power_level (55);
    QCOMPARE (state.tx_rf_power_level (), 55);
  }

  void power_not_in_state_comparison ()
  {
    // operator!= covers rig-reported fields only; a power-only
    // difference must not make states unequal (poll ping-pong guard).
    Transceiver::TransceiverState a;
    Transceiver::TransceiverState b;
    b.tx_rf_power_level (70);
    QVERIFY (!(a != b));
  }

  void dispatches_only_changed_nonnegative_levels ()
  {
    Transceiver::logger_type logger;
    RecordingTransceiver rig {&logger};

    rig.start (1);

    Transceiver::TransceiverState state;
    state.online (true);

    // No request: never dispatched.
    rig.set (state, 2);
    QCOMPARE (rig.power_calls.size (), 0);

    // First request: dispatched.
    state.tx_rf_power_level (40);
    rig.set (state, 3);
    QCOMPARE (rig.power_calls, QList<int> {} << 40);

    // Same value again: not re-dispatched.
    rig.set (state, 4);
    QCOMPARE (rig.power_calls.size (), 1);

    // Changed value: dispatched once more.
    state.tx_rf_power_level (85);
    rig.set (state, 5);
    QCOMPARE (rig.power_calls, QList<int> {} << 40 << 85);
  }
};

QTEST_MAIN (TestTxRfPowerPlumbing)
#include "test_tx_rf_power_plumbing.moc"
