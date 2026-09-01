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

  void apply_guards_pushed_fields_against_their_own_echo ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);

    // Pushing to the radio arms the guard for that field.
    levels.apply ("20m", 2000);

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
    levels.apply ("20m", 2000);

    // Nothing echoed. After the 2s timeout, captures land again.
    levels.capture ("20m", FlexBandLevels::RfWatts, 70, 4100);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::RfWatts], 70);
  }

  void guard_does_not_block_a_different_field ()
  {
    FlexBandLevels levels;
    levels.capture ("20m", FlexBandLevels::RfWatts, 35, 1000);
    levels.apply ("20m", 2000);

    levels.capture ("20m", FlexBandLevels::SliceAfGain, 42, 2100);
    QCOMPARE (levels.peek ("20m").values[FlexBandLevels::SliceAfGain], 42);
  }

  void unset_fields_are_not_guarded_by_apply ()
  {
    FlexBandLevels levels;
    // Nothing stored, so apply() pushes nothing and guards nothing.
    levels.apply ("20m", 2000);
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
