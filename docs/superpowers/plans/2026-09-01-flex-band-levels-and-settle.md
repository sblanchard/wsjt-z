# Per-Band Flex Levels and Antenna Settle Hold Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hold off transmit for 30 seconds after an automatic band change so the antenna can retune, and remember four Flex level settings per band so every hop lands with the right power and gains.

**Architecture:** Two new self-contained units in the `wsjt_qt` library — `BandSettleGate` (a deadline with an override flag) and `FlexBandLevels` (a per-band level store). `MainWindow` wires them in: the gate is consulted at the single PTT-assertion condition, and the level store is fed by the FLEX status stream and drained on band arrival. Two new command chains carry the new gains to the radio, copied from the existing RF-power chain.

**Tech Stack:** C++11, Qt 5 (Core, Widgets, Network, Test), CMake, QTest.

**Spec:** `docs/superpowers/specs/2026-09-01-flex-band-levels-and-settle-design.md`

## Global Constraints

- **C++11 only.** `CMakeLists.txt:1091` sets `-std=c++11`; `tests/CMakeLists.txt` sets `CMAKE_CXX_STANDARD 11`. No `std::optional`, no `auto` return deduction, no generic lambdas, no `std::make_unique`. Unset integers are represented by `-1`.
- **Both features are Flex Native VITA-49 only.** Every entry point is guarded by `m_config.is_flex_native_rig()`. The rig name string is exactly `"Flex Native VITA-49"`.
- **Comment attribution.** Existing Native FLEX code marks donor-derived work `// W7PP :` and local departures `// DEVIATION from W7PP:`. New code in this feature is neither — write ordinary comments without those markers.
- **New library sources go in `wsjt_qt_CXXSRCS`** (`CMakeLists.txt:183`). `widgets/mainwindow.cpp` is not in that library, which is why logic that needs tests must live outside it.
- **Build:** `cmake --build build -j4`  **Test:** `ctest --test-dir build --output-on-failure`
- **Never `git add -A`.** Every commit step names its files explicitly.

---

### Task 1: BandSettleGate

A deadline with an override flag. No Qt widgets, no timer — `MainWindow::guiUpdate()` already ticks at roughly 100 ms and will drive it. The clock is passed in so tests need no event loop.

**Files:**
- Create: `Transceiver/BandSettleGate.hpp`
- Create: `Transceiver/BandSettleGate.cpp`
- Create: `tests/test_band_settle_gate.cpp`
- Modify: `CMakeLists.txt` (add source to `wsjt_qt_CXXSRCS`, near line 221)
- Modify: `tests/CMakeLists.txt` (register the new test)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `class BandSettleGate`
  - `void arm (int ms, qint64 now_ms)`
  - `void override_hold ()`
  - `void disarm ()`
  - `bool active (qint64 now_ms) const`
  - `int remaining_seconds (qint64 now_ms) const`

Note the method is `override_hold()`, not `override()` — `override` is a contextual keyword and reads badly as a member name.

- [ ] **Step 1: Write the failing test**

Create `tests/test_band_settle_gate.cpp`:

```cpp
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
    // 23.5s left rounds up to 23 whole seconds remaining.
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
```

- [ ] **Step 2: Register the test in the build**

In `tests/CMakeLists.txt`, add after the `test_tx_rf_power_plumbing` block (which ends with `add_test (test_tx_rf_power_plumbing test_tx_rf_power_plumbing)`):

```cmake
add_executable (test_band_settle_gate test_band_settle_gate.cpp)
target_include_directories (test_band_settle_gate PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries (test_band_settle_gate wsjt_qt wsjt_cxx Qt5::Test)
if ((NOT ${OPENMP_FOUND}) OR APPLE)
  target_link_libraries (test_band_settle_gate wsjt_fort wsjt_cxx)
else ()
  target_link_libraries (test_band_settle_gate wsjt_fort_omp wsjt_cxx)
endif ()
add_test (test_band_settle_gate test_band_settle_gate)
```

Then add to the existing `if (WIN32)` block at the end of the file, alongside the other `ws2_32` lines:

```cmake
  target_link_libraries (test_band_settle_gate ws2_32)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build build -j4 --target test_band_settle_gate`
Expected: FAIL — `fatal error: 'Transceiver/BandSettleGate.hpp' file not found`

- [ ] **Step 4: Write the header**

Create `Transceiver/BandSettleGate.hpp`:

```cpp
#ifndef BAND_SETTLE_GATE_HPP_
#define BAND_SETTLE_GATE_HPP_

#include <QtGlobal>

//
// Inhibits transmit for a period after an automatic band change, so a
// motorised or remotely tuned antenna can reach the new frequency
// before any RF is applied.
//
// Holds no timer of its own: the caller supplies the clock, which
// keeps the class free of Qt event-loop dependencies and trivially
// testable.
//
class BandSettleGate
{
public:
  // Start a hold of ms milliseconds from now_ms. Replaces any hold
  // already running and clears a previous override. A non-positive
  // ms leaves the gate inactive, which is how the feature is
  // disabled.
  void arm (int ms, qint64 now_ms);

  // The operator asked to transmit anyway. Ends the hold at once.
  void override_hold ();

  // The reason for the hold is gone (band hopper switched off, mode
  // left the FT8 family). Ends the hold at once.
  void disarm ();

  // True while transmit must be inhibited.
  bool active (qint64 now_ms) const;

  // Whole seconds left, rounded up, floored at zero. For display.
  int remaining_seconds (qint64 now_ms) const;

private:
  qint64 deadline_ms_ {0};
  bool overridden_ {false};
};

#endif
```

- [ ] **Step 5: Write the implementation**

Create `Transceiver/BandSettleGate.cpp`:

```cpp
#include "Transceiver/BandSettleGate.hpp"

void BandSettleGate::arm (int ms, qint64 now_ms)
{
  overridden_ = false;

  if (ms <= 0)
    {
      deadline_ms_ = 0;
      return;
    }

  deadline_ms_ = now_ms + ms;
}

void BandSettleGate::override_hold ()
{
  overridden_ = true;
}

void BandSettleGate::disarm ()
{
  deadline_ms_ = 0;
  overridden_ = false;
}

bool BandSettleGate::active (qint64 now_ms) const
{
  if (overridden_ || deadline_ms_ <= 0)
    {
      return false;
    }

  return now_ms < deadline_ms_;
}

int BandSettleGate::remaining_seconds (qint64 now_ms) const
{
  if (!active (now_ms))
    {
      return 0;
    }

  qint64 const left = deadline_ms_ - now_ms;

  // Round up so a hold with 1 ms left still reads "1s", never "0s"
  // while transmit is still inhibited.
  return static_cast<int> ((left + 999) / 1000);
}
```

