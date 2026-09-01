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
