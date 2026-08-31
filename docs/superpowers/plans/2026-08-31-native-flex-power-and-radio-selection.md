# Native FLEX RF Power Control and Radio Selection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three remaining Native FLEX gaps: deliver RF power commands from the (already repurposed) watts slider to the radio, persist the selected radio across restarts, and replace the blocking 3.5 s discovery scan with an async dialog that handles multiple radios and direct connect-by-IP.

**Architecture:** RF power rides the existing `TransceiverState` → `set_transceiver` → `TransceiverBase::set()` command path as a new client-only state member dispatched change-detected to a `do_tx_rf_power_level(int)` virtual — the exact pattern the TCI extensions (`nsym`, `audio`, `tune`) already use. `NativeFlexTransceiver::do_tx_rf_power_level` **already exists and sends `transmit set rfpower=`**; it just becomes an override. Radio selection gains a `restore()` entry seeded from `QSettings` by `Configuration`, and `refresh()` is rebuilt on a signal-driven `NativeFlexDiscovery` engine plus a live-updating modal dialog with a manual `host[:port]` field.

**Tech Stack:** Qt 5.15 (Widgets, Network, Test), C++11/14 as per file, CMake. Tests follow `tests/` conventions (QTest, linked against `wsjt_qt`).

**Spec:** `docs/superpowers/specs/2026-08-30-native-flex-direct-vita-design.md` (original port design; this plan implements its stated "Known limitations" follow-ups). User requirements added 2026-08-31: support multiple radios on the network and direct connection by IP address.

## Global Constraints

- Rig name string is exactly `"Flex Native VITA-49"` everywhere.
- W7PP attribution stays intact; new W7PP-adjacent code carries `// W7PP :` style comments only where it extends donor behaviour.
- Native FLEX code must remain inert when the rig is not `"Flex Native VITA-49"` (inertness is an acceptance criterion of the original port).
- SmartSDR `rfpower` is a **percent 0–100**; the slider shows **watts 0–max_watts** (`W7PPNativeFlexMaxInternalPaPower`). Conversions: watts→percent `qRound(w*100.0/max)`, percent→watts `qRound(p*max/100.0)`, both `qBound`ed.
- `TransceiverState::operator!=` compares only rig-reported fields — do **NOT** add the new power member to it (matches `audio_`/`tune_` precedent; prevents poll ping-pong).
- Settings keys already in use and reused here: `W7PPNativeFlexRfWatts` (operator watts, MainWindow). New keys: `FlexNativeRadioModel`, `FlexNativeRadioSerial`, `FlexNativeRadioAddress`, `FlexNativeRadioPort` (Configuration).
- New source files compile into the `wsjt_qt` target (source list at `CMakeLists.txt:217-219`).
- All existing tests must keep passing: `test_qt_helpers`, `test_flex_socket_compat`, `test_flex_vita_receiver`, `test_native_flex_factory`.

---

### Task 1: `tx_rf_power_level` in TransceiverState and TransceiverBase

**Files:**
- Modify: `Transceiver/Transceiver.hpp` (state member, accessors, debug prints declaration comment)
- Modify: `Transceiver/Transceiver.cpp` (QDebug/ostream prints)
- Modify: `Transceiver/TransceiverBase.hpp` (new virtual)
- Modify: `Transceiver/TransceiverBase.cpp` (change-detected dispatch in `set()`)
- Modify: `Transceiver/NativeFlexTransceiver.hpp:57` (`override`)
- Create: `tests/test_tx_rf_power_plumbing.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `TransceiverBase::set()` dispatch structure (`TransceiverBase.cpp`, inside `if (requested_.online ())`).
- Produces, for Task 2: `TransceiverState::tx_rf_power_level () -> int` / `tx_rf_power_level (int)` (−1 = no request), and virtual `void TransceiverBase::do_tx_rf_power_level (int)` (default no-op) fired once per changed value ≥ 0.

- [ ] **Step 1: Write the failing test**

`tests/test_tx_rf_power_plumbing.cpp`:

```cpp
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
```

Append to `tests/CMakeLists.txt` after the `test_native_flex_factory` block:

```cmake
add_executable (test_tx_rf_power_plumbing test_tx_rf_power_plumbing.cpp)
target_link_libraries (test_tx_rf_power_plumbing wsjt_qt Qt5::Test)
add_test (test_tx_rf_power_plumbing test_tx_rf_power_plumbing)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j"$(nproc)" --target test_tx_rf_power_plumbing`
Expected: compile FAILURE — `tx_rf_power_level` is not a member of `TransceiverState`, `do_tx_rf_power_level` does not override.

- [ ] **Step 3: Write minimal implementation**

`Transceiver/Transceiver.hpp` — in `TransceiverState`:
- constructor init list, after `, fastmode_ {false}  //w3sz tci` (line ~110): add `, tx_rf_power_level_ {-1}`.
- getters, after `bool fastmode () const ...` (line ~141): `int tx_rf_power_level () const {return tx_rf_power_level_;}`
- setters, after `void fastmode(bool fastmode) ...` (line ~170): `void tx_rf_power_level (int level) {tx_rf_power_level_ = level;}`
- members, after `bool fastmode_;  //w3sz tci`: `int tx_rf_power_level_;   // percent 0-100, -1 = no request; client-only, NOT in operator!=`

`Transceiver/Transceiver.cpp` — extend both stream operators (QDebug at line ~14 and `std::ostream` at ~32) with `<< "; RFPWR: " << s.tx_rf_power_level_` before the closing `')'`. Do **not** touch `operator!=`.

`Transceiver/TransceiverBase.hpp` — after `virtual void do_ptt (bool = true) = 0;` (line 113):