- [ ] **Step 6: Add the source to the library**

In `CMakeLists.txt`, inside `set (wsjt_qt_CXXSRCS ...)` (starts line 183), add next to the other Transceiver entries near `Transceiver/NativeFlexTransceiver.cpp` (line 221):

```cmake
  Transceiver/BandSettleGate.cpp
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cmake --build build -j4 --target test_band_settle_gate && ctest --test-dir build -R test_band_settle_gate --output-on-failure`
Expected: PASS, 10 test functions.

- [ ] **Step 8: Commit**

```bash
git add Transceiver/BandSettleGate.hpp Transceiver/BandSettleGate.cpp tests/test_band_settle_gate.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add BandSettleGate to inhibit TX while an antenna retunes"
```

---

### Task 2: Configuration support for the settle hold

Adds the rig predicate both features gate on, and the configurable hold duration.

**Files:**
- Modify: `Configuration.hpp` (declare `is_flex_native_rig`, `band_settle_ms`)
- Modify: `Configuration.cpp` (member, accessors, read/write settings, UI read-back)
- Modify: `Configuration.ui` (spin box on the Radio tab)
- Modify: `widgets/mainwindow.cpp:13367` (use the new predicate)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces:
  - `bool Configuration::is_flex_native_rig () const`
  - `int Configuration::band_settle_ms () const`
  - Settings key `"BandSettleMs"`, default `30000`
  - UI widget `band_settle_spin_box`, label `band_settle_label`

- [ ] **Step 1: Declare the accessors**

In `Configuration.hpp`, next to the existing `int flex_dax_channel () const;` (line 94):

```cpp
  // True when the configured rig is the Native FLEX VITA-49 backend,
  // regardless of any RX-method preference. Distinct from
  // flex_native_rx(), which also requires the stored RX preference.
  bool is_flex_native_rig () const;

  // Milliseconds to inhibit transmit after an automatic band change,
  // letting the antenna retune. Zero disables the hold.
  int band_settle_ms () const;
```

- [ ] **Step 2: Add the member and accessors**

In `Configuration.cpp`, next to `int w7pp_flex_dax_channel_ {1};` (line 688):

```cpp
  int band_settle_ms_ {30000};
```

Next to `int Configuration::flex_dax_channel () const {return m_->w7pp_flex_dax_channel_;}` (line 925):

```cpp
bool Configuration::is_flex_native_rig () const
{
  return m_->rig_params_.rig_name == "Flex Native VITA-49";
}

int Configuration::band_settle_ms () const {return m_->band_settle_ms_;}
```

- [ ] **Step 3: Add the UI widget**

In `Configuration.ui`, immediately after the `</widget>` and `</item>` that close `w7pp_flex_dax_channel_combo_box` (its `<item row="4" column="1">` block, which starts at line 1658), add:

```xml
          <item row="5" column="0">
           <widget class="QLabel" name="band_settle_label">
            <property name="text">
             <string>Antenna settle:</string>
            </property>
           </widget>
          </item>
          <item row="5" column="1">
           <widget class="QSpinBox" name="band_settle_spin_box">
            <property name="toolTip">
             <string>Seconds to wait after an automatic band change before transmitting, allowing the antenna to retune. Zero disables the hold.</string>
            </property>
            <property name="suffix">
             <string> s</string>
            </property>
            <property name="minimum">
             <number>0</number>
            </property>
            <property name="maximum">
             <number>300</number>
            </property>
            <property name="value">
             <number>30</number>
            </property>
           </widget>
          </item>
```

The widget is in seconds and the accessor is in milliseconds; the conversion happens in the read/write steps below.

- [ ] **Step 4: Read, write, and populate the setting**

In `Configuration::impl::read_settings ()` (begins `Configuration.cpp:1990`), next to line 1994 (`w7pp_flex_dax_channel_ = settings_->value ("W7PPFlexDaxChannel", 1).toInt ();`):

```cpp
  band_settle_ms_ = settings_->value ("BandSettleMs", 30000).toInt ();
  if (band_settle_ms_ < 0 || band_settle_ms_ > 300000)
    band_settle_ms_ = 30000;
```

In `Configuration::impl::write_settings ()` (begins `Configuration.cpp:2346`), next to line 2350 (`settings_->setValue ("W7PPFlexDaxChannel", w7pp_flex_dax_channel_);`):

```cpp
  settings_->setValue ("BandSettleMs", band_settle_ms_);
```

In `Configuration::impl::initialize_models ()` (begins `Configuration.cpp:1777`), next to line 1849 (`ui_->w7pp_flex_dax_channel_combo_box->setCurrentIndex (...)`):

```cpp
  ui_->band_settle_spin_box->setValue (band_settle_ms_ / 1000);
```

Still inside `read_settings ()`, in the enable/disable block at lines 2308-2309 (`ui_->w7pp_flex_dax_channel_label->setEnabled (native);`):

```cpp
  ui_->band_settle_label->setEnabled (native);
  ui_->band_settle_spin_box->setEnabled (native);
```

In `Configuration::impl::set_rig_invariants ()` (begins `Configuration.cpp:2531`) — the function containing line 2689's `bool const is_flex_native_rig = (rig == "Flex Native VITA-49");` — add after the `w7pp_flex_dax_channel_combo_box->setEnabled (...)` call at line 2691. The settle hold does not depend on the RX method, only on the rig, so it uses the plain predicate rather than the compound one:

```cpp
  ui_->band_settle_spin_box->setEnabled (is_flex_native_rig);
  ui_->band_settle_label->setEnabled (is_flex_native_rig);
```

In `Configuration::impl::accept ()` (begins `Configuration.cpp:2860`), next to lines 2969-2970 (`w7pp_flex_dax_channel_ = ui_->w7pp_flex_dax_channel_combo_box->currentIndex () + 1;`):

```cpp
  band_settle_ms_ = ui_->band_settle_spin_box->value () * 1000;
```

