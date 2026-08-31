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