```cpp
  // W7PP : Native FLEX RF power (SmartSDR rfpower percent 0-100).
  // Default no-op so only rigs with real support implement it.
  virtual void do_tx_rf_power_level (int) {}
```

`Transceiver/TransceiverBase.cpp` — in `set()`, inside `if (requested_.online ())`, immediately after the `tune` block (`if (requested_.tune() != s.tune()) {...}`) and before `if (!audio_cmd) {`:

```cpp
          if (s.tx_rf_power_level () >= 0
              && requested_.tx_rf_power_level () != s.tx_rf_power_level ()) {
            do_tx_rf_power_level (s.tx_rf_power_level ());
            requested_.tx_rf_power_level (s.tx_rf_power_level ());
          }
```

(No `audio_cmd = true`: a power change must not suppress PTT processing in the same transaction.)

`Transceiver/NativeFlexTransceiver.hpp:57` — change `void do_tx_rf_power_level(int);` to `void do_tx_rf_power_level(int) override;`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j"$(nproc)" --target test_tx_rf_power_plumbing && ./build/tests/test_tx_rf_power_plumbing`
Expected: all three test functions PASS.

- [ ] **Step 5: Run the full existing test set and application build**

Run: `cmake --build build -j"$(nproc)" && for t in test_qt_helpers test_flex_socket_compat test_flex_vita_receiver test_native_flex_factory; do ./build/tests/$t || break; done`
Expected: full build succeeds; all suites pass.

- [ ] **Step 6: Commit**

```bash
git add Transceiver/Transceiver.hpp Transceiver/Transceiver.cpp \
  Transceiver/TransceiverBase.hpp Transceiver/TransceiverBase.cpp \
  Transceiver/NativeFlexTransceiver.hpp tests/test_tx_rf_power_plumbing.cpp tests/CMakeLists.txt
git commit -m "feat: dispatch tx_rf_power_level through TransceiverBase state path"
```

---

### Task 2: Configuration command chain

**Files:**
- Modify: `Configuration.hpp` (public Q_SLOT, near `transceiver_ptt` at line ~327)
- Modify: `Configuration.cpp` (public wrapper near line 1175; impl declaration beside the other `transceiver_*` impl members; impl definition after `impl::transceiver_ptt` at line ~4207)

**Interfaces:**
- Consumes: `TransceiverState::tx_rf_power_level (int)` from Task 1; existing `set_transceiver` signal / `cached_rig_state_` / `transceiver_command_number_` plumbing.
- Produces, for Task 3: `Q_SLOT void Configuration::transceiver_tx_rf_power_level (int level)` — level is SmartSDR percent 0–100.

- [ ] **Step 1: Add the declarations**

`Configuration.hpp`, after `Q_SLOT void transceiver_ptt (bool = true);`:

```cpp
  // W7PP : Native FLEX RF power, SmartSDR rfpower percent 0-100.
  Q_SLOT void transceiver_tx_rf_power_level (int level);
```

`Configuration.cpp`, in `class Configuration::impl`, beside the existing `void transceiver_ptt (bool);` declaration:

```cpp
  void transceiver_tx_rf_power_level (int);
```

- [ ] **Step 2: Add the definitions**

`Configuration.cpp`, after the public `Configuration::transceiver_ptt` definition (line ~1179):

```cpp
void Configuration::transceiver_tx_rf_power_level (int level)
{
  m_->transceiver_tx_rf_power_level (level);
}
```

After `Configuration::impl::transceiver_ptt` (line ~4215):

```cpp
void Configuration::impl::transceiver_tx_rf_power_level (int level)
{
  cached_rig_state_.online (true);
  set_cached_mode ();
  cached_rig_state_.tx_rf_power_level (level);
  Q_EMIT set_transceiver (cached_rig_state_, ++transceiver_command_number_);
}
```

(The level persists in `cached_rig_state_`; `TransceiverBase::set()` change-detection makes later unrelated transactions carrying the same value a no-op.)

- [ ] **Step 3: Verify it builds and tests pass**

Run: `cmake --build build -j"$(nproc)" && ./build/tests/test_tx_rf_power_plumbing`
Expected: clean build, tests pass.

- [ ] **Step 4: Commit**

```bash
git add Configuration.hpp Configuration.cpp
git commit -m "feat: add Configuration::transceiver_tx_rf_power_level command chain"
```

---

### Task 3: Enable the watts slider and send power

**Files:**
- Modify: `widgets/mainwindow.cpp` — two DEVIATION blocks: the `refresh_native_flex_power_ui` lambda (line ~2320-2400) and `on_outAttenuation_valueChanged` (line ~13280-13341)

**Interfaces:**
- Consumes: `m_config.transceiver_tx_rf_power_level (int)` from Task 2; application properties `W7PPNativeFlexMaxInternalPaPower`, `W7PPNativeFlexRfPower`, `W7PPNativeFlexRfPowerChangesAllowed` (already published by `NativeFlexTransceiver`); settings key `W7PPNativeFlexRfWatts` (already written by `writeSettings`/`on_outAttenuation` paths).
- Produces: operator-usable RF power slider in Native FLEX mode.

- [ ] **Step 1: Rework the `refresh_native_flex_power_ui` lambda**

In the lambda, first make the changes-allowed value usable — replace:

```cpp
          // The changes-allowed value itself is unused below (see the
          // DEVIATION note); only its presence still gates PowerReady.
          qApp->property(
              "W7PPNativeFlexRfPowerChangesAllowed")
              .toInt(&allowed_ok);
```

with:

```cpp
          int const changes_allowed =
              qApp->property(
                  "W7PPNativeFlexRfPowerChangesAllowed")
                  .toInt(&allowed_ok);