- [ ] **Step 5: Use the predicate at the existing string comparison**

In `widgets/mainwindow.cpp:13367`, replace:

```cpp
  if (m_config.rig_name() == "Flex Native VITA-49")
```

with:

```cpp
  if (m_config.is_flex_native_rig())
```

Leave the other `rig_name()` comparisons alone — this is the one this feature touches.

- [ ] **Step 6: Build to verify it compiles**

Run: `cmake --build build -j4 --target wsjtx`
Expected: builds clean. A `band_settle_spin_box` "no member named" error means the `.ui` edit did not regenerate — delete `build/ui_Configuration.h` and rebuild.

- [ ] **Step 7: Run the full test suite**

Run: `ctest --test-dir build --output-on-failure`
Expected: all existing tests still PASS.

- [ ] **Step 8: Commit**

```bash
git add Configuration.hpp Configuration.cpp Configuration.ui widgets/mainwindow.cpp
git commit -m "feat: add is_flex_native_rig predicate and configurable antenna settle time"
```

---

### Task 3: Wire the settle hold into MainWindow

The gate arms on band change, blocks the single PTT assertion, defers auto-tune, yields to an operator override, and shows a countdown. After this task the settle feature is complete and independently useful.

**Files:**
- Modify: `widgets/mainwindow.h` (include, members)
- Modify: `widgets/mainwindow.cpp` (arm at `:17585`, PTT gate at `:8596`, auto-tune deferral, override in tune/enable-Tx handlers, countdown near `:5104`)

**Interfaces:**
- Consumes: `BandSettleGate` from Task 1; `Configuration::is_flex_native_rig()` and `Configuration::band_settle_ms()` from Task 2.
- Produces: nothing later tasks depend on.

- [ ] **Step 1: Add the members**

In `widgets/mainwindow.h`, near the other Transceiver includes at the top (alongside `#include "WSPR/WSPRBandHopping.hpp"` at line 42):

```cpp
#include "Transceiver/BandSettleGate.hpp"
```

Next to `WSPRBandHopping m_WSPR_band_hopping;` (line 573):

```cpp
  BandSettleGate m_bandSettleGate;
  bool m_deferredAutoTune {false};
```

- [ ] **Step 2: Arm the gate and defer auto-tune in switchBand()**

In `widgets/mainwindow.cpp`, in `MainWindow::switchBand(int row)` (line 17585), replace this block:

```cpp
        if (m_config.autoTune()) {
            ui->tuneButton->setChecked (true);
            on_tuneButton_clicked (true);
            tuneButtonTimer.start(4000);
        }
```

with:

```cpp
        // The antenna needs time to reach the new frequency. Hold off
        // every transmission, the auto-tune carrier included, until it
        // has had it. Flex Native VITA-49 only.
        bool settling = false;
        if (m_config.is_flex_native_rig() && m_config.band_settle_ms() > 0) {
            m_bandSettleGate.arm (m_config.band_settle_ms(),
                                  QDateTime::currentMSecsSinceEpoch());
            settling = m_bandSettleGate.active (QDateTime::currentMSecsSinceEpoch());
        }

        if (m_config.autoTune()) {
            if (settling) {
                // Fired by guiUpdate() when the hold expires.
                m_deferredAutoTune = true;
            } else {
                ui->tuneButton->setChecked (true);
                on_tuneButton_clicked (true);
                tuneButtonTimer.start(4000);
            }
        }
```

- [ ] **Step 3: Gate the PTT assertion**

In `widgets/mainwindow.cpp:8596`, replace:

```cpp
    if(g_iptt==0 and ((m_bTxTime and (fTR < 0.75) and (msgLength>0)) or m_tune)) {
```

with:

```cpp
    if(g_iptt==0 and ((m_bTxTime and (fTR < 0.75) and (msgLength>0)) or m_tune)
       and !m_bandSettleGate.active (QDateTime::currentMSecsSinceEpoch())) {
```

This is the only place PTT is asserted (`:8646`), so it covers the auto-TX cycle and the tune carrier alike.

- [ ] **Step 4: Fire the deferred tune and show the countdown**

In `MainWindow::guiUpdate()`, immediately before the PTT block edited in Step 3, add:

```cpp
  qint64 const settle_now = QDateTime::currentMSecsSinceEpoch();
  if (m_deferredAutoTune && !m_bandSettleGate.active (settle_now)) {
      m_deferredAutoTune = false;
      ui->tuneButton->setChecked (true);
      on_tuneButton_clicked (true);
      tuneButtonTimer.start (4000);
  }
```

In `MainWindow::update_mode_switch_status_label ()` (`widgets/mainwindow.cpp:5069`) — the function containing the `BH %1->%2` construction at line 5104 — add just before the `mode_switch_status_label.setVisible (!parts.isEmpty ());` line near the end:

```cpp
  int const settle_left =
      m_bandSettleGate.remaining_seconds (QDateTime::currentMSecsSinceEpoch());
  if (settle_left > 0) {
      parts << QString {"SETTLE %1s"}.arg (settle_left);
  }
```

- [ ] **Step 5: Let the operator override**

In `MainWindow::on_tuneButton_clicked (bool checked)`, at the very top of the function body:

```cpp
  // A tune the operator asked for ends the antenna settle hold; one
  // WSJT-Z scheduled itself does not.
  if (checked) {
      if (m_deferredAutoTune) {
          m_deferredAutoTune = false;
      } else {
          m_bandSettleGate.override_hold ();
      }
  }
```

Enable Tx is the `autoButton`. `MainWindow::on_autoButton_clicked (bool checked)` (`widgets/mainwindow.cpp:4269`) opens with a Native FLEX safety-trip guard that can `ui->autoButton->setChecked(false)` and **return early**, refusing the request. Do **not** insert at the very top of the function: a refused Enable Tx is not the operator commanding transmit, and clearing the hold there would drop the antenna protection on a click that never enabled TX.

Insert after that guard, on the line that commits to the request — immediately before the existing `if (checked) tx_watchdog(false);`:

```cpp
  // Clicking Enable Tx is the operator saying "transmit anyway", so it
  // ends the antenna settle hold. Placed after the safety-trip guard
  // above: a refused request must not clear the hold.
  if (checked) {
      m_bandSettleGate.override_hold ();
  }
```

