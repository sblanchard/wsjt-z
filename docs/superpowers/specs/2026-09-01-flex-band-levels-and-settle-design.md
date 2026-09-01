# Per-Band Flex Levels and Antenna Settle Hold

Date: 2026-09-01
Status: Approved design, pending implementation plan

## Problem

Two gaps in the Native FLEX (VITA-49) path, both hit during automatic band
hopping.

**The antenna needs time.** `switchBand()` changes band and, when `autoTune`
is enabled, immediately keys a four-second tune carrier. The station's antenna
takes about thirty seconds to retune to a new frequency, so that carrier — and
the auto-TX cycle that follows it — go into a badly mismatched load.

**Levels are not per band.** WSJT-X already remembers the audio Pwr slider per
band (`m_pwrBandTxMemory`). In Native FLEX mode that slider is reinterpreted as
RF watts, and `on_outAttenuation_valueChanged` returns early before reaching the
per-band memory, persisting instead to a single global `W7PPNativeFlexRfWatts`
key. Slice AF gain, DAX RX gain, and DAX TX gain are not tracked at all —
`NativeFlexTransceiver` implements only `do_tx_rf_power_level`. Every hop
therefore lands on a band with the previous band's levels.

## Scope

Both features are active **only** when the configured rig is
`"Flex Native VITA-49"`. Every other rig sees no behaviour change whatsoever.

The settle hold arms in `switchBand()` and nowhere else. That function is
precisely the set of paths where WSJT-Z changes band and then starts
transmitting on its own initiative, which is the behaviour the hold exists to
restrain. A manual band change through the combo box calls
`on_bandComboBox_activated()` directly, never `switchBand()`, so it does not arm
the hold — the operator is at the keyboard and can judge for themselves.

`switchBand()` has two callers, and both are covered deliberately:

- `toggleBands()` (`widgets/mainwindow.cpp:17579`) — the band hopper. The
  central case.
- `pskTableClicked()` (`widgets/mainwindow.cpp:18019`) — the operator clicks a
  spot in the PSK Reporter table. The click is manual, but the code that follows
  it is not: it calls `on_txb1_clicked()` and `auto_tx_mode(true)` immediately,
  so TX timing is decided by the program, not the operator, and the antenna is
  just as unsettled. The hold applies. The cost is that a spot clicked right
  after a band change may be missed; the operator override (a Tune or Enable Tx
  click) is the escape hatch.

Four levels are stored per band:

| Field | Source |
| --- | --- |
| `rf_watts` | SmartSDR `rfpower`, already plumbed |
| `slice_af_gain` | slice `audio_gain`, new |
| `dax_rx_gain` | DAX RX stream gain, new |
| `dax_tx_gain` | DAX TX stream gain, new |

## Architecture

Two new units in `wsjt_qt` (`CMakeLists.txt:183`). They are standalone rather
than members of `MainWindow` for two reasons: `mainwindow.cpp` is already about
18,000 lines, and it is not part of `wsjt_qt`, so nothing in it can be reached
by the test executables in `tests/`.

### `Transceiver/BandSettleGate.{hpp,cpp}`

Pure logic, no widgets, no timer of its own — `guiUpdate()` already ticks at
roughly 100 ms and drives the status bar.

```
void  arm (int ms, qint64 now);
void  override ();          // operator forced TX; hold ends now
void  disarm ();
bool  active (qint64 now) const;
int   remaining_seconds (qint64 now) const;
```

State is a deadline timestamp plus an `overridden_` flag. The clock is passed
in so tests need no event loop.

### `Transceiver/FlexBandLevels.{hpp,cpp}`

```
enum Field {RfWatts, SliceAfGain, DaxRxGain, DaxTxGain, FieldCount};

struct LevelSet {
  int values[FieldCount];   // -1 == unset
};

void      capture (QString const& band, Field, int value, qint64 now_ms);
LevelSet  apply   (QString const& band, qint64 now_ms);   // not const: arms guards
void      load    (QSettings&);
void      save    (QSettings&) const;
void      migrate_legacy_watts (QSettings&, QStringList const& bands);
```

`apply()` is not const because it arms the echo guards described below, and both
it and `capture()` take the clock so the guards are testable without an event
loop.

