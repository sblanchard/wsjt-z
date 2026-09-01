#include <QObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

#include "Transceiver/FlexBandLevels.hpp"

class TestFlexBandLevels final
  : public QObject
{
  Q_OBJECT

private slots:
  void unvisited_band_has_every_field_unset ()
  {
    FlexBandLevels levels;
    FlexBandLevels::LevelSet const set = levels.peek ("20m");
    for (int f = 0; f < FlexBandLevels::FieldCount; ++f)
      {
        QCOMPARE (set.values[f], -1);
      }
  }

  void capture_then_peek_returns_the_value ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);
    FlexBandLevels::LevelSet const set = levels.peek ("20m");
    QCOMPARE (set.values[FlexBandLevels::RfWatts], 35);
  }

  // peek() is the read-only observer: unlike apply(), it must not arm
  // the echo guards, or every assertion would change what it measures.
  void peek_does_not_arm_guards ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);
    levels.peek ("20m");
    levels.capture ("20m", FlexBandLevels::RfWatts, 70, 1100);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 70);
  }

  void bands_are_independent ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);
    levels.capture ("40m", FlexBandLevels::RfWatts, 80, 1000);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 35);
    QCOMPARE (levels.peek ("40m").values[FlexBandLevels::RfWatts], 80);
  }

  void all_four_fields_round_trip_independently ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);
    levels.capture ("20m", FlexBandLevels::SliceAfGain, 42, 1000);
    levels.capture ("20m", FlexBandLevels::DaxRxGain, 55, 1000);
    levels.capture ("20m", FlexBandLevels::DaxTxGain, 61, 1000);

    FlexBandLevels::LevelSet const set = levels.peek ("20m");
    QCOMPARE (set.values[FlexBandLevels::RfWatts], 35);
    QCOMPARE (set.values[FlexBandLevels::SliceAfGain], 42);
    QCOMPARE (set.values[FlexBandLevels::DaxRxGain], 55);
    QCOMPARE (set.values[FlexBandLevels::DaxTxGain], 61);
  }

  void arm_guard_guards_the_pushed_field_against_its_own_echo ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);

    // Pushing to the radio arms the guard for that field, on the value
    // being pushed.
    levels.arm_guard (FlexBandLevels::RfWatts, 35, 2000);

    // The radio echoes the value straight back: dropped.
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 2100);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 35);

    // The echo cleared the guard, so a genuine later change lands.
    levels.capture ("20m", FlexBandLevels::RfWatts, 70, 2300);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 70);
  }

  void guard_expires_when_no_echo_arrives ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);
    levels.arm_guard (FlexBandLevels::RfWatts, 35, 2000);

    // Nothing echoed. After the 2s timeout, captures land again.
    levels.capture ("20m", FlexBandLevels::RfWatts, 70, 4100);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 70);
  }

  void guard_does_not_block_a_different_field ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);
    levels.arm_guard (FlexBandLevels::RfWatts, 35, 2000);

    levels.capture ("20m", FlexBandLevels::SliceAfGain, 42, 2100);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::SliceAfGain], 42);
  }

  // The capture source is a once-a-second poll of a sticky property,
  // not an event stream, so the first sample after a push normally
  // still carries the PREVIOUS band's value. It must not be stored --
  // that would overwrite this band's level with the last band's -- and
  // it must not consume the guard, or the real echo would then land as
  // if it were an operator change.
  void a_stale_sample_inside_the_window_neither_lands_nor_clears ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::SliceAfGain, 40, 1000);

    // Arriving on 20m pushes 40 and arms the guard on it.
    levels.arm_guard (FlexBandLevels::SliceAfGain, 40, 2000);

    // One second later the poll still reports 75, the value the radio
    // held on the band we just left.
    levels.capture ("20m", FlexBandLevels::SliceAfGain, 75, 3000);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::SliceAfGain], 40);

    // The genuine echo arrives while the guard is still armed: still
    // recognised, still dropped, and now the guard is consumed.
    levels.capture ("20m", FlexBandLevels::SliceAfGain, 40, 3500);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::SliceAfGain], 40);

    // Guard gone: a real later change lands.
    levels.capture ("20m", FlexBandLevels::SliceAfGain, 75, 3600);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::SliceAfGain], 75);
  }

  // Guards are per field, not per band: an echo that arrives after a
  // hop must not be recorded against the band that happens to be
  // current when it lands.
  void an_echo_arriving_after_a_hop_is_not_stored_on_the_new_band ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::DaxRxGain, 30, 1000);
    levels.capture ("40m", FlexBandLevels::DaxRxGain, 80, 1000);

    // Push 30 on 20m, then hop to 40m before the echo comes back.
    levels.arm_guard (FlexBandLevels::DaxRxGain, 30, 2000);
    levels.capture ("40m", FlexBandLevels::DaxRxGain, 30, 2100);

    QCOMPARE (levels.peek ("40m").values[FlexBandLevels::DaxRxGain], 80);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::DaxRxGain], 30);
  }

  // A field the caller never actually pushed -- and therefore never
  // armed -- must not be guarded. Deciding what to arm is now entirely
  // the caller's job (peek(), decide what to push, arm_guard() only
  // for that), so this is what used to be "apply() pushed nothing and
  // guarded nothing" when nothing was stored.
  void field_never_armed_is_never_guarded ()
  {
    FlexBandLevels levels;
    // No arm_guard() call at all for this field.
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 2100);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 35);
  }

  void negative_captures_are_ignored ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, -1, 1000);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], -1);
  }

  void save_and_load_preserve_all_bands_and_fields ()
  {
    QTemporaryDir dir;
    QVERIFY (dir.isValid ());
    QString const path = dir.path () + "/levels.ini";

    {
      FlexBandLevels levels;
      levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);
      levels.capture ("20m", FlexBandLevels::DaxTxGain, 61, 1000);
      levels.capture ("40m", FlexBandLevels::SliceAfGain, 42, 1000);

      QSettings settings {path, QSettings::IniFormat};
      levels.save (settings);
      settings.sync ();
    }

    FlexBandLevels restored;
    QSettings settings {path, QSettings::IniFormat};
    restored.load (settings);

    FlexBandLevels::LevelSet const twenty = restored.peek ("20m");
    QCOMPARE (twenty.values[FlexBandLevels::RfWatts], 35);
    QCOMPARE (twenty.values[FlexBandLevels::DaxTxGain], 61);
    // Never captured: still unset after a round trip.
    QCOMPARE (twenty.values[FlexBandLevels::SliceAfGain], -1);

    QCOMPARE (restored.peek ("40m").values[FlexBandLevels::SliceAfGain], 42);
  }

  void load_migrates_the_legacy_global_watts_key ()
  {
    QTemporaryDir dir;
    QVERIFY (dir.isValid ());
    QString const path = dir.path () + "/legacy.ini";

    {
      QSettings settings {path, QSettings::IniFormat};
      settings.setValue (FlexBandLevels::legacy_watts_key (), 45);
      settings.sync ();
    }

    FlexBandLevels levels;
    QSettings settings {path, QSettings::IniFormat};
    QStringList bands;
    bands << "20m" << "40m";
    levels.load (settings);
    levels.migrate_legacy_watts (settings, bands);

    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 45);
    QCOMPARE (levels.peek ("40m").values[FlexBandLevels::RfWatts], 45);
  }

  void migration_never_overwrites_a_stored_band ()
  {
    QTemporaryDir dir;
    QVERIFY (dir.isValid ());
    QString const path = dir.path () + "/legacy2.ini";

    QSettings settings {path, QSettings::IniFormat};
    settings.setValue (FlexBandLevels::legacy_watts_key (), 45);

    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 90, 1000);

    QStringList bands;
    bands << "20m" << "40m";
    levels.migrate_legacy_watts (settings, bands);

    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 90);
    QCOMPARE (levels.peek ("40m").values[FlexBandLevels::RfWatts], 45);
  }
};

QTEST_MAIN (TestFlexBandLevels)
#include "test_flex_band_levels.moc"
