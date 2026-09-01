#ifndef FLEX_BAND_LEVELS_HPP_
#define FLEX_BAND_LEVELS_HPP_

#include <QHash>
#include <QString>
#include <QStringList>
#include <QtGlobal>

class QSettings;

//
// Remembers the Flex power and gain levels that belong to each band,
// so an automatic band hop lands with the levels the operator last
// used there.
//
// Values are captured from the radio's own status stream, which means
// a change made in SmartSDR is recorded exactly like one made in
// WSJT-Z. That creates an echo problem: pushing a value on band
// arrival produces a status message carrying the value back. Each
// pushed field is therefore guarded briefly, so its own echo cannot
// be mistaken for the operator setting the value again -- or, worse,
// be attributed to whichever band is current when it arrives.
//
// Unset is -1. The project is C++11, so std::optional is unavailable;
// -1 matches the convention already used for tx_rf_power_level.
//
class FlexBandLevels
{
public:
  enum Field
  {
    RfWatts,
    SliceAfGain,
    DaxRxGain,
    DaxTxGain,
    FieldCount
  };

  struct LevelSet
  {
    LevelSet ();
    int values[FieldCount];
  };

  // How long a pushed field ignores incoming captures while waiting
  // for its own echo.
  static int const guard_ms = 2000;

  // Record a level for a band. Ignored while that field is guarded,
  // and ignored for negative values.
  void capture (QString const& band, Field, int value, qint64 now_ms);

  // Read a band's levels without arming anything. Use this to decide
  // what to push; it arms nothing itself.
  LevelSet peek (QString const& band) const;

  // Arm the echo guard for exactly one field, because the caller is
  // about to push its value to the radio right now. Call this only
  // for a field actually being pushed -- arming a field that was
  // merely read via peek() but never sent would swallow its next
  // genuine capture for no reason.
  void arm_guard (QString const& band, Field, qint64 now_ms);

  void load (QSettings&);
  void save (QSettings&) const;

  // Seed RfWatts for every named band from the pre-per-band global
  // key, so the first run after upgrading behaves as before. Bands
  // that already hold a value are left alone.
  void migrate_legacy_watts (QSettings&, QStringList const& bands);

  static QString const& settings_key ();
  static QString const& legacy_watts_key ();

private:
  struct Guard
  {
    Guard ();
    qint64 until_ms[FieldCount];
  };

  QHash<QString, LevelSet> bands_;
  QHash<QString, Guard> guards_;
};

#endif
