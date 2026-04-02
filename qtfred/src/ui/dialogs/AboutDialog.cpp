//
//

#include "AboutDialog.h"

#include "ui_AboutDialog.h"

#include <QtGui/QDesktopServices>
#include <QtCore/QUrl>
#include <QtWidgets/QApplication>

namespace fso::fred::dialogs {

AboutDialog::AboutDialog(QWidget* parent, EditorViewport* viewport)
	: QDialog(parent),
	  ui(new Ui::AboutDialog()),
	  _model(new AboutDialogModel(this, viewport))
{
	ui->setupUi(this);
	ui->labelVersion->setText(QString::fromStdString(_model->getVersionString()));
}

void AboutDialog::on_reportBugButton_clicked()
{
	QDesktopServices::openUrl(QUrl("https://github.com/scp-fs2open/fs2open.github.com/issues", QUrl::TolerantMode));
}

void AboutDialog::on_visitForumsButton_clicked()
{
	QDesktopServices::openUrl(QUrl("https://www.hard-light.net/forums/", QUrl::TolerantMode));
}

void AboutDialog::on_aboutQtButton_clicked()
{
	QApplication::aboutQt();
}

} // namespace fso::fred::dialogs