```

Then replace everything from `ui->outAttenuation->setMaximum(max_watts);` down to (and including) the `setToolTip(tr("Native FLEX RF power control is not available in this build"));` call — i.e. the whole DEVIATION block, keeping the final `setProperty("w7ppNativeFlexPowerReady", true);` — with:

```cpp
          ui->outAttenuation->setMaximum(max_watts);

          // W7PP : restore the operator's saved watts when the radio
          // permits power changes; otherwise show the radio's own
          // report and keep the control disabled.
          int display_watts = current_watts;

          if (changes_allowed != 0)
            {
              int const saved_watts =
                  m_settings->value(
                      "W7PPNativeFlexRfWatts",
                      current_watts).toInt();

              display_watts =
                  qBound(0, saved_watts, max_watts);
            }

          m_block_pwr_tooltip = true;
          ui->outAttenuation->setValue(display_watts);
          m_block_pwr_tooltip = false;

          if (changes_allowed != 0)
            {
              // Deliver the restored value so slider and radio agree
              // before the operator takes over.
              if (display_watts != current_watts)
                {
                  m_config.transceiver_tx_rf_power_level(
                      qBound(
                          0,
                          qRound(
                              double(display_watts)
                              * 100.0
                              / double(max_watts)),
                          100));
                }

              ui->outAttenuation->setEnabled(true);
              ui->outAttenuation->setToolTip(
                  tr("Set Native FLEX RF transmit power"));
            }
          else
            {
              ui->outAttenuation->setEnabled(false);
              ui->outAttenuation->setToolTip(
                  tr("The radio does not currently allow RF power changes"));
            }
```

- [ ] **Step 2: Send from `on_outAttenuation_valueChanged`**

Replace the DEVIATION comment block and its bare `// Do NOT fall through...` / `return;` tail (everything after the `changes_allowed == 0` early-return closes) with:

```cpp
      // W7PP : deliver the watts as SmartSDR rfpower percent and
      // remember the operator's choice.
      m_config.transceiver_tx_rf_power_level(
          qBound(
              0,
              qRound(
                  double(a)
                  * 100.0
                  / double(max_watts)),
              100));

      m_settings->setValue(
          "W7PPNativeFlexRfWatts",
          a);

      // Do NOT fall through to SoundOutput attenuation.
      return;
```

Note the guards already above this point (unchanged): programmatic changes blocked by `m_block_pwr_tooltip`/focus/enabled checks; `changes_allowed == 0` returns early. The existing early-return for `!max_ok || max_watts <= 0 || !allowed_ok || changes_allowed == 0` stays.

- [ ] **Step 3: Update the README limitation**

`README.md` "Known limitations": remove the "RF power control is unavailable" bullet; add under Status: `- **RF power** — the power slider sets the radio's RF output (watts, scaled to the PA capability the radio reports)`.

- [ ] **Step 4: Build and run all tests**

Run: `cmake --build build -j"$(nproc)" && for t in test_qt_helpers test_flex_socket_compat test_flex_vita_receiver test_native_flex_factory test_tx_rf_power_plumbing; do ./build/tests/$t || break; done`
Expected: clean build, all pass. GUI paths verified on hardware by the operator (see plan tail).

- [ ] **Step 5: Commit**

```bash
git add widgets/mainwindow.cpp README.md
git commit -m "feat: enable Native FLEX RF power slider end to end"
```

---

### Task 4: Persist the selected radio

**Files:**
- Modify: `Transceiver/NativeFlexRadioSelection.hpp` (add `restore`)
- Modify: `Transceiver/NativeFlexRadioSelection.cpp` (implement `restore`)
- Modify: `Configuration.cpp` (`read_settings` near line 1984, `write_settings` near line 2321, combo handler at line ~3354)
- Create: `tests/test_native_flex_selection.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `Radio` struct, `selected()`, `hasSelection()`.
- Produces, for Task 6: `void NativeFlexRadioSelection::restore (Radio const&)` — seeds the session selection without any UI.

- [ ] **Step 1: Write the failing test**

`tests/test_native_flex_selection.cpp`:

```cpp
#include <QObject>
#include <QTest>

#include "Transceiver/NativeFlexRadioSelection.hpp"

class TestNativeFlexSelection final
  : public QObject
{
  Q_OBJECT

private slots:
  void init ()
  {
    NativeFlexRadioSelection::clear ();
  }

  void restore_round_trips ()
  {
    NativeFlexRadioSelection::Radio radio;
    radio.model = "FLEX-8400M";
    radio.serial = "1234-5678-9012-3456";
    radio.address = "192.168.1.100";
    radio.port = 4992;

    QVERIFY (!NativeFlexRadioSelection::hasSelection ());
    NativeFlexRadioSelection::restore (radio);
    QVERIFY (NativeFlexRadioSelection::hasSelection ());

    auto const got = NativeFlexRadioSelection::selected ();
    QCOMPARE (got.model, radio.model);
    QCOMPARE (got.serial, radio.serial);
    QCOMPARE (got.address, radio.address);
    QCOMPARE (got.port, radio.port);
  }

  void invalid_radio_restores_but_reports_no_selection ()
  {
    NativeFlexRadioSelection::Radio radio;   // empty address
    NativeFlexRadioSelection::restore (radio);
    QVERIFY (!NativeFlexRadioSelection::hasSelection ());
  }

  void clear_removes_selection ()
  {
    NativeFlexRadioSelection::Radio radio;
    radio.address = "10.0.0.5";
    NativeFlexRadioSelection::restore (radio);
    QVERIFY (NativeFlexRadioSelection::hasSelection ());
    NativeFlexRadioSelection::clear ();
    QVERIFY (!NativeFlexRadioSelection::hasSelection ());
  }
};

