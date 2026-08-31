#include "NativeFlexDiscovery.hpp"

#include <QHostAddress>

namespace
{
  // Moved verbatim from NativeFlexRadioSelection.cpp: extract the
  // value of a space-delimited key=value field.
  QByteArray fieldValue(
      QByteArray const& text,
      char const * key)
  {
    QByteArray const needle =
        QByteArray(key) + "=";

    int searchFrom = 0;

    while (true)
      {
        int const start =
            text.indexOf(
                needle,
                searchFrom);

        if (start < 0)
          return QByteArray {};

        bool const fieldBoundary =
            0 == start
            || ' ' == text.at(start - 1);

        if (fieldBoundary)
          {
            int const valueStart =
                start + needle.size();

            int end =
                text.indexOf(
                    ' ',
                    valueStart);

            if (end < 0)
              end = text.size();

            return text.mid(
                valueStart,
                end - valueStart);
          }

        searchFrom =
            start + needle.size();
      }
  }
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

  /*
   * W7PP
   *
   * Native FLEX discovery supports both:
   *
   *   FLEX-*   FLEX-6000 / FLEX-8000 families
   *   AU-*     Aurora family
   */
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
  if (radio.address.isEmpty ())
    {
      return NativeFlexRadioSelection::Radio {};
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