Per-field optionality is load-bearing: the first visit to a band must adopt the
radio's current value, not push a zero. The project builds as **C++11**
(`CMakeLists.txt:1091`, `tests/CMakeLists.txt`), so `std::optional` is not
available. Unset is represented by `-1`, matching the convention already used
for `tx_rf_power_level_` (`Transceiver/Transceiver.hpp:111`). All four fields
are non-negative in normal use, so `-1` is unambiguous.

### `Configuration::is_flex_native_rig()`

A new const accessor returning `rig_params_.rig_name == "Flex Native VITA-49"`.
The existing `Configuration::flex_native_rx()` (`Configuration.cpp:921`) also
requires a stored user preference, so it is the wrong predicate here. The bare
string comparison at `widgets/mainwindow.cpp:13367` is switched to the new
accessor as part of this work — a targeted cleanup in code the feature already
touches, not a wider refactor.

## Settle hold: data flow

`switchBand()` (`widgets/mainwindow.cpp:17585`) calls
`m_bandSettleGate.arm(m_config.band_settle_ms(), now)` immediately after
`on_bandComboBox_activated(row)`, guarded by `is_flex_native_rig()`.

**The hard gate** is the PTT decision at `widgets/mainwindow.cpp:8596`:

```cpp
if (g_iptt == 0 and ((m_bTxTime and (fTR < 0.75) and (msgLength > 0)) or m_tune))
```

It gains `and !m_bandSettleGate.active(now)`. This single condition guards the
only place PTT is asserted (`:8646`), and covers both the auto-TX cycle and the
tune carrier. Gating the roughly nineteen `auto_tx_mode(true)` call sites
instead was considered and rejected: fragile, and it misses tune entirely.

**Auto-tune is deferred, not cancelled.** The `m_config.autoTune()` block moves
out of `switchBand()`. `MainWindow` records `m_deferredAutoTune = true`; when
`guiUpdate()` observes the gate has expired it performs the original sequence —
`ui->tuneButton->setChecked(true)`, `on_tuneButton_clicked(true)`,
`tuneButtonTimer.start(4000)`. Same four-second carrier, thirty seconds later.

**Operator override.** `on_tuneButton_clicked` and the Enable Tx handler call
`m_bandSettleGate.override()`, which ends the hold immediately. A tune that
WSJT-Z scheduled itself must not count as an override: the handlers test
`m_deferredAutoTune` and skip `override()` when it is set, clearing the flag
afterwards.

**Feedback.** The countdown appends `SETTLE 23s` to the existing
`mode_switch_status_label`, next to the `BH n->band` text produced around
`widgets/mainwindow.cpp:5104`.

**Configuration.** `band_settle_ms`, default 30000, on the Radio tab. Zero
disables the hold. The thirty seconds is a station property, not a constant.

## Per-band levels: data flow

### New command chains

Two chains, each copying the shape of the existing
`transceiver_tx_rf_power_level` path (`Configuration.hpp:330` →
`TransceiverBase::do_tx_rf_power_level` (`TransceiverBase.hpp:117`) →
`NativeFlexTransceiver::do_tx_rf_power_level` (`NativeFlexTransceiver.cpp:44`),
dispatched from `TransceiverBase.cpp:132`):

- `transceiver_slice_af_gain(int)` → `slice s <n> audio_gain=<v>`
- `transceiver_dax_gain(int value, bool tx)` → the DAX stream gain command

Both follow the existing dispatch guard: send only when the requested value
differs from the last one sent.

### Status ingestion

`NativeFlexTransceiver`'s status parser — the block around
`NativeFlexTransceiver.cpp:428` that already scrapes `max_power_level=` and
`tx_rf_power_changes_allowed=` — gains `audio_gain=` on slice status and the DAX
`rx_gain` / `tx_gain` fields. Each is published as a qApp property alongside the
existing `W7PPNativeFlexRfPower`:

- `W7PPNativeFlexSliceAfGain`
- `W7PPNativeFlexDaxRxGain`
- `W7PPNativeFlexDaxTxGain`

The qApp-property mechanism is inherited from the existing Native FLEX work.
This design follows it for consistency rather than introducing a second,
competing convention.

### Capture and apply

**On arrival at a band**, `switchBand()` calls `apply(band)` and pushes each
field that holds a value. Fields with no stored value are left alone and are
seeded by the next status report from the radio.

