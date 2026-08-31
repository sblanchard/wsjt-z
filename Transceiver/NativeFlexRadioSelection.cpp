#include "NativeFlexRadioSelection.hpp"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QInputDialog>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QStringList>
#include <QUdpSocket>
#include <QWidget>

namespace
{
  QMutex selectionMutex;
  NativeFlexRadioSelection::Radio sessionRadio;

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

  QString displayText(
      NativeFlexRadioSelection::Radio const& radio)
  {
    QString text = radio.model;

    if (!radio.serial.isEmpty())
      {
        text +=
            QString {"  S/N %1"}
                .arg(radio.serial);
      }

    text +=
        QString {"  %1"}
            .arg(radio.address);

    if (4992 != radio.port)
      {
        text +=
            QString {":%1"}
                .arg(radio.port);
      }

    return text;
  }
}

namespace NativeFlexRadioSelection
{
  void clear()
  {
    QMutexLocker guard {&selectionMutex};
    sessionRadio = Radio {};
  }

  void restore(Radio const& radio)
  {
    QMutexLocker guard {&selectionMutex};
    sessionRadio = radio;
  }

  Radio selected()
  {
    QMutexLocker guard {&selectionMutex};
    return sessionRadio;
  }

  bool hasSelection()
  {
    return selected().valid();
  }

  bool refresh(QWidget * parent)
  {
    QList<Radio> radios;

    QUdpSocket discovery;

    if (!discovery.bind(
            QHostAddress::AnyIPv4,
            4992,
            QUdpSocket::ShareAddress
              | QUdpSocket::ReuseAddressHint))
      {
        clear();
        return false;
      }

    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < 3500)
      {
        if (!discovery.waitForReadyRead(400))
          continue;

        while (discovery.hasPendingDatagrams())
          {
            QByteArray packet;

            packet.resize(
                static_cast<int>(
                    discovery.pendingDatagramSize()));

            QHostAddress senderAddress;
            quint16 senderPort = 0;

            qint64 const received =
                discovery.readDatagram(
                    packet.data(),
                    packet.size(),
                    &senderAddress,
                    &senderPort);

            if (received <= 0)
              continue;

            int const textStart =
                packet.indexOf(
                    "discovery_protocol_version=");

            if (textStart < 0)
              continue;

            QByteArray const text =
                packet.mid(textStart);

            QByteArray const modelBytes =
                fieldValue(
                    text,
                    "model");

            /*
             * W7PP 
             *
             * Native FLEX discovery supports both:
             *
             *   FLEX-*   FLEX-6000 / FLEX-8000 families
             *   AU-*     Aurora family
             *
             * Keep the exact model string reported by the radio.
             * Later Native FLEX power control will use the
             * selected radio plus transmit-status capability data
             * rather than assuming every radio is 100 watts.
             */
            if (
                !modelBytes.startsWith("FLEX-")
                && !modelBytes.startsWith("AU-"))
              continue;

            Radio radio;

            radio.model =
                QString::fromLatin1(
                    modelBytes)
                    .trimmed();

            radio.serial =
                QString::fromLatin1(
                    fieldValue(
                        text,
                        "serial"))
                    .trimmed();

            radio.address =
                QString::fromLatin1(
                    fieldValue(
                        text,
                        "ip"))
                    .trimmed();

            if (radio.address.isEmpty())
              {
                radio.address =
                    senderAddress.toString();
              }

            if (radio.address.isEmpty())
              continue;

            bool portOk = false;

            int const reportedPort =
                QString::fromLatin1(
                    fieldValue(
                        text,
                        "port"))
                    .toInt(
                        &portOk);

            radio.port =
                static_cast<quint16>(
                    portOk
                      && reportedPort > 0
                      && reportedPort <= 65535
                        ? reportedPort
                        : 4992);

            bool duplicate = false;

            for (Radio const& existing : radios)
              {
                if (existing.address == radio.address
                    && existing.port == radio.port)
                  {
                    duplicate = true;
                    break;
                  }
              }

            if (!duplicate)
              radios.append(radio);
          }
      }

    if (radios.isEmpty())
      {
        clear();
        return false;
      }

    Radio chosen;

    if (1 == radios.size())
      {
        chosen = radios.first();
      }

    if (radios.size() > 1)
      {
        QStringList choices;

        for (Radio const& radio : radios)
          {
            choices.append(
                displayText(radio));
          }

        bool accepted = false;

        QString const choice =
            QInputDialog::getItem(
                parent,
                QObject::tr(
                    "Flex Native VITA-49"),
                QObject::tr(
                    "Select FLEX radio:"),
                choices,
                0,
                false,
                &accepted);

        if (!accepted)
          {
            clear();
            return false;
          }

        int const index =
            choices.indexOf(choice);

        if (index < 0
            || index >= radios.size())
          {
            clear();
            return false;
          }

        chosen = radios.at(index);
      }

    {
      QMutexLocker guard {&selectionMutex};
      sessionRadio = chosen;
    }

    return true;
  }
}