QTEST_MAIN (TestNativeFlexSelection)
#include "test_native_flex_selection.moc"
```

`tests/CMakeLists.txt` append:

```cmake
add_executable (test_native_flex_selection test_native_flex_selection.cpp)
target_link_libraries (test_native_flex_selection wsjt_qt Qt5::Test)
add_test (test_native_flex_selection test_native_flex_selection)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build -j"$(nproc)" --target test_native_flex_selection`
Expected: compile FAILURE — `restore` is not a member.

- [ ] **Step 3: Implement `restore`**

`NativeFlexRadioSelection.hpp`, after `bool refresh(QWidget * parent);`:

```cpp
  // Seed the session selection without any UI (settings restore,
  // dialog acceptance).
  void restore(Radio const& radio);
```

`NativeFlexRadioSelection.cpp`, after `clear()`:

```cpp
  void restore(Radio const& radio)
  {
    QMutexLocker guard {&selectionMutex};
    sessionRadio = radio;
  }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build -j"$(nproc)" --target test_native_flex_selection && ./build/tests/test_native_flex_selection`
Expected: PASS.

- [ ] **Step 5: Wire Configuration persistence**

`Configuration.cpp` — add `#include "Transceiver/NativeFlexRadioSelection.hpp"` if not already present (it is — `refresh` is called at line 3358).

In `read_settings`, next to `w7pp_flex_native_rx_ = settings_->value ("W7PPFlexNativeRx", false).toBool ();` (line ~1984):

```cpp
  // W7PP : restore the persisted Native FLEX radio selection.
  {
    NativeFlexRadioSelection::Radio radio;
    radio.model = settings_->value ("FlexNativeRadioModel").toString ();
    radio.serial = settings_->value ("FlexNativeRadioSerial").toString ();
    radio.address = settings_->value ("FlexNativeRadioAddress").toString ();
    radio.port = static_cast<quint16> (
        settings_->value ("FlexNativeRadioPort", 4992).toUInt ());
    if (radio.valid ())
      {
        NativeFlexRadioSelection::restore (radio);
      }
  }
```

In `write_settings`, next to `settings_->setValue ("W7PPFlexNativeRx", w7pp_flex_native_rx_);` (line ~2321):

```cpp
  // W7PP : persist the Native FLEX radio selection.
  {
    auto const radio = NativeFlexRadioSelection::selected ();
    if (radio.valid ())
      {
        settings_->setValue ("FlexNativeRadioModel", radio.model);
        settings_->setValue ("FlexNativeRadioSerial", radio.serial);
        settings_->setValue ("FlexNativeRadioAddress", radio.address);
        settings_->setValue ("FlexNativeRadioPort", radio.port);
      }
  }
```

In `on_rig_combo_box_currentIndexChanged` (line ~3354), only prompt when nothing usable is selected:

```cpp
  if (ui_->rig_combo_box->currentText() == "Flex Native VITA-49")
    {
      // A persisted or session selection stands; the "Select FLEX
      // Radio..." button (Task 6) is the way to change it.
      if (!NativeFlexRadioSelection::hasSelection())
        {
          NativeFlexRadioSelection::refresh(this);
        }
    }
```

- [ ] **Step 6: Build, run all tests**

Run: `cmake --build build -j"$(nproc)" && for t in test_qt_helpers test_flex_socket_compat test_flex_vita_receiver test_native_flex_factory test_tx_rf_power_plumbing test_native_flex_selection; do ./build/tests/$t || break; done`
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add Transceiver/NativeFlexRadioSelection.hpp Transceiver/NativeFlexRadioSelection.cpp \
  Configuration.cpp tests/test_native_flex_selection.cpp tests/CMakeLists.txt
git commit -m "feat: persist Native FLEX radio selection across restarts"
```

---

### Task 5: Async discovery engine

**Files:**
- Create: `Transceiver/NativeFlexDiscovery.hpp`
- Create: `Transceiver/NativeFlexDiscovery.cpp`
- Modify: `CMakeLists.txt:218` (add to `wsjt_qt` sources beside `NativeFlexRadioSelection.cpp`)
- Modify: `tests/test_native_flex_selection.cpp` (add discovery tests)

**Interfaces:**
- Consumes: `NativeFlexRadioSelection::Radio`.
- Produces, for Task 6: `NativeFlexDiscovery` QObject — `explicit NativeFlexDiscovery (quint16 port = 4992, QObject * parent = nullptr)`, `bool start ()`, `void stop ()`, `quint16 bound_port () const`, `QList<NativeFlexRadioSelection::Radio> radios () const`, signal `void radios_changed ()`, and `static NativeFlexRadioSelection::Radio parse (QByteArray const& datagram, QString const& sender_address)`.

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_native_flex_selection.cpp` (new include at top: `#include "Transceiver/NativeFlexDiscovery.hpp"`, plus `#include <QSignalSpy>` and `#include <QUdpSocket>`):

