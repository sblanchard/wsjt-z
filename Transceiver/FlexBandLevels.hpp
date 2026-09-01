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
// The guard matches on the pushed *value*, not merely on the arrival
// of any sample. The capture source is a 1 Hz poll of a sticky
// property, not an event stream, so the first sample after a push
// usually still carries the previous band's value: the command has
// not round-tripped yet. A guard that were consumed by the first
// sample to touch it would be spent on exactly the stale reading it
// exists to reject, and the band would then store the previous band's
// level -- destroying the per-band memory after a few hops. Matching
// on value means a stale sample is dropped and the guard keeps
// waiting for the real echo.
//
// Guards are keyed by field rather than by band for the same reason:
// a push on band A whose echo lands after a hop must not be recorded
// against band B, where a band-keyed guard would not exist.
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

  // Record a level for a band. Ignored for negative values. While a
  // push of this field is outstanding, a sample carrying the pushed
  // value is the echo -- dropped, and it consumes the guard -- while
  // any other value is a stale reading taken before the push landed:
  // dropped, and the guard stays armed. Once the guard lapses samples
  // are recorded normally, which is right even when the radio
  // rejected the push: the radio genuinely is still at the old value.
  void capture (QString const& band, Field, int value, qint64 now_ms);

  // Read a band's levels without arming anything. Use this to decide
  // what to push; it arms nothing itself.
  LevelSet peek (QString const& band) const;

  // Arm the echo guard for exactly one field, because the caller is
  // about to push `value` to the radio right now. Call this only for
  // a field actually being pushed -- arming a field that was merely
  // read via peek() but never sent would swallow its next genuine
  // capture for no reason. No band is needed: the echo is recognised
  // by the value, whichever band happens to be current when it lands.
  void arm_guard (Field, int value, qint64 now_ms);

  void load (QSettings&);
  void save (QSettings&) const;

  // Seed RfWatts for every named band from the pre-per-band global
  // key, so the first run after upgrading behaves as before. Bands
  // that already hold a value are left alone.
  void migrate_legacy_watts (QSettings&, QStringList const& bands);

  static QString const& settings_key ();
  static QString const& legacy_watts_key ();

private:
  // One outstanding push per field: the value sent, and the instant
  // after which we stop waiting for its echo.
  struct Pending
  {
    Pending ();
    qint64 until_ms;
    int value;
  };

  QHash<QString, LevelSet> bands_;
  Pending pending_[FieldCount];
};

#endif
