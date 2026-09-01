#include "Transceiver/FlexBandLevels.hpp"

#include <QSettings>
#include <QVariant>
#include <QVariantMap>

namespace
{
  char const * const field_names[FlexBandLevels::FieldCount] = {
    "rf_watts",
    "slice_af_gain",
    "dax_rx_gain",
    "dax_tx_gain"
  };
}

FlexBandLevels::LevelSet::LevelSet ()
{
  for (int f = 0; f < FieldCount; ++f)
    {
      values[f] = -1;
    }
}

FlexBandLevels::Pending::Pending ()
  : until_ms {0}
  , value {-1}
{
}

QString const& FlexBandLevels::settings_key ()
{
  static QString const key {"W7PPFlexBandLevels"};
  return key;
}

QString const& FlexBandLevels::legacy_watts_key ()
{
  static QString const key {"W7PPNativeFlexRfWatts"};
  return key;
}

void FlexBandLevels::capture (QString const& band, Field field,
                              int value, qint64 now_ms)
{
  if (band.isEmpty () || value < 0
      || field < 0 || field >= FieldCount)
    {
      return;
    }

  if (now_ms < pending_[field].until_ms)
    {
      if (value == pending_[field].value)
        {
          // Our own push coming back. Consume the guard so the next
          // change -- a real one -- is recorded. Nothing to store: the
          // pushed value is already what this field holds, and this
          // sample may well belong to a band we have since left.
          pending_[field].until_ms = 0;
          return;
        }

      // Not what we pushed, so it is a reading the radio produced
      // before our command landed -- the source is a poll of a sticky
      // property, not an event stream. Drop it, and leave the guard
      // armed: the real echo has not arrived yet.
      return;
    }

  bands_[band].values[field] = value;
}

void FlexBandLevels::arm_guard (Field field, int value, qint64 now_ms)
{
  if (field < 0 || field >= FieldCount)
    {
      return;
    }

  pending_[field].until_ms = now_ms + guard_ms;
  pending_[field].value = value;
}

FlexBandLevels::LevelSet FlexBandLevels::peek (QString const& band) const
{
  LevelSet set;

  QHash<QString, LevelSet>::const_iterator b = bands_.constFind (band);
  if (b != bands_.constEnd ())
    {
      set = *b;
    }

  return set;
}

void FlexBandLevels::load (QSettings& settings)
{
  bands_.clear ();

  QVariantMap const stored =
      settings.value (settings_key ()).toMap ();

  for (QVariantMap::const_iterator it = stored.constBegin ();
       it != stored.constEnd (); ++it)
    {
      QVariantMap const fields = it.value ().toMap ();
      LevelSet set;

      for (int f = 0; f < FieldCount; ++f)
        {
          QVariantMap::const_iterator v =
              fields.constFind (QString::fromLatin1 (field_names[f]));

          if (v != fields.constEnd ())
            {
              bool ok = false;
              int const value = v.value ().toInt (&ok);
              if (ok && value >= 0)
                {
                  set.values[f] = value;
                }
            }
        }

      bands_.insert (it.key (), set);
    }
}

void FlexBandLevels::save (QSettings& settings) const
{
  QVariantMap stored;

  for (QHash<QString, LevelSet>::const_iterator it = bands_.constBegin ();
       it != bands_.constEnd (); ++it)
    {
      QVariantMap fields;

      for (int f = 0; f < FieldCount; ++f)
        {
          if (it.value ().values[f] >= 0)
            {
              fields.insert (QString::fromLatin1 (field_names[f]),
                             it.value ().values[f]);
            }
        }

      if (!fields.isEmpty ())
        {
          stored.insert (it.key (), fields);
        }
    }

  settings.setValue (settings_key (), stored);
}

void FlexBandLevels::migrate_legacy_watts (QSettings& settings,
                                           QStringList const& bands)
{
  if (!settings.contains (legacy_watts_key ()))
    {
      return;
    }

  bool ok = false;
  int const watts = settings.value (legacy_watts_key ()).toInt (&ok);

  if (!ok || watts < 0)
    {
      return;
    }

  for (int i = 0; i < bands.size (); ++i)
    {
      QString const& band = bands.at (i);
      if (band.isEmpty ())
        {
          continue;
        }

      LevelSet& set = bands_[band];
      if (set.values[RfWatts] < 0)
        {
          set.values[RfWatts] = watts;
        }
    }
}