```cpp
  void parse_extracts_radio_fields ()
  {
    QByteArray const datagram =
        QByteArray ("\x38\x00\x00\x00", 4)
        + "discovery_protocol_version=3.0.0.2 model=FLEX-8400M "
          "serial=1234-5678-9012-3456 ip=192.168.1.100 port=4992 "
          "nickname=Shack";

    auto const radio =
        NativeFlexDiscovery::parse (datagram, "192.168.1.50");

    QVERIFY (radio.valid ());
    QCOMPARE (radio.model, QString {"FLEX-8400M"});
    QCOMPARE (radio.serial, QString {"1234-5678-9012-3456"});
    QCOMPARE (radio.address, QString {"192.168.1.100"});
    QCOMPARE (radio.port, quint16 {4992});
  }

  void parse_rejects_non_flex_models ()
  {
    QByteArray const datagram =
        "discovery_protocol_version=3.0.0.2 model=IC-7300 "
        "serial=X ip=192.168.1.7";
    QVERIFY (!NativeFlexDiscovery::parse (datagram, "192.168.1.7").valid ());
  }

  void parse_falls_back_to_sender_address ()
  {
    QByteArray const datagram =
        "discovery_protocol_version=3.0.0.2 model=FLEX-6600 serial=S1";
    auto const radio =
        NativeFlexDiscovery::parse (datagram, "192.168.1.60");
    QVERIFY (radio.valid ());
    QCOMPARE (radio.address, QString {"192.168.1.60"});
  }

  void engine_collects_and_deduplicates ()
  {
    NativeFlexDiscovery discovery {0};   // ephemeral test port
    QVERIFY (discovery.start ());

    QSignalSpy spy {&discovery, &NativeFlexDiscovery::radios_changed};

    QUdpSocket sender;
    QByteArray const announce =
        "discovery_protocol_version=3.0.0.2 model=FLEX-8400M "
        "serial=AAAA ip=192.168.1.100 port=4992";

    sender.writeDatagram (
        announce, QHostAddress::LocalHost, discovery.bound_port ());
    QTRY_COMPARE (discovery.radios ().size (), 1);
    QVERIFY (spy.count () >= 1);

    // Same radio again: no new entry.
    sender.writeDatagram (
        announce, QHostAddress::LocalHost, discovery.bound_port ());
    QTest::qWait (100);
    QCOMPARE (discovery.radios ().size (), 1);

    // Same serial, new address (DHCP move): entry updated in place.
    QByteArray const moved =
        "discovery_protocol_version=3.0.0.2 model=FLEX-8400M "
        "serial=AAAA ip=192.168.1.222 port=4992";
    sender.writeDatagram (
        moved, QHostAddress::LocalHost, discovery.bound_port ());
    QTRY_COMPARE (discovery.radios ().first ().address,
                  QString {"192.168.1.222"});
    QCOMPARE (discovery.radios ().size (), 1);

    // Second radio: second entry.
    QByteArray const other =
        "discovery_protocol_version=3.0.0.2 model=FLEX-6600 "
        "serial=BBBB ip=192.168.1.101 port=4992";
    sender.writeDatagram (
        other, QHostAddress::LocalHost, discovery.bound_port ());
    QTRY_COMPARE (discovery.radios ().size (), 2);

    discovery.stop ();
  }
```

Also add `Qt5::Network` to `test_native_flex_selection` link libraries in `tests/CMakeLists.txt`.

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j"$(nproc)" --target test_native_flex_selection`
Expected: compile FAILURE — no `NativeFlexDiscovery.hpp`.

- [ ] **Step 3: Implement the engine**

`Transceiver/NativeFlexDiscovery.hpp`:

```cpp
#ifndef NATIVE_FLEX_DISCOVERY_HPP_
#define NATIVE_FLEX_DISCOVERY_HPP_

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QUdpSocket>

#include "NativeFlexRadioSelection.hpp"

// W7PP : signal-driven FLEX discovery listener.
//
// Radios announce themselves roughly once per second on UDP 4992.
// This engine binds, parses each announcement as it arrives, and
// keeps a deduplicated list (keyed by serial when present, else by
// address:port) that updates in place when a known radio moves to a
// new address. No blocking waits anywhere.
class NativeFlexDiscovery final
  : public QObject
{
  Q_OBJECT

public:
  explicit NativeFlexDiscovery (quint16 port = 4992,
                                QObject * parent = nullptr);

  bool start ();
  void stop ();
  quint16 bound_port () const;

  QList<NativeFlexRadioSelection::Radio> radios () const;

  static NativeFlexRadioSelection::Radio parse (
      QByteArray const& datagram,
      QString const& sender_address);

  Q_SIGNAL void radios_changed ();

private:
  void read_pending ();

  quint16 port_;
  QUdpSocket socket_;
  QList<NativeFlexRadioSelection::Radio> radios_;
};

#endif
```

`Transceiver/NativeFlexDiscovery.cpp` — move `fieldValue()` here from `NativeFlexRadioSelection.cpp` (anonymous namespace, verbatim), then:

```cpp
#include "NativeFlexDiscovery.hpp"

#include <QHostAddress>

namespace
{
  // fieldValue() moved verbatim from NativeFlexRadioSelection.cpp
}

NativeFlexDiscovery::NativeFlexDiscovery (quint16 port, QObject * parent)
  : QObject {parent}
  , port_ {port}
{
  connect (&socket_, &QUdpSocket::readyRead,
           this, &NativeFlexDiscovery::read_pending);
}