- [ ] **Step 6: Disarm when the reason for the hold is gone**

In the `cb_bandHopper` toggled lambda at `widgets/mainwindow.cpp:1259`, inside the lambda body:

```cpp
    if (!ui->cb_bandHopper->isChecked ()) {
        m_bandSettleGate.disarm ();
        m_deferredAutoTune = false;
    }
```

The hold is also meaningless once the mode leaves the FT8 family that `toggleBands()` supports. In `MainWindow::guiUpdate()`, in the block added in Step 4, before the deferred-tune check:

```cpp
  if (m_mode != "FT8" && m_mode != "FT4" && m_mode != "FT2") {
      m_bandSettleGate.disarm ();
      m_deferredAutoTune = false;
  }
```

- [ ] **Step 7: Build and run the full suite**

Run: `cmake --build build -j4 && ctest --test-dir build --output-on-failure`
Expected: builds clean, all tests PASS.

- [ ] **Step 8: Verify against the radio**

This wiring is not unit-testable — verify by hand with the FLEX connected:
1. Enable the band hopper with at least two bands and Auto CQ.
2. Trigger a hop with the Band Change Now button.
3. Confirm the status bar shows `SETTLE 30s` counting down, and that no PTT and no tune carrier occurs during it.
4. Confirm the auto-tune carrier fires once the countdown reaches zero.
5. Hop again, click Tune during the countdown, and confirm the hold ends immediately.
6. Set Antenna settle to 0 in settings, hop, and confirm the old immediate-tune behaviour returns.

- [ ] **Step 9: Commit**

```bash
git add widgets/mainwindow.h widgets/mainwindow.cpp
git commit -m "feat: hold off TX for the antenna settle time after an automatic band change"
```

---

### Task 4: FlexBandLevels

The per-band level store, including the echo-suppression guard and migration off the old global RF-watts key. Pure logic, fully unit tested.

**Files:**
- Create: `Transceiver/FlexBandLevels.hpp`
- Create: `Transceiver/FlexBandLevels.cpp`
- Create: `tests/test_flex_band_levels.cpp`
- Modify: `CMakeLists.txt` (add source to `wsjt_qt_CXXSRCS`)
- Modify: `tests/CMakeLists.txt` (register the test)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `class FlexBandLevels`
  - `enum FlexBandLevels::Field {RfWatts, SliceAfGain, DaxRxGain, DaxTxGain, FieldCount}`
  - `struct FlexBandLevels::LevelSet {int values[FieldCount];}` — `-1` means unset
  - `void capture (QString const& band, Field, int value, qint64 now_ms)`
  - `LevelSet apply (QString const& band, qint64 now_ms)` — non-const; sets the in-flight guards
  - `LevelSet peek (QString const& band) const` — reads without arming guards
  - `void load (QSettings&)`
  - `void save (QSettings&) const`
  - `static QString const& settings_key ()` returning `"W7PPFlexBandLevels"`
  - `static QString const& legacy_watts_key ()` returning `"W7PPNativeFlexRfWatts"`

- [ ] **Step 1: Write the failing test**

Create `tests/test_flex_band_levels.cpp`:

```cpp
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
```

- [ ] **Step 2: Register the test in the build**

In `tests/CMakeLists.txt`, after the `test_band_settle_gate` block:

```cmake
add_executable (test_flex_band_levels test_flex_band_levels.cpp)
target_include_directories (test_flex_band_levels PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries (test_flex_band_levels wsjt_qt wsjt_cxx Qt5::Test)
if ((NOT ${OPENMP_FOUND}) OR APPLE)
  target_link_libraries (test_flex_band_levels wsjt_fort wsjt_cxx)
else ()
  target_link_libraries (test_flex_band_levels wsjt_fort_omp wsjt_cxx)
endif ()
add_test (test_flex_band_levels test_flex_band_levels)
```

And in the `if (WIN32)` block at the end:

```cmake
  target_link_libraries (test_flex_band_levels ws2_32)
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake --build build -j4 --target test_flex_band_levels`
Expected: FAIL — `fatal error: 'Transceiver/FlexBandLevels.hpp' file not found`

- [ ] **Step 4: Write the header**

Create `Transceiver/FlexBandLevels.hpp`:

```cpp
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

  // The levels to push on arrival at a band. Arms the guard for every
  // field that carries a value. Not const: the guards are state.
  LevelSet apply (QString const& band, qint64 now_ms);

  // Read a band's levels without arming anything. Use this to observe;
  // use apply() only when the values are actually going to the radio.
  LevelSet peek (QString const& band) const;

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
```

- [ ] **Step 5: Write the implementation**

Create `Transceiver/FlexBandLevels.cpp`:

```cpp
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

FlexBandLevels::Guard::Guard ()
{
  for (int f = 0; f < FieldCount; ++f)
    {
      until_ms[f] = 0;
    }
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

  QHash<QString, Guard>::iterator g = guards_.find (band);
  if (g != guards_.end () && now_ms < g->until_ms[field])
    {
      // This is our own push coming back. Consume the guard so the
      // next change -- a real one -- is recorded.
      g->until_ms[field] = 0;
      return;
    }

  bands_[band].values[field] = value;
}

FlexBandLevels::LevelSet FlexBandLevels::apply (QString const& band,
                                                qint64 now_ms)
{
  LevelSet set;

  if (band.isEmpty ())
    {
      return set;
    }

  set = peek (band);

  Guard& guard = guards_[band];
  for (int f = 0; f < FieldCount; ++f)
    {
      if (set.values[f] >= 0)
        {
          guard.until_ms[f] = now_ms + guard_ms;
        }
    }

  return set;
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
```

- [ ] **Step 6: Add the source to the library**

In `CMakeLists.txt`, in `wsjt_qt_CXXSRCS` next to the `Transceiver/BandSettleGate.cpp` line added in Task 1:

```cmake
  Transceiver/FlexBandLevels.cpp
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cmake --build build -j4 --target test_flex_band_levels && ctest --test-dir build -R test_flex_band_levels --output-on-failure`
Expected: PASS, 13 test functions.

- [ ] **Step 8: Commit**

```bash
git add Transceiver/FlexBandLevels.hpp Transceiver/FlexBandLevels.cpp tests/test_flex_band_levels.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat: add FlexBandLevels store for per-band Flex power and gains"
```

