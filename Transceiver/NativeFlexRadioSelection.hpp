#ifndef NATIVE_FLEX_RADIO_SELECTION_HPP_
#define NATIVE_FLEX_RADIO_SELECTION_HPP_

#include <QString>
#include <QtGlobal>

class QWidget;

namespace NativeFlexRadioSelection
{
  struct Radio
  {
    QString model {};
    QString serial {};
    QString address {};
    quint16 port {4992};

    bool valid() const noexcept
    {
      return !address.isEmpty() && 0 != port;
    }
  };

  // One short discovery scan only.
  //
  // One FLEX:
  //   automatic selection.
  //
  // Multiple FLEX radios:
  //   display model / serial / IP picker.
  //
  // No SmartSDR client dependency.
  // No persistent discovery listener.
  bool refresh(QWidget * parent);

  Radio selected();
  bool hasSelection();
  void clear();
}

#endif