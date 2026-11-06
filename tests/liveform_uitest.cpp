// Drives the WSPR Scheduler Live window offscreen against a fake wspr.live (own thread) and the scratch
// MariaDB: start-up, saving without duplicates, automatic updates, filters, version search, another call,
// Config, and the power-cut case (database and internet missing at start, then back). Saves live_form.png.
#include "credentials.h"
#include "liveform.h"
#include "livetablemodel.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMutex>
#include <QPushButton>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTableView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <cstdio>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; std::printf("FAIL line %d: %s\n", __LINE__, #c); } else std::printf("ok   %s\n", #c); } while (0)

static QString row(qint64 id, const QString &time, int band, const QString &rx, const QString &rxLoc, int km, int freq,
                   int snr, const QString &ver, const QString &tx = "G4GCI")
{
    return QString("%1\t%2\t%3\t%4\t51.5\t-0.1\t%5\t%6\t50.6\t-1.6\tIO90gu\t%7\t75\t255\t%8\t37\t%9\t0\t%10\t1")
        .arg(id).arg(time).arg(band).arg(rx, rxLoc, tx).arg(km).arg(freq).arg(snr).arg(ver);
}

class FakeLive : public QObject
{
public:
    QTcpServer *server = nullptr;
    QMutex lock;
    QStringList queries;
    QByteArray body;
    void start()
    {
        server = new QTcpServer(this);
        server->listen(QHostAddress::LocalHost);
        connect(server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *c = server->nextPendingConnection();
            connect(c, &QTcpSocket::readyRead, c, [this, c] {
                const QByteArray path = c->readAll().split(' ').value(1);
                const QString q = QUrl::fromPercentEncoding(QUrl(QString::fromLatin1(path)).query(QUrl::FullyEncoded).mid(6).toLatin1());
                QByteArray out;
                {
                    QMutexLocker l(&lock);
                    queries << q;
                    out = q.startsWith("SELECT version()") ? QByteArray("24.8.4.13\n") : body;
                }
                c->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(out.size()) + "\r\nConnection: close\r\n\r\n" + out);
                c->disconnectFromHost();
            });
        });
    }
    void setBody(const QStringList &rows) { QMutexLocker l(&lock); body = (rows.join('\n') + '\n').toUtf8(); }
    QString lastQuery() { QMutexLocker l(&lock); return queries.value(queries.size() - 1); }
    int count() { QMutexLocker l(&lock); return int(queries.size()); }
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    {   // scratch database: empty reported table
        QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", "probe");
        db.setHostName("127.0.0.1");
        db.setUserName("admin");
        db.setPassword("wspr");
        if (!db.open()) {
            std::printf("liveform_uitest: no scratch database; skipped\n");
            return 77;
        }
        QSqlQuery(db).exec("CREATE DATABASE IF NOT EXISTS wspr_rx");
        QSqlQuery(db).exec("DROP TABLE IF EXISTS wspr_rx.reported");
        db.close();
    }
    QSqlDatabase::removeDatabase("probe");

    QStringList answered;
    QTimer auto_;
    QObject::connect(&auto_, &QTimer::timeout, [&] {
        for (QWidget *w : QApplication::topLevelWidgets()) {
            auto *d = qobject_cast<QDialog *>(w);
            if (!d || !d->isVisible() || d->objectName() != "MessageDialog") continue;
            if (auto *l = d->findChild<QLabel *>()) answered << l->text().split('\n').first();
            for (QPushButton *b : d->findChildren<QPushButton *>())
                if (b->text() == "Yes" || b->text() == "OK") { b->click(); break; }
        }
    });
    auto_.start(30);
    auto pump = [&](int ms = 100) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); };
    auto waitFor = [&](const std::function<bool()> &f, int ms = 4000) {
        for (int i = 0; i < ms / 20 && !f(); ++i) pump(20);
        return f();
    };

    QThread th;
    FakeLive fake;
    fake.moveToThread(&th);
    th.start();
    QMetaObject::invokeMethod(&fake, [&] { fake.start(); }, Qt::BlockingQueuedConnection);
    const QString liveUrl = QString("http://127.0.0.1:%1/").arg(fake.server->serverPort());
    fake.setBody({row(9001, "2026-11-06 12:28:00", 14, "DL1ECN-8", "JO32uo", 660, 14097023, -12, "web-888"),
                  row(9002, "2026-11-06 12:30:00", 14, "KC9CHE", "EN60xg", 6322, 14097026, -11, "POTACAT"),
                  row(9003, "2026-11-06 12:34:00", 50, "G7ELK", "IO90jv", 18, 50294411, -18, "web-888")});

    // ---- power cut: wrong database password and no internet at start ----
    QTemporaryDir home;
    Security::writeDbCredential("admin", "not-yet", home.path(), "G4GCI");
    qputenv("WSPRLIVE_URL", "http://127.0.0.1:1/");
    LiveForm w;
    w.setHomeDir(home.path());
    w.show();
    auto *status = w.findChild<QLabel *>("statuslabel");
    auto *grid = w.findChild<QTableView *>("dataGridView1");
    CHECK(waitFor([&] { return status->text().contains("not reachable"); }));
    CHECK(w.callsign() == "G4GCI" && w.windowTitle().startsWith("Received transmissions for: G4GCI")
          && w.windowTitle().contains("WSPR Scheduler Live  V.") && w.windowTitle().endsWith("GNU GPLv3 License"));
    CHECK(waitFor([&] { return !answered.filter("unable to connect to wspr.live").isEmpty(); }));   // the start-up fetch is shown
    pump(2300);   // the timed message closes itself
    std::printf("     %s\n", qPrintable(status->text()));
    // MySQL and the internet come back: the next minute connects and fetches (C# needed a restart)
    Security::writeDbCredential("admin", "wspr", home.path(), "G4GCI");
    w.fetcher()->setBaseUrl(liveUrl);
    answered.clear();
    w.tick();
    CHECK(waitFor([&] { return w.model()->rowCount() == 3; }));
    CHECK(status->text().contains("3 reports from wspr.live, 3 new"));
    CHECK(fake.lastQuery().contains("LIKE '%G4GCI%'") && fake.lastQuery().contains("now(), 10)"));
    CHECK(answered.isEmpty());   // automatic: no pop-ups
    // newest first, values as in the C# grid
    auto cell = [&](int r, int c) { return grid->model()->index(r, c).data().toString(); };
    CHECK(cell(0, LiveTableModel::Reporter) == "G7ELK" && cell(0, LiveTableModel::Frequency) == "50.294411");
    CHECK(cell(1, LiveTableModel::Reporter) == "KC9CHE" && cell(1, LiveTableModel::Km) == "6322" && cell(1, LiveTableModel::Miles) == "3928");
    CHECK(cell(1, LiveTableModel::Cw) == "readable" && cell(1, LiveTableModel::Ssb) == "unusable" && cell(1, LiveTableModel::Snr) == "-11");
    CHECK(grid->model()->headerData(LiveTableModel::Cw, Qt::Horizontal).toString() == "CW @100W");

    // ---- automatic update every 5 minutes, no duplicates ----
    const int before = fake.count();
    for (int i = 0; i < 4; ++i) w.tick();
    pump(200);
    CHECK(fake.count() == before);
    fake.setBody({row(9003, "2026-11-06 12:34:00", 50, "G7ELK", "IO90jv", 18, 50294411, -18, "web-888"),
                  row(9004, "2026-11-06 12:36:00", 7, "VK2XYZ", "QF56", 17000, 7040100, -25, "WD_4.0")});
    w.tick();
    CHECK(waitFor([&] { return w.model()->rowCount() == 4; }));
    CHECK(fake.count() == before + 1 && status->text().contains("2 reports from wspr.live, 1 new"));

    // ---- CW/SSB comparison power ----
    w.findChild<QComboBox *>("CWSSBlistBox")->setCurrentText("1000");
    CHECK(grid->model()->headerData(LiveTableModel::Cw, Qt::Horizontal).toString() == "CW @1000W");

    // ---- filters: band, call, distance (miles), version search; automatic updates leave them alone ----
    auto *band = w.findChild<QComboBox *>("bandlistBox");
    band->setCurrentText("20m");
    w.findChild<QPushButton *>("filterbutton")->click();
    CHECK(w.model()->rowCount() == 2);
    band->setCurrentIndex(0);
    w.findChild<QLineEdit *>("callFiltertextBox")->setText("vk*");
    emit w.findChild<QLineEdit *>("callFiltertextBox")->textEdited("vk*");
    w.findChild<QPushButton *>("filterbutton")->click();
    CHECK(w.model()->rowCount() == 1 && cell(0, LiveTableModel::Reporter) == "VK2XYZ"
          && w.findChild<QLineEdit *>("callFiltertextBox")->text() == "VK");
    w.findChild<QLineEdit *>("callFiltertextBox")->clear();
    w.findChild<QLineEdit *>("DFromtextBox")->setText("100");   // miles
    w.findChild<QPushButton *>("filterbutton")->click();
    CHECK(w.model()->rowCount() == 3);   // G7ELK (11 miles) left out
    w.findChild<QLineEdit *>("DFromtextBox")->clear();
    for (int i = 0; i < 5; ++i) w.tick();   // automatic update: the filtered grid stays
    pump(300);
    CHECK(w.model()->rowCount() == 3);
    w.findChild<QPushButton *>("Clearbutton")->click();
    CHECK(w.model()->rowCount() == 4);
    // version search: click a Version cell, answer Yes
    int potacat = -1;
    for (int r = 0; r < 4; ++r) if (cell(r, LiveTableModel::Version) == "POTACAT") potacat = r;
    emit grid->clicked(grid->model()->index(potacat, LiveTableModel::Version));
    pump(100);
    CHECK(answered.contains("Search by version (Y/N)?") && w.model()->rowCount() == 1 && cell(0, LiveTableModel::Reporter) == "KC9CHE");
    w.findChild<QPushButton *>("updatebutton")->click();
    CHECK(w.model()->rowCount() == 4);

    // ---- Update now: one press a minute, then back to 10 minutes ----
    auto *period = w.findChild<QComboBox *>("PlistBox");
    period->setCurrentText("24 hours");
    w.findChild<QPushButton *>("Nowbutton")->click();
    CHECK(waitFor([&] { return fake.lastQuery().contains("now(), 1440)"); }));
    CHECK(w.findChild<QPushButton *>("Nowbutton")->text() == "Wait ..." && period->currentIndex() == 0);
    const int n2 = fake.count();
    w.findChild<QPushButton *>("Nowbutton")->click();
    pump(200);
    CHECK(fake.count() == n2);

    // ---- another call: shown, not saved ----
    fake.setBody({row(7001, "2026-11-06 12:40:00", 14, "W1AW", "FN31", 5300, 14097100, -9, "WD_4.0", "K1ABC")});
    auto *other = w.findChild<QCheckBox *>("othercheckBox");
    auto *otherCall = w.findChild<QLineEdit *>("calltextBox");
    other->setChecked(true);
    CHECK(otherCall->isEnabled() && !w.findChild<QPushButton *>("filterbutton")->isVisible());
    otherCall->setText("k1abc");
    emit otherCall->textEdited("k1abc");
    CHECK(w.windowTitle().startsWith("Received transmissions for: K1ABC"));
    emit otherCall->returnPressed();
    CHECK(waitFor([&] { return w.model()->rowCount() == 1; }));
    CHECK(fake.lastQuery().contains("LIKE '%K1ABC%'") && status->text().contains("(not saved)"));
    fake.setBody({});
    other->setChecked(false);
    CHECK(w.callsign() == "G4GCI" && w.model()->rowCount() == 4);   // own reports again; K1ABC's weren't saved

    // ---- Config: own call instead of WSPR Scheduler's (~/LiveCall) ----
    QTimer::singleShot(200, [&] {
        for (QWidget *tl : QApplication::topLevelWidgets())
            if (auto *d = qobject_cast<QDialog *>(tl); d && d->objectName() == "configgroupBox" && d->isVisible()) {
                d->findChild<QLineEdit *>("owncalltextBox")->setText("G4GCI/P");
                d->findChild<QCheckBox *>("prefcheckBox")->setChecked(true);
                d->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            }
    });
    answered.clear();
    w.findChild<QPushButton *>("configbutton")->click();
    pump(300);
    CHECK(answered.contains("Use this call as default (Y/N)?") && w.callsign() == "G4GCI/P");
    const auto lc = Live::readLiveCall(home.path());
    CHECK(lc && lc->call == "G4GCI/P" && lc->preferred);
    CHECK(waitFor([&] { return fake.lastQuery().contains("LIKE '%G4GCI/P%'"); }));

    // ---- test connection ----
    answered.clear();
    w.findChild<QPushButton *>("testDBbutton")->click();
    CHECK(waitFor([&] { return !answered.filter("successful GET").isEmpty(); }));

    w.resize(1137, 620);
    pump(100);
    w.grab().save("live_form.png");
    th.quit();
    th.wait();
    if (fails) for (const QString &a : answered) std::printf("  dialog: %s\n", qPrintable(a));
    std::printf(fails ? "liveform_uitest: %d FAILED\n" : "liveform_uitest: all passed\n", fails);
    return fails ? 1 : 0;
}