---

### Task 5: Command chains for slice AF gain and DAX gain

Two new paths from `Configuration` down to `NativeFlexTransceiver`, each an exact copy of the existing `tx_rf_power_level` chain, including its change-detection guard.

**Files:**
- Modify: `Transceiver/Transceiver.hpp` (state fields and accessors)
- Modify: `Transceiver/TransceiverBase.hpp` (virtuals)
- Modify: `Transceiver/TransceiverBase.cpp:132` (dispatch)
- Modify: `Configuration.hpp` (slots)
- Modify: `Configuration.cpp` (impl declarations, forwarders, impl bodies)
- Modify: `Transceiver/NativeFlexTransceiver.hpp` / `.cpp` (overrides)
- Modify: `tests/test_tx_rf_power_plumbing.cpp` (extend for the new fields)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `int Transceiver::TransceiverState::slice_af_gain () const` / `void slice_af_gain (int)`
  - `int Transceiver::TransceiverState::dax_rx_gain () const` / `void dax_rx_gain (int)`
  - `int Transceiver::TransceiverState::dax_tx_gain () const` / `void dax_tx_gain (int)`
  - `virtual void TransceiverBase::do_slice_af_gain (int)`
  - `virtual void TransceiverBase::do_dax_gain (int value, bool tx)`
  - `Q_SLOT void Configuration::transceiver_slice_af_gain (int)`
  - `Q_SLOT void Configuration::transceiver_dax_gain (int value, bool tx)`

- [ ] **Step 1: Extend the failing test**

In `tests/test_tx_rf_power_plumbing.cpp`, add to `RecordingTransceiver` next to `do_tx_rf_power_level`:

```cpp
    QList<int> af_calls;
    QList<int> dax_rx_calls;
    QList<int> dax_tx_calls;
```

```cpp
    void do_slice_af_gain (int value) override
    {
      af_calls.append (value);
    }
    void do_dax_gain (int value, bool tx) override
    {
      if (tx) dax_tx_calls.append (value);
      else dax_rx_calls.append (value);
    }
```

And add these test slots:

```cpp
  void gain_state_defaults_to_unset ()
  {
    Transceiver::TransceiverState state;
    QCOMPARE (state.slice_af_gain (), -1);
    QCOMPARE (state.dax_rx_gain (), -1);
    QCOMPARE (state.dax_tx_gain (), -1);
  }

  void gains_not_in_state_comparison ()
  {
    Transceiver::TransceiverState a;
    Transceiver::TransceiverState b;
    b.slice_af_gain (50);
    b.dax_rx_gain (20);
    b.dax_tx_gain (30);
    QVERIFY (!(a != b));
  }

  void dispatches_only_changed_nonnegative_gains ()
  {
    Transceiver::logger_type logger;
    RecordingTransceiver rig {&logger};

    rig.start (1);

    Transceiver::TransceiverState state;
    state.online (true);

    rig.set (state, 2);
    QCOMPARE (rig.af_calls.size (), 0);
    QCOMPARE (rig.dax_rx_calls.size (), 0);
    QCOMPARE (rig.dax_tx_calls.size (), 0);

    state.slice_af_gain (50);
    state.dax_rx_gain (20);
    state.dax_tx_gain (30);
    rig.set (state, 3);
    QCOMPARE (rig.af_calls, QList<int> {} << 50);
    QCOMPARE (rig.dax_rx_calls, QList<int> {} << 20);
    QCOMPARE (rig.dax_tx_calls, QList<int> {} << 30);

    // Unchanged: not re-dispatched.
    rig.set (state, 4);
    QCOMPARE (rig.af_calls.size (), 1);
    QCOMPARE (rig.dax_rx_calls.size (), 1);
    QCOMPARE (rig.dax_tx_calls.size (), 1);

    state.slice_af_gain (75);
    rig.set (state, 5);
    QCOMPARE (rig.af_calls, QList<int> {} << 50 << 75);
    QCOMPARE (rig.dax_rx_calls.size (), 1);
  }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j4 --target test_tx_rf_power_plumbing`
Expected: FAIL — `no member named 'slice_af_gain' in 'Transceiver::TransceiverState'`

- [ ] **Step 3: Add the state fields**

In `Transceiver/Transceiver.hpp`, next to `, tx_rf_power_level_ {-1}` in the initialiser list (line 111):

```cpp
      , slice_af_gain_ {-1}
      , dax_rx_gain_ {-1}
      , dax_tx_gain_ {-1}
```

Next to `int tx_rf_power_level () const {return tx_rf_power_level_;}` (line 143):

```cpp
    int slice_af_gain () const {return slice_af_gain_;}
    int dax_rx_gain () const {return dax_rx_gain_;}
    int dax_tx_gain () const {return dax_tx_gain_;}
```

Next to `void tx_rf_power_level (int level) {tx_rf_power_level_ = level;}` (line 173):

```cpp
    void slice_af_gain (int v) {slice_af_gain_ = v;}
    void dax_rx_gain (int v) {dax_rx_gain_ = v;}
    void dax_tx_gain (int v) {dax_tx_gain_ = v;}
```

Next to `int tx_rf_power_level_;` (line 207):

```cpp
    int slice_af_gain_;
    int dax_rx_gain_;
    int dax_tx_gain_;
```

Do **not** add these to `operator!=`. Like `tx_rf_power_level_`, they are commands rather than rig-reported state, and including them would cause poll ping-pong. The `gains_not_in_state_comparison` test pins this.

- [ ] **Step 4: Add the virtuals**

In `Transceiver/TransceiverBase.hpp`, next to `virtual void do_tx_rf_power_level (int) {}` (line 117):

```cpp
  // Native FLEX slice receive audio gain (SmartSDR audio_gain 0-100).
  // Default no-op so only rigs with real support implement it.
  virtual void do_slice_af_gain (int) {}

  // Native FLEX DAX stream gain, tx selecting the TX stream over RX.
  virtual void do_dax_gain (int, bool) {}
```

- [ ] **Step 5: Dispatch them**

In `Transceiver/TransceiverBase.cpp`, immediately after the `tx_rf_power_level` block ending at line 136:

```cpp
          // Gains follow the power rule: no audio_cmd, so a gain
          // change cannot suppress PTT processing in the same
          // transaction.
          if (s.slice_af_gain () >= 0
              && requested_.slice_af_gain () != s.slice_af_gain ()) {
            do_slice_af_gain (s.slice_af_gain ());
            requested_.slice_af_gain (s.slice_af_gain ());
          }
          if (s.dax_rx_gain () >= 0
              && requested_.dax_rx_gain () != s.dax_rx_gain ()) {
            do_dax_gain (s.dax_rx_gain (), false);
            requested_.dax_rx_gain (s.dax_rx_gain ());
          }
          if (s.dax_tx_gain () >= 0
              && requested_.dax_tx_gain () != s.dax_tx_gain ()) {
            do_dax_gain (s.dax_tx_gain (), true);
            requested_.dax_tx_gain (s.dax_tx_gain ());
          }
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cmake --build build -j4 --target test_tx_rf_power_plumbing && ctest --test-dir build -R test_tx_rf_power_plumbing --output-on-failure`
Expected: PASS.

- [ ] **Step 7: Add the Configuration slots**

In `Configuration.hpp`, next to `Q_SLOT void transceiver_tx_rf_power_level (int level);` (line 330):

```cpp
  // Native FLEX slice receive audio gain, SmartSDR audio_gain 0-100.
  Q_SLOT void transceiver_slice_af_gain (int gain);

  // Native FLEX DAX stream gain, tx selecting the TX stream over RX.
  Q_SLOT void transceiver_dax_gain (int gain, bool tx);
```

In `Configuration.cpp`, next to `void transceiver_tx_rf_power_level (int);` in the impl class (line 494):

```cpp
  void transceiver_slice_af_gain (int);
  void transceiver_dax_gain (int, bool);
```

Next to `Configuration::transceiver_tx_rf_power_level` (line 1184):

```cpp
void Configuration::transceiver_slice_af_gain (int gain)
{
  LOG_TRACE (gain << ' ' << m_->cached_rig_state_);
  m_->transceiver_slice_af_gain (gain);
}

void Configuration::transceiver_dax_gain (int gain, bool tx)
{
  LOG_TRACE (gain << ' ' << tx << ' ' << m_->cached_rig_state_);
  m_->transceiver_dax_gain (gain, tx);
}
```

Next to `Configuration::impl::transceiver_tx_rf_power_level` (line 4272):

```cpp
void Configuration::impl::transceiver_slice_af_gain (int gain)
{
  cached_rig_state_.online (true);
  set_cached_mode ();
  cached_rig_state_.slice_af_gain (gain);
  Q_EMIT set_transceiver (cached_rig_state_, ++transceiver_command_number_);
}

void Configuration::impl::transceiver_dax_gain (int gain, bool tx)
{
  cached_rig_state_.online (true);
  set_cached_mode ();
  if (tx) cached_rig_state_.dax_tx_gain (gain);
  else cached_rig_state_.dax_rx_gain (gain);
  Q_EMIT set_transceiver (cached_rig_state_, ++transceiver_command_number_);
}
```

- [ ] **Step 8: Implement the overrides on the Flex transceiver**

In `Transceiver/NativeFlexTransceiver.hpp`, next to `void do_tx_rf_power_level(int) override;` (line 57):

```cpp
  void do_slice_af_gain(int) override;
  void do_dax_gain(int, bool) override;
```

In `Transceiver/NativeFlexTransceiver.cpp`, immediately after `do_tx_rf_power_level` (which ends around line 52):

```cpp
void NativeFlexTransceiver::do_slice_af_gain(int gain)
{
  if (gain < 0 || gain > 100)
    {
      throw error {"Native FLEX slice audio gain out of range"};
    }

  if (slice_id_ < 0)
    {
      // No slice yet: the level is re-pushed on the next band arrival.
      return;
    }

  send_command(
      QStringLiteral("slice s %1 audio_gain=%2")
          .arg(slice_id_)
          .arg(gain));
}

void NativeFlexTransceiver::do_dax_gain(int gain, bool tx)
{
  if (gain < 0 || gain > 100)
    {
      throw error {"Native FLEX DAX gain out of range"};
    }

  if (tx)
    {
      if (!dax_tx_stream_id_)
        {
          return;
        }

      send_command(
          QStringLiteral("dax tx %1")
              .arg(gain));
      return;
    }

  send_command(
      QStringLiteral("audio stream 0x%1 slice %2 gain %3")
          .arg(dax_channel_, 0, 16)
          .arg(slice_id_ < 0 ? 0 : slice_id_)
          .arg(gain));
}
```

The exact SmartSDR command strings cannot be unit-tested and are verified against the radio in Task 7, Step 5.

- [ ] **Step 9: Build and run the full suite**

Run: `cmake --build build -j4 && ctest --test-dir build --output-on-failure`
Expected: builds clean, all tests PASS.

- [ ] **Step 10: Commit**

```bash
git add Transceiver/Transceiver.hpp Transceiver/TransceiverBase.hpp Transceiver/TransceiverBase.cpp Configuration.hpp Configuration.cpp Transceiver/NativeFlexTransceiver.hpp Transceiver/NativeFlexTransceiver.cpp tests/test_tx_rf_power_plumbing.cpp
git commit -m "feat: add slice AF gain and DAX gain command chains for Native FLEX"
```

---

### Task 6: Ingest the new gains from the FLEX status stream

Publishes three new qApp properties so `MainWindow` learns about changes made anywhere, including in SmartSDR itself.

**Files:**
- Modify: `Transceiver/NativeFlexTransceiver.hpp` (declare `capture_gain_status`)
- Modify: `Transceiver/NativeFlexTransceiver.cpp` (implement it, call it from the four read loops)

**Interfaces:**
- Consumes: nothing.
- Produces: qApp properties `W7PPNativeFlexSliceAfGain`, `W7PPNativeFlexDaxRxGain`, `W7PPNativeFlexDaxTxGain`, each an `int` 0-100.

- [ ] **Step 1: Declare the capture function**

In `Transceiver/NativeFlexTransceiver.hpp`, next to `void capture_transmit_status(QByteArray const& line);` (line ~61):

```cpp
  void capture_gain_status(QByteArray const& line);
```

- [ ] **Step 2: Implement it**

In `Transceiver/NativeFlexTransceiver.cpp`, immediately after `capture_transmit_status` (which ends around line 522):

