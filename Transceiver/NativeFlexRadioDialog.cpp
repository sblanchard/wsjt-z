#include "NativeFlexRadioDialog.hpp"

#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace
{
  // Moved from NativeFlexRadioSelection.cpp: one list row per radio.
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
