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
