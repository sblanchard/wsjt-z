#include "NativeFlexRadioSelection.hpp"

#include <QMutex>
#include <QMutexLocker>
#include <QWidget>

#include "NativeFlexRadioDialog.hpp"

namespace
{
  QMutex selectionMutex;
  NativeFlexRadioSelection::Radio sessionRadio;
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
    // W7PP : discovery runs inside the dialog's event loop, so the
    // list fills as radios announce themselves -- no blocking scan.
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
}
