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
