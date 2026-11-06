// WSPR Scheduler Live core: row parsing, query, ratings, ~/LiveCall, fetching from a fake wspr.live
// (in its own thread) and the wspr_rx.reported store (scratch MySQL; skipped if there is none).
#include "credentials.h"
#include "livecore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMutex>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static QString row(qint64 id, const QString &time, int band, const QString &rx, const QString &rxLoc, int km,
                   int freq, int pwr, int snr, const QString &ver = "WD_3.3.2")
{
    return QString("%1\t%2\t%3\t%4\t51.5\t-0.1\t%5\tG4GCI\t50.6\t-1.6\tIO90gu\t%6\t75\t255\t%7\t%8\t%9\t0\t%10\t1")
        .arg(id).arg(time).arg(band).arg(rx, rxLoc).arg(km).arg(freq).arg(pwr).arg(snr).arg(ver);
}

// answers like db1.wspr.live: the query is in ?query=
class FakeLive : public QObject
{
public:
    QTcpServer *server = nullptr;
    QMutex lock;
    QStringList queries;
    QByteArray body;
    int status = 200;
    void start()
    {
        server = new QTcpServer(this);
        server->listen(QHostAddress::LocalHost);
        connect(server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *c = server->nextPendingConnection();
            connect(c, &QTcpSocket::readyRead, c, [this, c] {
                const QByteArray req = c->readAll();
                const QByteArray path = req.split(' ').value(1);
                const QString q = QUrl::fromPercentEncoding(QUrl(QString::fromLatin1(path)).query(QUrl::FullyEncoded)
                                                                .mid(6).toLatin1());
                QByteArray out;
                int st;
                {
                    QMutexLocker l(&lock);
                    queries << q;
                    out = q.startsWith("SELECT version()") ? QByteArray("24.8.4.13\n") : body;
                    st = status;
                }
                c->write("HTTP/1.1 " + QByteArray::number(st) + (st == 200 ? " OK" : " Error")
                         + "\r\nContent-Type: text/tab-separated-values\r\nContent-Length: "
                         + QByteArray::number(out.size()) + "\r\nConnection: close\r\n\r\n" + out);
                c->disconnectFromHost();
            });
        });
    }
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace Live;

    // ---- parsing (C#: process_data) ----
    const auto s = parseRow(row(9001, "2026-11-06 12:28:00", 14, "DL1ECN-8", "JO32uo", 660, 14097023, 37, -12));
    CHECK(s && s->id == 9001 && s->rxSign == "DL1ECN-8" && s->rxLoc == "JO32uo" && s->txSign == "G4GCI");
    CHECK(s && s->time == QDateTime(QDate(2026, 11, 6), QTime(12, 28), QTimeZone::utc()));
    CHECK(s && s->band == 14 && s->distance == 660 && s->frequency == 14097023 && s->power == 37 && s->snr == -12);
    CHECK(s && s->version == "WD_3.3.2" && s->code == 1 && std::abs(s->rxLat - 51.5) < 1e-9);
    CHECK(!parseRow("") && !parseRow("garbage\tline") && !parseRow("x\t2026-11-06 12:28:00\t14\tA\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t"));

    // ---- query ----
    CHECK(validCall("G4GCI") && validCall("G4GCI/P") && validCall("PJ4/K1ABC") && !validCall("G4GCI'; DROP") && !validCall("ab"));
    const QString q = liveQuery("G4GCI", 30, 1000);
    CHECK(q.contains("tx_sign LIKE '%G4GCI%'") && q.contains("subtractMinutes(now(), 30)") && q.contains("subtractMinutes(now(), 2)"));
    CHECK(q.contains("LIMIT 1000") && q.startsWith("SELECT id, time, band, rx_sign") && q.endsWith("FORMAT TabSeparated"));

    // ---- ratings and units (C#: getCW / getSSB / convertTodBm) ----
    CHECK(wattsToDbm(100) == 50 && wattsToDbm(5) == 36 && wattsToDbm(1000) == 60);
    // the screenshot: SNR -12 at 37 dBm, compared at 100 W (50 dBm) -> +1: "readable", SSB "unusable"
    CHECK(cwRating(-12, 37, 50) == "readable" && ssbRating(-12, 37, 50) == "unusable");
    CHECK(cwRating(+10, 37, 50) == "strong" && ssbRating(+10, 37, 50) == "excellent");   // HB9VQQ/RS
    CHECK(cwRating(-3, 37, 50) == "very good" && ssbRating(-3, 37, 50) == "workable");   // WZ7I
    CHECK(cwRating(-28, 37, 50) == "unusable" && cwRating(-18, 40, 50) == "very weak");
    CHECK(ssbRating(-5, 37, 50) == "borderline");
    CHECK(kmToMiles(6322) == 3928 && kmToMiles(18) == 11 && milesToKm(100) == 160);
    CHECK(bandForListIndex(0) == -2 && bandForListIndex(9) == 14 && bandForListIndex(15) == 50 && bandForListIndex(99) == -2);
    CHECK(bandName(14) == "20m" && bandName(-1) == "LF" && bandName(1296) == "23cm");

    // ---- ~/LiveCall and DBcredential's db_call ----
    QTemporaryDir home;
    CHECK(!readLiveCall(home.path()));
    CHECK(writeLiveCall({"g4gci/p", true}, home.path()));
    auto lc = readLiveCall(home.path());
    CHECK(lc && lc->call == "G4GCI/P" && lc->preferred);
    writeLiveCall({"G4GCI", false}, home.path());
    lc = readLiveCall(home.path());
    CHECK(lc && lc->call == "G4GCI" && !lc->preferred);
    CHECK(!Security::readDbCall(home.path()));
    Security::writeDbCredential("admin", "wspr", home.path(), "g4gci");
    CHECK(Security::readDbCall(home.path()) == QString("G4GCI") && Security::readDbPassword(home.path()) == QString("wspr"));

    // ---- fetching from a fake wspr.live ----
    QThread th;
    FakeLive fake;
    fake.moveToThread(&th);
    th.start();
    QMetaObject::invokeMethod(&fake, [&] { fake.start(); }, Qt::BlockingQueuedConnection);
    {
        QMutexLocker l(&fake.lock);
        fake.body = (row(9001, "2026-11-06 12:28:00", 14, "DL1ECN-8", "JO32uo", 660, 14097023, 37, -12) + "\n"
                     + row(9002, "2026-11-06 12:30:00", 14, "KC9CHE", "EN60xg", 6322, 14097026, 37, -11, "POTACAT") + "\n"
                     + "\n" + "not a row\n").toUtf8();
    }
    Fetcher f;
    f.setBaseUrl(QString("http://127.0.0.1:%1/").arg(fake.server->serverPort()));
    f.setTimeoutMs(3000);
    bool got = false, okFlag = false;
    QVector<Spot> spots;
    QString err;
    QObject::connect(&f, &Fetcher::fetched, [&](bool ok, const QVector<Spot> &sp, const QString &e) {
        got = true; okFlag = ok; spots = sp; err = e;
    });
    auto wait = [&](bool &flag) { QEventLoop l; QTimer t; QObject::connect(&t, &QTimer::timeout, &l, [&] { if (flag) l.quit(); });
                                  t.start(20); QTimer::singleShot(5000, &l, &QEventLoop::quit); l.exec(); };
    f.fetch("g4gci", 10);
    CHECK(f.busy());
    wait(got);
    CHECK(got && okFlag && spots.size() == 2 && spots[1].version == "POTACAT");
    const QVector<Spot> fromLive = spots;   // later fetches below fail and clear `spots`
    {
        QMutexLocker l(&fake.lock);
        CHECK(fake.queries.size() == 1 && fake.queries[0].contains("LIKE '%G4GCI%'") && fake.queries[0].contains("now(), 10)"));
    }
    got = false;
    f.fetch("bad'call", 10);   // never sent
    CHECK(got && !okFlag && err.contains("not valid"));
    { QMutexLocker l(&fake.lock); fake.status = 500; fake.body = "Code: 241. DB::Exception: Memory limit"; }
    got = false;
    f.fetch("G4GCI", 10);
    wait(got);
    CHECK(got && !okFlag && err.contains("HTTP 500") && err.contains("Memory limit"));
    { QMutexLocker l(&fake.lock); fake.status = 200; }
    bool tested = false, tOk = false;
    QString answer;
    QObject::connect(&f, &Fetcher::tested, [&](bool ok, const QString &a) { tested = true; tOk = ok; answer = a; });
    f.testConnection();
    wait(tested);
    CHECK(tested && tOk && answer == "24.8.4.13");
    // wspr.live not reachable (the power-cut case): an error, and the next fetch can still go
    f.setBaseUrl("http://127.0.0.1:1/");
    got = false;
    f.fetch("G4GCI", 10);
    wait(got);
    CHECK(got && !okFlag && !err.isEmpty() && !f.busy());
    std::printf("     unreachable: %s\n", qPrintable(err));
    th.quit();
    th.wait();

    // ---- the reported table (scratch MySQL) ----
    {
        QSqlDatabase probe = QSqlDatabase::addDatabase("QMYSQL", "probe");
        probe.setHostName("127.0.0.1");
        probe.setUserName("admin");
        probe.setPassword("wspr");
        if (!probe.open()) {
            std::printf("     no scratch MySQL: store part skipped\n");
        } else {
            QSqlQuery(probe).exec("CREATE DATABASE IF NOT EXISTS wspr_rx");
            QSqlQuery(probe).exec("DROP TABLE IF EXISTS wspr_rx.reported");
            probe.close();
        }
    }
    QSqlDatabase::removeDatabase("probe");
    Store st;
    if (st.open("127.0.0.1", "admin", "wspr")) {
        CHECK(st.ensureTable() && st.count() == 0);
        int added = -1;
        CHECK(st.save(fromLive, &added) && added == 2 && st.count() == 2);
        CHECK(st.save(fromLive, &added) && added == 0 && st.count() == 2);   // fetched again: no duplicates
        QVector<Spot> more = fromLive;
        Spot s3 = fromLive[0];
        s3.id = 9003; s3.rxSign = "VK2XYZ"; s3.band = 7; s3.distance = 17000; s3.frequency = 7040100;
        s3.time = QDateTime(QDate(2026, 11, 5), QTime(23, 0), QTimeZone::utc());
        more << s3;
        CHECK(st.save(more, &added) && added == 1 && st.count() == 3);
        const auto all = st.latest(5000);
        CHECK(all.size() == 3 && all[0].rxSign == "KC9CHE" && all[2].rxSign == "VK2XYZ");   // newest first
        CHECK(all[0].time == QDateTime(QDate(2026, 11, 6), QTime(12, 30), QTimeZone::utc()));   // stays UTC
        CHECK(st.latest(1).size() == 1);
        Filter fl;
        fl.band = 7;
        CHECK(st.select(fl, 5000).size() == 1);
        fl = Filter();
        fl.callPrefix = "DL*";
        CHECK(st.select(fl, 5000).size() == 1 && st.select(fl, 5000)[0].rxSign == "DL1ECN-8");
        fl = Filter();
        fl.minKm = 1000;
        fl.maxKm = 10000;
        CHECK(st.select(fl, 5000).size() == 1);
        fl = Filter();
        fl.useDates = true;
        fl.from = QDateTime(QDate(2026, 11, 6), QTime(0, 0), QTimeZone::utc());
        fl.to = QDateTime(QDate(2026, 11, 6), QTime(12, 29), QTimeZone::utc());
        CHECK(st.select(fl, 5000).size() == 1);
        fl = Filter();
        fl.version = "POTACAT";
        CHECK(st.select(fl, 5000).size() == 1);
        fl.version = "x' OR '1'='1";   // bound, not pasted into the SQL
        CHECK(st.select(fl, 5000).isEmpty() && st.lastError().isEmpty());
        // a table without a key on id (as some wspr_db.sql copies): still no duplicates
        QSqlQuery(QSqlDatabase::database(st.connectionName())).exec("DROP TABLE reported");
        QSqlQuery(QSqlDatabase::database(st.connectionName())).exec(
            "CREATE TABLE reported (id BIGINT, time DATETIME, band SMALLINT, rx_sign VARCHAR(20), rx_lat FLOAT, rx_lon FLOAT, "
            "rx_loc VARCHAR(8), tx_sign VARCHAR(20), tx_lat FLOAT, tx_lon FLOAT, tx_loc VARCHAR(8), distance INT, azimuth INT, "
            "rx_azimuth INT, frequency INT, power SMALLINT, snr SMALLINT, drift SMALLINT, version VARCHAR(30), code SMALLINT)");
        CHECK(st.ensureTable());
        CHECK(st.save(more, &added) && added == 3 && st.save(more, &added) && added == 0 && st.count() == 3);
        QSqlQuery iq(QSqlDatabase::database(st.connectionName()));
        iq.exec("SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema='wspr_rx' AND table_name='reported' "
                "AND index_name='idx_reported_id'");
        CHECK(iq.next() && iq.value(0).toInt() == 1);
        QSqlQuery(QSqlDatabase::database(st.connectionName())).exec("DROP TABLE reported");
        CHECK(st.ensureTable() && st.count() == 0);
        st.close();
        CHECK(!st.isOpen());
        Store bad;
        CHECK(!bad.open("127.0.0.1", "admin", "wrong") && !bad.lastError().isEmpty());
    }

    std::printf(failures ? "live_test: %d FAILED\n" : "live_test: all passed\n", failures);
    return failures ? 1 : 0;
}