```cpp
void NativeFlexTransceiver::capture_gain_status(
    QByteArray const& line)
{
  //
  // Publish the receive and transmit audio gains the radio reports,
  // so a level the operator changes in SmartSDR is seen here exactly
  // like one changed in WSJT-Z.
  //
  // Read-only with respect to the radio.
  //
  if (!line.startsWith('S'))
    {
      return;
    }

  int const pipe = line.indexOf('|');

  if (pipe <= 1)
    {
      return;
    }

  QByteArray const payload = line.mid(pipe + 1).trimmed();

  QCoreApplication * const app =
      QCoreApplication::instance();

  if (!app)
    {
      return;
    }

  QByteArray property;
  QByteArray marker;

  if (payload.startsWith("slice "))
    {
      // Only our own slice speaks for the current band.
      if (slice_id_ < 0
          || !payload.startsWith(
                 QStringLiteral("slice %1 ")
                     .arg(slice_id_)
                     .toLatin1()))
        {
          return;
        }

      marker = "audio_gain=";
      property = "W7PPNativeFlexSliceAfGain";
    }
  else if (payload.startsWith("dax "))
    {
      if (payload.contains("tx_gain="))
        {
          marker = "tx_gain=";
          property = "W7PPNativeFlexDaxTxGain";
        }
      else
        {
          marker = "rx_gain=";
          property = "W7PPNativeFlexDaxRxGain";
        }
    }
  else
    {
      return;
    }

  for (QByteArray const& field : payload.split(' '))
    {
      if (!field.startsWith(marker))
        {
          continue;
        }

      bool ok = false;

      int const value =
          QString::fromLatin1(
              field.mid(marker.size()))
              .toInt(&ok);

      if (!ok || value < 0 || value > 100)
        {
          continue;
        }

      app->setProperty(
          property.constData(),
          QVariant::fromValue(value));
      return;
    }
}
```

- [ ] **Step 3: Call it from every read loop**

`capture_transmit_status` is called from four separate read loops. Add `capture_gain_status(line);` immediately after **every** `capture_transmit_status(line);` call.

Find them with `grep -n "capture_transmit_status(line);" Transceiver/NativeFlexTransceiver.cpp` and expect **exactly four** call sites. Do not use line numbers for this: Task 5 inserts roughly 45 lines into this same file above all four sites, so any number quoted here is stale by the time you run. The count is the check — four calls before, four `capture_gain_status(line);` after.

While at line 675, note the pre-existing duplicated `capture_dax_tx_stream(line);` on consecutive lines 674-675. Leave it alone — it is harmless and out of scope for this feature.

- [ ] **Step 4: Build and run the full suite**

Run: `cmake --build build -j4 && ctest --test-dir build --output-on-failure`
Expected: builds clean, all tests PASS.

- [ ] **Step 5: Verify the properties appear**

With the FLEX connected, start WSJT-Z and confirm `build/W7PP_NATIVE_FLEX_TX_STATUS.txt` is still written and that gain changes made in SmartSDR reach the app. If a property never populates, the field name in the status stream differs from the guess above — capture a few raw status lines and correct the markers before continuing to Task 7.

- [ ] **Step 6: Commit**

```bash
git add Transceiver/NativeFlexTransceiver.hpp Transceiver/NativeFlexTransceiver.cpp
git commit -m "feat: publish Native FLEX slice and DAX gains from the status stream"
```

---

### Task 7: Wire per-band levels into MainWindow

Captures level changes as the radio reports them, restores them on band arrival, and moves RF watts off the old global key. Completes the feature.

**Files:**
- Modify: `widgets/mainwindow.h` (include, member)
- Modify: `widgets/mainwindow.cpp` (settings read/write, capture polling, apply in `switchBand()`, remove the global watts write at `:13421`)

**Interfaces:**
- Consumes: `FlexBandLevels` from Task 4; the Configuration slots from Task 5; the qApp properties from Task 6.
- Produces: nothing.

- [ ] **Step 1: Add the member**

In `widgets/mainwindow.h`, next to the `BandSettleGate` include added in Task 3:

```cpp
#include "Transceiver/FlexBandLevels.hpp"
```

Next to `BandSettleGate m_bandSettleGate;`:

```cpp
  FlexBandLevels m_flexBandLevels;
```

- [ ] **Step 2: Load and save the store**

In `MainWindow::readSettings()`, next to line 2467 (`m_pwrBandTxMemory=m_settings->value("pwrBandTxMemory").toHash();`):

`models/Bands` is a table model with no band-name list accessor, so enumerate the band combo box, which holds exactly the bands the operator can reach:

```cpp
  m_flexBandLevels.load (*m_settings);

  QStringList all_bands;
  for (int i = 0; i < ui->bandComboBox->count (); ++i) {
      QString const b = ui->bandComboBox->itemText (i).trimmed ();
      if (!b.isEmpty ()) all_bands << b;
  }
  m_flexBandLevels.migrate_legacy_watts (*m_settings, all_bands);
```

In `MainWindow::writeSettings()`, next to line 1839 (`m_settings->setValue("pwrBandTxMemory",m_pwrBandTxMemory);`):

```cpp
  m_flexBandLevels.save (*m_settings);
```

- [ ] **Step 3: Capture reported levels**

Add a private slot to `widgets/mainwindow.h` next to the other private slots:

```cpp
  void pollFlexBandLevels ();
```

Implement it in `widgets/mainwindow.cpp`, next to `on_outAttenuation_valueChanged`:

```cpp
// Record whatever the radio currently reports for this band. Driven by
// the FLEX status stream via qApp properties, so a level changed in
// SmartSDR is captured exactly like one changed here.
void MainWindow::pollFlexBandLevels ()
{
  if (!m_config.is_flex_native_rig ()) return;

  QString const band = ui->bandComboBox->currentText ().trimmed ();
  if (band.isEmpty ()) return;

  qint64 const now = QDateTime::currentMSecsSinceEpoch ();

  struct {
    char const * property;
    FlexBandLevels::Field field;
  } const sources[] = {
    {"W7PPNativeFlexSliceAfGain", FlexBandLevels::SliceAfGain},
    {"W7PPNativeFlexDaxRxGain",   FlexBandLevels::DaxRxGain},
    {"W7PPNativeFlexDaxTxGain",   FlexBandLevels::DaxTxGain}
  };

  for (unsigned i = 0; i < sizeof (sources) / sizeof (sources[0]); ++i) {
      bool ok = false;
      int const value = qApp->property (sources[i].property).toInt (&ok);
      if (ok && value >= 0) {
          m_flexBandLevels.capture (band, sources[i].field, value, now);
      }
  }

  // RF watts is reported as an rfpower percentage; store the watts the
  // operator actually sees on the slider.
  bool max_ok = false;
  int const max_watts =
      qApp->property ("W7PPNativeFlexMaxInternalPaPower").toInt (&max_ok);
  bool rf_ok = false;
  int const rf_level = qApp->property ("W7PPNativeFlexRfPower").toInt (&rf_ok);

  if (max_ok && max_watts > 0 && rf_ok && rf_level >= 0 && rf_level <= 100) {
      int const watts =
          qBound (0, qRound (double (rf_level) * double (max_watts) / 100.0),
                  max_watts);
      m_flexBandLevels.capture (band, FlexBandLevels::RfWatts, watts, now);
  }
}
```

Add a companion member to `widgets/mainwindow.h` next to `m_deferredAutoTune`:

```cpp
  int m_lastLevelPollSec {-1};
```

`guiUpdate()` runs about every 100 ms, which is more often than these levels can meaningfully change. Drive the poll once a second from the `nsec` already computed at `widgets/mainwindow.cpp:8426`, immediately after the `update_mode_switch_status_label ();` call at line 8432:

```cpp
  if (nsec != m_lastLevelPollSec) {
      m_lastLevelPollSec = nsec;
      pollFlexBandLevels ();
  }
```

- [ ] **Step 4: Apply on band arrival**

In `MainWindow::switchBand(int row)`, immediately after the settle-gate block added in Task 3:

```cpp
        if (m_config.is_flex_native_rig()) {
            QString const new_band = ui->bandComboBox->currentText ().trimmed ();
            FlexBandLevels::LevelSet const levels =
                m_flexBandLevels.apply (new_band,
                                        QDateTime::currentMSecsSinceEpoch ());

            bool allowed_ok = false;
            int const changes_allowed =
                qApp->property ("W7PPNativeFlexRfPowerChangesAllowed")
                    .toInt (&allowed_ok);
            bool max_ok = false;
            int const max_watts =
                qApp->property ("W7PPNativeFlexMaxInternalPaPower")
                    .toInt (&max_ok);

            if (levels.values[FlexBandLevels::RfWatts] >= 0
                && allowed_ok && changes_allowed != 0
                && max_ok && max_watts > 0) {
                int const watts =
                    qBound (0, levels.values[FlexBandLevels::RfWatts], max_watts);
                m_block_pwr_tooltip = true;
                ui->outAttenuation->setValue (watts);
                m_block_pwr_tooltip = false;
                m_config.transceiver_tx_rf_power_level (
                    qBound (0, qRound (double (watts) * 100.0 / double (max_watts)),
                            100));
            }

            if (levels.values[FlexBandLevels::SliceAfGain] >= 0) {
                m_config.transceiver_slice_af_gain (
                    levels.values[FlexBandLevels::SliceAfGain]);
            }
            if (levels.values[FlexBandLevels::DaxRxGain] >= 0) {
                m_config.transceiver_dax_gain (
                    levels.values[FlexBandLevels::DaxRxGain], false);
            }
            if (levels.values[FlexBandLevels::DaxTxGain] >= 0) {
                m_config.transceiver_dax_gain (
                    levels.values[FlexBandLevels::DaxTxGain], true);
            }
        }
```

- [ ] **Step 5: Move RF watts off the global key**

In `on_outAttenuation_valueChanged` (`widgets/mainwindow.cpp:13363`), replace the global write near line 13421:

```cpp
      m_settings->setValue(
          "W7PPNativeFlexRfWatts",
          a);
```

with:

```cpp
      m_flexBandLevels.capture (
          ui->bandComboBox->currentText ().trimmed (),
          FlexBandLevels::RfWatts,
          a,
          QDateTime::currentMSecsSinceEpoch ());
```

Then in the startup restore lambda at `widgets/mainwindow.cpp:2374`, replace:

```cpp
              int const saved_watts =
                  m_settings->value(
                      "W7PPNativeFlexRfWatts",
                      current_watts).toInt();
```

with:

```cpp
              FlexBandLevels::LevelSet const startup_levels =
                  m_flexBandLevels.apply(
                      ui->bandComboBox->currentText().trimmed(),
                      QDateTime::currentMSecsSinceEpoch());

              int const stored_watts =
                  startup_levels.values[FlexBandLevels::RfWatts];

              int const saved_watts =
                  stored_watts >= 0 ? stored_watts : current_watts;
```

- [ ] **Step 6: Build and run the full suite**

Run: `cmake --build build -j4 && ctest --test-dir build --output-on-failure`
Expected: builds clean, all tests PASS.

- [ ] **Step 7: Verify against the radio**

With the FLEX connected:
1. On 20m, set RF watts on the slider and set slice AF gain and DAX gains in SmartSDR to distinctive values.
2. Hop to 40m, set clearly different values there.
3. Hop back to 20m and confirm all four values return.
4. Restart WSJT-Z and confirm they survive.
5. Confirm no oscillation: watch the values for a minute on one band and check nothing drifts or flip-flops. Drift means the echo guard in `FlexBandLevels::capture` is not matching the real echo timing — raise `guard_ms`.
6. Confirm the first hop to a never-visited band pushes nothing and simply adopts the radio's current levels.

- [ ] **Step 8: Commit**

```bash
git add widgets/mainwindow.h widgets/mainwindow.cpp
git commit -m "feat: remember Flex power and gain levels per band across hops"
```

---

## Notes for the implementer

**Line numbers drift.** Every reference is against the tree at commit `33ea73b`. After each task the file grows; re-locate by the quoted code, not the number.

**The two riskiest steps** are Task 6 Step 5 (the SmartSDR status field names are informed guesses — verify them against real status lines before building on them) and Task 7 Step 7 item 5 (echo oscillation). Both are radio-verification steps, and both have a stated remedy.

**Stopping early is fine.** Tasks 1-3 deliver the settle hold complete and useful on their own. Tasks 4-7 deliver the level memory. Nothing in 1-3 depends on 4-7.