bool NativeFlexDiscovery::start ()
{
  radios_.clear ();
  return socket_.bind (
      QHostAddress::AnyIPv4,
      port_,
      QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
}

void NativeFlexDiscovery::stop ()
{
  socket_.close ();
}

quint16 NativeFlexDiscovery::bound_port () const
{
  return socket_.localPort ();
}

QList<NativeFlexRadioSelection::Radio> NativeFlexDiscovery::radios () const
{
  return radios_;
}

NativeFlexRadioSelection::Radio NativeFlexDiscovery::parse (
    QByteArray const& datagram,
    QString const& sender_address)
{
  NativeFlexRadioSelection::Radio radio;

  int const textStart =
      datagram.indexOf ("discovery_protocol_version=");
  if (textStart < 0)
    {
      return radio;
    }

  QByteArray const text = datagram.mid (textStart);
  QByteArray const modelBytes = fieldValue (text, "model");

  // W7PP : FLEX-6000/8000 families and Aurora.
  if (!modelBytes.startsWith ("FLEX-")
      && !modelBytes.startsWith ("AU-"))
    {
      return radio;
    }

  radio.model = QString::fromLatin1 (modelBytes).trimmed ();
  radio.serial =
      QString::fromLatin1 (fieldValue (text, "serial")).trimmed ();
  radio.address =
      QString::fromLatin1 (fieldValue (text, "ip")).trimmed ();

  if (radio.address.isEmpty ())
    {
      radio.address = sender_address;
    }

  bool portOk = false;
  int const reportedPort =
      QString::fromLatin1 (fieldValue (text, "port")).toInt (&portOk);
  radio.port = static_cast<quint16> (
      portOk && reportedPort > 0 && reportedPort <= 65535
        ? reportedPort
        : 4992);

  return radio;
}

void NativeFlexDiscovery::read_pending ()
{
  bool changed = false;

  while (socket_.hasPendingDatagrams ())
    {
      QByteArray packet;
      packet.resize (
          static_cast<int> (socket_.pendingDatagramSize ()));

      QHostAddress senderAddress;
      quint16 senderPort = 0;

      qint64 const received = socket_.readDatagram (
          packet.data (), packet.size (), &senderAddress, &senderPort);
      if (received <= 0)
        {
          continue;
        }

      auto const radio = parse (packet, senderAddress.toString ());
      if (!radio.valid ())
        {
          continue;
        }

      int existing_index = -1;
      for (int i = 0; i < radios_.size (); ++i)
        {
          auto const& existing = radios_.at (i);
          bool const same =
              !radio.serial.isEmpty ()
                ? existing.serial == radio.serial
                : existing.address == radio.address
                    && existing.port == radio.port;
          if (same)
            {
              existing_index = i;
              break;
            }
        }

      if (existing_index < 0)
        {
          radios_.append (radio);
          changed = true;
        }
      else if (radios_.at (existing_index).address != radio.address
               || radios_.at (existing_index).port != radio.port
               || radios_.at (existing_index).model != radio.model)
        {
          radios_.replace (existing_index, radio);
          changed = true;
        }
    }

  if (changed)
    {
      Q_EMIT radios_changed ();
    }
}
```

`CMakeLists.txt` — beside `Transceiver/NativeFlexRadioSelection.cpp` (line 218) add `Transceiver/NativeFlexDiscovery.cpp`.

- [ ] **Step 4: Run tests to verify pass**

Run: `cmake --build build -j"$(nproc)" --target test_native_flex_selection && ./build/tests/test_native_flex_selection`
Expected: all PASS (including the Task 4 ones).

- [ ] **Step 5: Commit**

```bash
git add Transceiver/NativeFlexDiscovery.hpp Transceiver/NativeFlexDiscovery.cpp \
  CMakeLists.txt tests/test_native_flex_selection.cpp tests/CMakeLists.txt
git commit -m "feat: add signal-driven Native FLEX discovery engine"
```

---

### Task 6: Selection dialog with live list and connect-by-IP

**Files:**
- Create: `Transceiver/NativeFlexRadioDialog.hpp`
- Create: `Transceiver/NativeFlexRadioDialog.cpp`
- Modify: `Transceiver/NativeFlexRadioSelection.cpp` (rebuild `refresh()` on the dialog; delete the blocking scan and `QInputDialog` picker; `displayText` moves to the dialog)
- Modify: `CMakeLists.txt` (add dialog beside `NativeFlexDiscovery.cpp`)
- Modify: `Configuration.ui` (add "Select FLEX Radio…" button, grid row 5 under the DAX channel row at rows 4)
- Modify: `Configuration.cpp` (auto-connect slot + enable/disable beside the DAX widgets at lines ~2285)
- Modify: `tests/test_native_flex_selection.cpp` (manual-entry parsing tests)

**Interfaces:**
- Consumes: `NativeFlexDiscovery` (Task 5), `NativeFlexRadioSelection::restore` (Task 4).
- Produces: `NativeFlexRadioDialog` — `NativeFlexRadioDialog (NativeFlexRadioSelection::Radio const& current, QWidget * parent)`, `NativeFlexRadioSelection::Radio chosen_radio () const`, `static NativeFlexRadioSelection::Radio parse_manual_entry (QString const& text)`. `refresh()` keeps its exact signature `bool refresh(QWidget * parent)` for its caller (`Configuration.cpp:3358` plus the button handler added in this task).

- [ ] **Step 1: Write the failing manual-entry tests**

Append to `tests/test_native_flex_selection.cpp` (include `Transceiver/NativeFlexRadioDialog.hpp`; the dialog is constructed only in GUI paths, but `parse_manual_entry` is static and GUI-free):

```cpp
  void manual_entry_bare_host ()
  {
    auto const radio =
        NativeFlexRadioDialog::parse_manual_entry ("192.168.1.100");
    QVERIFY (radio.valid ());
    QCOMPARE (radio.address, QString {"192.168.1.100"});
    QCOMPARE (radio.port, quint16 {4992});
    QCOMPARE (radio.model, QString {"Manual"});
  }

  void manual_entry_host_and_port ()
  {
    auto const radio =
        NativeFlexRadioDialog::parse_manual_entry ("flex.example.net:4993");
    QVERIFY (radio.valid ());
    QCOMPARE (radio.address, QString {"flex.example.net"});
    QCOMPARE (radio.port, quint16 {4993});
  }

  void manual_entry_rejects_bad_input ()
  {
    QVERIFY (!NativeFlexRadioDialog::parse_manual_entry ("").valid ());
    QVERIFY (!NativeFlexRadioDialog::parse_manual_entry ("   ").valid ());
    QVERIFY (!NativeFlexRadioDialog::parse_manual_entry ("host:0").valid ());
    QVERIFY (!NativeFlexRadioDialog::parse_manual_entry ("host:99999").valid ());
    QVERIFY (!NativeFlexRadioDialog::parse_manual_entry (":4992").valid ());
  }
```

Link `test_native_flex_selection` against `Qt5::Widgets` as well in `tests/CMakeLists.txt` (the dialog is a QDialog).

- [ ] **Step 2: Run to verify failure**

Run: `cmake --build build -j"$(nproc)" --target test_native_flex_selection`
Expected: compile FAILURE — no `NativeFlexRadioDialog.hpp`.

- [ ] **Step 3: Implement the dialog**

`Transceiver/NativeFlexRadioDialog.hpp`:

```cpp
#ifndef NATIVE_FLEX_RADIO_DIALOG_HPP_
#define NATIVE_FLEX_RADIO_DIALOG_HPP_

#include <QDialog>
#include <QString>

#include "NativeFlexDiscovery.hpp"
#include "NativeFlexRadioSelection.hpp"

class QLineEdit;
class QListWidget;
class QPushButton;

// W7PP : Native FLEX radio picker.
//
// Discovered radios appear in the list as their announcements
// arrive (no blocking scan). A radio outside the broadcast domain
// can be entered directly as host or host:port.
class NativeFlexRadioDialog final
  : public QDialog
{
  Q_OBJECT

public:
  explicit NativeFlexRadioDialog (
      NativeFlexRadioSelection::Radio const& current,
      QWidget * parent = nullptr);

  NativeFlexRadioSelection::Radio chosen_radio () const;

  static NativeFlexRadioSelection::Radio parse_manual_entry (
      QString const& text);

private:
  void repopulate ();
  void update_ok_enabled ();

  NativeFlexRadioSelection::Radio current_;
  NativeFlexDiscovery discovery_;
  QListWidget * list_;
  QLineEdit * manual_;
  QPushButton * ok_button_;
};

#endif
```

`Transceiver/NativeFlexRadioDialog.cpp`:

```cpp
#include "NativeFlexRadioDialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace
{
  QString displayText (NativeFlexRadioSelection::Radio const& radio)
  {
    QString text = radio.model;
    if (!radio.serial.isEmpty ())
      {
        text += QString {"  S/N %1"}.arg (radio.serial);
      }
    text += QString {"  %1"}.arg (radio.address);
    if (4992 != radio.port)
      {
        text += QString {":%1"}.arg (radio.port);
      }
    return text;
  }
}

NativeFlexRadioDialog::NativeFlexRadioDialog (
    NativeFlexRadioSelection::Radio const& current,
    QWidget * parent)
  : QDialog {parent}
  , current_ {current}
  , list_ {new QListWidget {this}}
  , manual_ {new QLineEdit {this}}
{
  setWindowTitle (tr ("Flex Native VITA-49"));

  auto * layout = new QVBoxLayout {this};
  layout->addWidget (
      new QLabel {tr ("Radios discovered on the network:"), this});
  layout->addWidget (list_);
  layout->addWidget (
      new QLabel {tr ("Or connect by address (host or host:port):"), this});
  manual_->setPlaceholderText (QStringLiteral ("192.168.1.100:4992"));
  layout->addWidget (manual_);

  auto * buttons = new QDialogButtonBox {
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this};
  ok_button_ = buttons->button (QDialogButtonBox::Ok);
  layout->addWidget (buttons);

  connect (buttons, &QDialogButtonBox::accepted,
           this, &QDialog::accept);
  connect (buttons, &QDialogButtonBox::rejected,
           this, &QDialog::reject);
  connect (list_, &QListWidget::itemSelectionChanged,
           this, &NativeFlexRadioDialog::update_ok_enabled);
  connect (list_, &QListWidget::itemDoubleClicked,
           this, &QDialog::accept);
  connect (manual_, &QLineEdit::textChanged,
           this, &NativeFlexRadioDialog::update_ok_enabled);
  connect (&discovery_, &NativeFlexDiscovery::radios_changed,
           this, &NativeFlexRadioDialog::repopulate);

  discovery_.start ();
  repopulate ();
  update_ok_enabled ();
}

NativeFlexRadioSelection::Radio
NativeFlexRadioDialog::chosen_radio () const
{
  auto const manual_text = manual_->text ().trimmed ();
  if (!manual_text.isEmpty ())
    {
      return parse_manual_entry (manual_text);
    }

  auto const radios = discovery_.radios ();
  int const row = list_->currentRow ();
  if (row >= 0 && row < radios.size ())
    {
      return radios.at (row);
    }
  return NativeFlexRadioSelection::Radio {};
}

NativeFlexRadioSelection::Radio
NativeFlexRadioDialog::parse_manual_entry (QString const& text)
{
  NativeFlexRadioSelection::Radio radio;

  QString const trimmed = text.trimmed ();
  if (trimmed.isEmpty ())
    {
      return radio;
    }

  QString host = trimmed;
  quint16 port = 4992;

  int const colon = trimmed.lastIndexOf (':');
  if (colon >= 0)
    {
      host = trimmed.left (colon).trimmed ();
      bool ok = false;
      int const parsed = trimmed.mid (colon + 1).toInt (&ok);
      if (!ok || parsed <= 0 || parsed > 65535)
        {
          return radio;
        }
      port = static_cast<quint16> (parsed);
    }

  if (host.isEmpty ())
    {
      return radio;
    }

  radio.model = QStringLiteral ("Manual");
  radio.address = host;
  radio.port = port;
  return radio;
}

void NativeFlexRadioDialog::repopulate ()
{
  auto const radios = discovery_.radios ();

  // Preserve the highlighted radio across refreshes.
  QString selected_key;
  int const row = list_->currentRow ();
  if (row >= 0 && row < radios.size ())
    {
      selected_key = radios.at (row).serial + radios.at (row).address;
    }

  list_->clear ();
  int select_row = -1;

  for (int i = 0; i < radios.size (); ++i)
    {
      auto const& radio = radios.at (i);
      list_->addItem (displayText (radio));

      QString const key = radio.serial + radio.address;
      if (!selected_key.isEmpty () && key == selected_key)
        {
          select_row = i;
        }
      else if (selected_key.isEmpty ()
               && current_.valid ()
               && (!current_.serial.isEmpty ()
                     ? radio.serial == current_.serial
                     : radio.address == current_.address))
        {
          select_row = i;
        }
    }

  if (select_row < 0 && 1 == radios.size ())
    {
      select_row = 0;
    }
  if (select_row >= 0)
    {
      list_->setCurrentRow (select_row);
    }
  update_ok_enabled ();
}

void NativeFlexRadioDialog::update_ok_enabled ()
{
  bool ok = false;
  auto const manual_text = manual_->text ().trimmed ();
  if (!manual_text.isEmpty ())
    {
      ok = parse_manual_entry (manual_text).valid ();
    }
  else
    {
      ok = list_->currentRow () >= 0;
    }
  ok_button_->setEnabled (ok);
}
```

- [ ] **Step 4: Rebuild `refresh()` on the dialog**

`Transceiver/NativeFlexRadioSelection.cpp`: delete the blocking scan body of `refresh()`, the `QInputDialog` picker, `fieldValue()` (now lives in `NativeFlexDiscovery.cpp`) and `displayText()` (now in the dialog); drop the now-unused includes (`QElapsedTimer`, `QInputDialog`, `QUdpSocket`, `QStringList`, `QHostAddress`, `QByteArray`); add `#include "NativeFlexRadioDialog.hpp"`. New body:

```cpp
  bool refresh(QWidget * parent)
  {
    NativeFlexRadioDialog dialog {selected(), parent};

    if (QDialog::Accepted != dialog.exec())
      {
        // Cancel keeps whatever selection stood before.
        return hasSelection();
      }

    Radio const chosen = dialog.chosen_radio();
    if (!chosen.valid())
      {
        return hasSelection();
      }

    restore(chosen);
    return true;
  }
```

`CMakeLists.txt`: add `Transceiver/NativeFlexRadioDialog.cpp` beside `Transceiver/NativeFlexDiscovery.cpp`.

- [ ] **Step 5: Add the Settings button**

`Configuration.ui` — in the grid holding `w7pp_flex_dax_channel_label` (row 4), add at the next free row (verify row 5 is unused in that grid; if occupied, use the next free index):

```xml
          <item row="5" column="0" colspan="2">
           <widget class="QPushButton" name="flex_radio_select_push_button">
            <property name="toolTip">
             <string>Discover FLEX radios on the network or connect to one by IP address.</string>
            </property>
            <property name="text">
             <string>Select FLEX Radio...</string>
            </property>
           </widget>
          </item>
```

`Configuration.cpp`:
- In `Configuration::impl`, beside the `on_rig_combo_box_currentIndexChanged` slot declaration: `Q_SLOT void on_flex_radio_select_push_button_clicked ();`
- Definition near `on_rig_combo_box_currentIndexChanged`:

```cpp
void Configuration::impl::on_flex_radio_select_push_button_clicked ()
{
  NativeFlexRadioSelection::refresh (this);
}
```

- Beside `ui_->w7pp_flex_dax_channel_combo_box->setEnabled (native);` (line ~2286): `ui_->flex_radio_select_push_button->setEnabled (native);`

- [ ] **Step 6: Run all tests and full build**

Run: `cmake --build build -j"$(nproc)" && for t in test_qt_helpers test_flex_socket_compat test_flex_vita_receiver test_native_flex_factory test_tx_rf_power_plumbing test_native_flex_selection; do ./build/tests/$t || break; done`
Expected: full build, all suites pass.

- [ ] **Step 7: Update README known limitations**

Remove the "radio selection is not persisted" and "discovery blocks the UI" bullets; note multi-radio + connect-by-IP support in the Status section.

- [ ] **Step 8: Commit**

```bash
git add Transceiver/NativeFlexRadioDialog.hpp Transceiver/NativeFlexRadioDialog.cpp \
  Transceiver/NativeFlexRadioSelection.cpp CMakeLists.txt Configuration.ui Configuration.cpp \
  tests/test_native_flex_selection.cpp tests/CMakeLists.txt README.md
git commit -m "feat: async Native FLEX radio dialog with multi-radio list and connect-by-IP"
```

---

## Completion criteria

1. Full build passes with no new warnings in touched files.
2. All six test binaries pass (`test_qt_helpers`, `test_flex_socket_compat`, `test_flex_vita_receiver`, `test_native_flex_factory`, `test_tx_rf_power_plumbing`, `test_native_flex_selection`).
3. Inertness: with any rig other than `"Flex Native VITA-49"` no new code path is reachable (the state member defaults to −1, the dispatch requires ≥ 0, the dialog opens only from Flex-only UI, restore only seeds statics).
4. **Not** included in "done": on-air behaviour. The RF power command against real hardware, discovery on a multi-radio LAN, and connect-by-IP across subnets can only be confirmed by the operator with a FlexRadio. Say so when reporting completion.
