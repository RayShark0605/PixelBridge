#include "qt_raster_test_support.h"

#include <QEvent>
#include <QEventLoop>

namespace pbencoder::test
{

QApplication& EnsureApplication()
{
    static int argumentCount = 1;
    static char executable[] = "PBQtRasterPresentationTests";
    static char* arguments[]{executable, nullptr};
    static QApplication application(argumentCount, arguments);
    application.setQuitOnLastWindowClosed(false);
    return application;
}

void DrainEvents()
{
    EnsureApplication().processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

}
