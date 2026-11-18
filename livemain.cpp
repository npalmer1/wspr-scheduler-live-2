// WSPR Scheduler Live (Qt 6): reports of this station's WSPR transmissions from wspr.live, saved in
// the WSPR Scheduler database (wspr_rx.reported) for WSPR Scheduler Map. C#: WSPR_Live.
#include "liveform.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("G4GCI");
    QCoreApplication::setApplicationName("WSPR Scheduler Live");
    QApplication::setWindowIcon(QIcon(":/wsprlive.ico"));   // (Windows: also in the .exe; Linux: only here)
    QGuiApplication::setDesktopFileName("wspr-scheduler-live-2");   // Linux: the menu entry's icon in the taskbar
    LiveForm w;
    w.show();
    return app.exec();
}