**On any level change**, the qApp-property watcher at
`widgets/mainwindow.cpp:2320` gains a branch calling
`capture(band, field, value)`. Because capture is driven by the radio's status
stream rather than by the UI, a change made directly in SmartSDR is recorded
identically to one made with the Pwr slider. This is the intended behaviour: the
last value set for a band wins, wherever it was set.

### Echo suppression

Pushing four values produces four status echoes. Unguarded, `apply()` → echo →
`capture()` writes the same values straight back, and an echo arriving after a
subsequent hop would attribute the previous band's levels to the new band.

`apply()` therefore sets a per-field in-flight guard, cleared when that field's
echo arrives or after a two-second timeout, whichever comes first. Captures for
a guarded field are dropped. This is the principal correctness risk in the
feature and needs deliberate testing.

### Migration

`rf_watts` moves out of the global `W7PPNativeFlexRfWatts` key. On first run
after the upgrade, every band already present in the band list is seeded with
the old global value, so behaviour is unchanged until the operator starts
setting per-band levels. The old key is left in place, unread.

## Persistence

One settings key, `W7PPFlexBandLevels`: a `QHash<QString, QVariant>` keyed by
band string (`"20m"`), each value a variant map of the present fields. Absent
fields are omitted from the map rather than stored as a sentinel.

Saved in `writeSettings()` beside `pwrBandTxMemory`
(`widgets/mainwindow.cpp:1839`); read in `readSettings()` at `:2467`.

## Edge cases

- **Non-Flex rig.** Both features inert. `m_pwrBandTxMemory` and
  `m_pwrBandTuneMemory` behave exactly as today.
- **`tx_rf_power_changes_allowed=0`.** RF watts is captured but never pushed,
  matching the guard already present at `widgets/mainwindow.cpp:13404`.
- **Band change while transmitting.** `switchBand()` already clicks
  `stopTxButton` first; the gate arms after that.
- **Mode leaves the FT8/FT4/FT2 family, or the band hopper is switched off,
  while the gate is armed.** `disarm()`.
- **Hop back to a band during an active hold.** `arm()` again from the new
  change; the deadline is replaced, not extended cumulatively.
- **First visit to a band.** No stored values, nothing pushed; the radio's
  reported values seed the entry.

## Testing

New unit tests, linking `wsjt_qt wsjt_cxx Qt5::Test` and registered in
`tests/CMakeLists.txt` following `test_tx_rf_power_plumbing`:

`tests/test_band_settle_gate.cpp`
- arm then query before the deadline: active
- arm then query after the deadline: inactive
- `override()` during a hold: immediately inactive
- `disarm()` during a hold: immediately inactive
- re-`arm()` during a hold replaces the deadline
- `remaining_seconds` counts down and floors at zero
- `arm(0)` never becomes active

`tests/test_flex_band_levels.cpp`
- capture then apply for the same band returns the value
- capture on one band does not affect another
- apply for an unvisited band returns an empty `LevelSet`
- each of the four fields round-trips independently
- `save`/`load` through `QSettings` preserves all bands and fields
- migration seeds `rf_watts` from the legacy global key
- echo suppression: a capture for a guarded field is dropped; the guard clears
  on echo, and clears on timeout when no echo arrives

Not unit-testable, verified against the radio: the `mainwindow.cpp` wiring, the
exact SmartSDR command strings, and the real status-message field names. This
matches the boundary drawn by the existing Native FLEX tests.

## Build sequence

1. `BandSettleGate` plus its tests.
2. `Configuration::is_flex_native_rig()` and `band_settle_ms` with its Radio-tab
   control.
3. Settle-hold wiring in `mainwindow.cpp`: arm, PTT gate, deferred auto-tune,
   override, status countdown. Verify on the radio.
4. `FlexBandLevels` plus its tests, including migration and echo suppression.
5. The two new command chains through `Configuration` / `TransceiverBase` /
   `NativeFlexTransceiver`.
6. Status ingestion and the three new qApp properties.
7. Capture and apply wiring in `mainwindow.cpp`; move `rf_watts` off the global
   key. Verify on the radio.

Steps 1–3 deliver the settle hold on its own and are independently useful, so
the work can stop or ship there if the level memory proves harder than expected.
