// WSPR Scheduler Live core - see livecore.h.
#include "livecore.h"

#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QTimeZone>
#include <QTimer>
#include <QSet>
#include <QTcpSocket>
#include <QUuid>
#include <cmath>

namespace Live {

namespace {
const char *kColumns = "id, time, band, rx_sign, rx_lat, rx_lon, rx_loc, tx_sign, tx_lat, tx_lon, tx_loc, "
                       "distance, azimuth, rx_azimuth, frequency, power, snr, drift, version, code";

QDateTime utcFrom(const QString &s)
{
    QDateTime t = QDateTime::fromString(s.trimmed(), "yyyy-MM-dd HH:mm:ss");
    if (!t.isValid())
        t = QDateTime::fromString(s.trimmed(), Qt::ISODate);
    if (t.isValid())
        t.setTimeZone(QTimeZone::utc());
    return t;
}
} // namespace

std::optional<Spot> parseRow(const QString &line)
{
    const QStringList r = line.split('\t');
    if (r.size() < 20)
        return std::nullopt;
    Spot s;
    bool ok = false;
    s.id = r[0].trimmed().toLongLong(&ok);
    if (!ok)
        return std::nullopt;
    s.time = utcFrom(r[1]);
    if (!s.time.isValid())
        return std::nullopt;
    s.band = r[2].toInt();
    s.rxSign = r[3].trimmed();
    s.rxLat = r[4].toDouble();
    s.rxLon = r[5].toDouble();
    s.rxLoc = r[6].trimmed();
    s.txSign = r[7].trimmed();
    s.txLat = r[8].toDouble();
    s.txLon = r[9].toDouble();
    s.txLoc = r[10].trimmed();
    s.distance = r[11].toInt();
    s.azimuth = r[12].toInt();
    s.rxAzimuth = r[13].toInt();
    s.frequency = r[14].toInt();
    s.power = r[15].toInt();
    s.snr = r[16].toInt();
    s.drift = r[17].toInt();
    s.version = r[18].trimmed();
    s.code = r[19].trimmed().toInt();
    if (s.rxSign.isEmpty())
        return std::nullopt;
    return s;
}

bool validCall(const QString &call)
{
    static const QRegularExpression re("^[A-Z0-9/]{3,15}$");
    return re.match(call).hasMatch();
}

QString liveQuery(const QString &call, int minutes, int limit)
{
    // C#: SELECT * ... tx_sign LIKE '%call%' (so G4GCI also finds G4GCI/P); columns named so a change
    // in wspr.live's column order can't shift the fields
    return QString("SELECT %1 FROM wspr.rx WHERE tx_sign LIKE '%%2%' AND time >= subtractMinutes(now(), %3) "
                   "AND time <= subtractMinutes(now(), 2) ORDER BY time DESC LIMIT %4 FORMAT TabSeparated")
        .arg(QString::fromLatin1(kColumns), call).arg(qMax(1, minutes)).arg(qMax(1, limit));
}

int wattsToDbm(int watts)
{
    if (watts <= 0)
        return 50;
    return int(10 * std::log10(watts * 1000.0));   // C#: (int)(10 * Math.Log10(pwrW * 1000))
}

// C#: getCW - the SNR the spot would have had at the comparison power
QString cwRating(int snr, int spotDbm, int compareDbm)
{
    const int l = snr + (compareDbm - spotDbm);
    if (l < -10) return "unusable";
    if (l < -6) return "very weak";
    if (l < -2) return "copyable";
    if (l < 3) return "readable";
    if (l < 8) return "solid";
    if (l < 13) return "very good";
    return "strong";
}

QString ssbRating(int snr, int spotDbm, int compareDbm)
{
    const int l = snr + (compareDbm - spotDbm);
    if (l < 5) return "unusable";
    if (l < 10) return "borderline";   // C#: "bordeline"
    if (l < 12) return "workable";
    if (l < 15) return "good";
    return "excellent";
}

int kmToMiles(int km) { return int(std::lround(km * 0.621371)); }
int milesToKm(int miles) { return int(miles * 1.609); }   // C#: (int)(k * 1.609)

int bandForListIndex(int index)
{
    static const int bands[] = {-2, -1, 0, 1, 3, 5, 7, 10, 13, 14, 18, 21, 24, 28, 40, 50, 70, 144, 432, 1296};
    return index >= 0 && index < int(sizeof bands / sizeof bands[0]) ? bands[index] : -2;
}

QString bandName(int band)
{
    switch (band) {
    case -1: return "LF";
    case 0: return "MF";
    case 1: return "160m";
    case 3: return "80m";
    case 5: return "60m";
    case 7: return "40m";
    case 10: return "30m";
    case 13: return "22m";
    case 14: return "20m";
    case 18: return "17m";
    case 21: return "15m";
    case 24: return "12m";
    case 28: return "10m";
    case 40: return "8m";
    case 50: return "6m";
    case 70: return "4m";
    case 144: return "2m";
    case 432: return "70cm";
    case 1296: return "23cm";
    default: return "?";
    }
}

static QString liveCallPath(const QString &homeDir)
{
    return QDir(homeDir.isEmpty() ? QDir::homePath() : homeDir).filePath("LiveCall");
}

std::optional<LiveCall> readLiveCall(const QString &homeDir)
{
    QFile f(liveCallPath(homeDir));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return std::nullopt;
    QTextStream in(&f);
    LiveCall c;
    c.call = in.readLine().trimmed().toUpper();
    c.preferred = in.readLine().trimmed().compare("yes", Qt::CaseInsensitive) == 0;
    if (c.call.isEmpty())
        return std::nullopt;
    return c;
}

bool writeLiveCall(const LiveCall &c, const QString &homeDir)
{
    QFile f(liveCallPath(homeDir));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        return false;
    QTextStream out(&f);
    out << c.call.trimmed().toUpper() << '\n' << (c.preferred ? "yes" : "no") << '\n';
    return true;
}

// ---- Fetcher ----------------------------------------------------------------------------------

Fetcher::Fetcher(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QNetworkReply *Fetcher::get(const QString &sql)
{
    QUrl url(m_base);
    url.setQuery("query=" + QString::fromLatin1(QUrl::toPercentEncoding(sql)), QUrl::StrictMode);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "WSPR Scheduler Live");
    req.setTransferTimeout(m_timeoutMs);
    return m_net->get(req);
}

void Fetcher::cancel()
{
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
}

void Fetcher::fetch(const QString &call, int minutes, int limit)
{
    if (m_reply)
        return;
    const QString c = call.trimmed().toUpper();
    if (!validCall(c)) {
        emit fetched(false, {}, "callsign \"" + call + "\" not valid");
        return;
    }
    m_reply = get(liveQuery(c, minutes, limit));
    QNetworkReply *r = m_reply;
    connect(r, &QNetworkReply::finished, this, [this, r] {
        if (r != m_reply)
            return;
        m_reply = nullptr;
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() != QNetworkReply::NoError || status != 200) {
            // wspr.live puts the reason (e.g. "too many requests") in the body of an error answer
            const QString body = QString::fromUtf8(r->readAll()).trimmed().left(200);
            emit fetched(false, {}, status > 0 && status != 200 ? QString("HTTP %1 %2").arg(status).arg(body)
                                                                : r->errorString());
            return;
        }
        QVector<Spot> spots;
        const QList<QByteArray> lines = r->readAll().split('\n');
        for (const QByteArray &l : lines)
            if (const auto s = parseRow(QString::fromUtf8(l)))
                spots << *s;
        emit fetched(true, spots, QString());
    });
}

void Fetcher::testConnection()
{
    QNetworkReply *r = get("SELECT version()");
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        const QString body = QString::fromUtf8(r->readAll()).trimmed();
        const bool ok = r->error() == QNetworkReply::NoError && !body.isEmpty();
        emit tested(ok, ok ? body : r->errorString());
    });
}

// ---- Store ------------------------------------------------------------------------------------

Store::Store() : m_name("live_" + QUuid::createUuid().toString(QUuid::Id128)) {}

Store::~Store()
{
    close();
}

bool Store::open(const QString &host, const QString &user, const QString &password)
{
    close();
    m_error.clear();
    m_host = host;
    m_user = user;
    m_password = password;
    {   // quick check first: a MySQL that isn't there would otherwise hold the window for the whole
        // connect timeout at every retry (on Windows even a refused connection takes about 2 s)
        QTcpSocket probe;
        probe.connectToHost(host.isEmpty() || host == "localhost" ? QString("127.0.0.1") : host, 3306);
        if (!probe.waitForConnected(300)) {
            m_error = QString("Can't connect to MySQL server on '%1' (port 3306 not answering)").arg(host);
            return false;
        }
        probe.abort();
    }
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", m_name);
        db.setHostName(host);
        db.setUserName(user);
        db.setPassword(password);
        db.setConnectOptions("MYSQL_OPT_CONNECT_TIMEOUT=5");
        db.setDatabaseName("wspr_rx");
        if (db.open())
            return true;
        m_error = db.lastError().text();
        // no wspr_rx database yet (a new PC): make it
        if (!m_error.contains("Unknown database", Qt::CaseInsensitive))
            return false;
        db.setDatabaseName(QString());
        if (!db.open()) {
            m_error = db.lastError().text();
            return false;
        }
        QSqlQuery q(db);
        if (!q.exec("CREATE DATABASE IF NOT EXISTS wspr_rx")) {
            m_error = q.lastError().text();
            return false;
        }
        db.close();
        db.setDatabaseName("wspr_rx");
        if (db.open()) {
            m_error.clear();
            return true;
        }
        m_error = db.lastError().text();
    }
    return false;
}

bool Store::isOpen() const
{
    return QSqlDatabase::contains(m_name) && QSqlDatabase::database(m_name, false).isOpen();
}

void Store::close()
{
    if (QSqlDatabase::contains(m_name)) {
        QSqlDatabase::database(m_name, false).close();
        QSqlDatabase::removeDatabase(m_name);
    }
}

bool Store::ensureTable()
{
    QSqlDatabase db = QSqlDatabase::database(m_name, false);
    QSqlQuery q(db);
    if (!q.exec("CREATE TABLE IF NOT EXISTS reported ("
                "id BIGINT NOT NULL PRIMARY KEY, time DATETIME NOT NULL, band SMALLINT NOT NULL DEFAULT 0, "
                "rx_sign VARCHAR(20) NOT NULL DEFAULT '', rx_lat FLOAT NOT NULL DEFAULT 0, rx_lon FLOAT NOT NULL DEFAULT 0, "
                "rx_loc VARCHAR(8) NOT NULL DEFAULT '', tx_sign VARCHAR(20) NOT NULL DEFAULT '', "
                "tx_lat FLOAT NOT NULL DEFAULT 0, tx_lon FLOAT NOT NULL DEFAULT 0, tx_loc VARCHAR(8) NOT NULL DEFAULT '', "
                "distance INT NOT NULL DEFAULT 0, azimuth INT NOT NULL DEFAULT 0, rx_azimuth INT NOT NULL DEFAULT 0, "
                "frequency INT NOT NULL DEFAULT 0, power SMALLINT NOT NULL DEFAULT 0, snr SMALLINT NOT NULL DEFAULT 0, "
                "drift SMALLINT NOT NULL DEFAULT 0, version VARCHAR(30) NOT NULL DEFAULT '', code SMALLINT NOT NULL DEFAULT 0)")) {
        m_error = q.lastError().text();
        return false;
    }
    // C#: EnsureIndexes, plus one on id when the table has none (a table from wspr_db.sql may not have a
    // primary key; without an index, checking for an already-saved spot would read the whole table)
    QSet<QString> have;
    bool idIndexed = false;
    if (q.exec("SELECT index_name, column_name, seq_in_index FROM information_schema.statistics "
               "WHERE table_schema = 'wspr_rx' AND table_name = 'reported'"))
        while (q.next()) {
            have.insert(q.value(0).toString().toLower());
            if (q.value(1).toString().compare("id", Qt::CaseInsensitive) == 0 && q.value(2).toInt() == 1)
                idIndexed = true;
        }
    QList<QPair<QString, QString>> idx = {{"idx_reported_time", "(time)"},
                                          {"idx_reported_band", "(band)"},
                                          {"idx_reported_time_band", "(time, band)"},
                                          {"idx_reported_rx_sign", "(rx_sign)"},
                                          {"idx_reported_distance", "(distance)"},
                                          {"idx_reported_version", "(version)"}};
    if (!idIndexed)
        idx.prepend({"idx_reported_id", "(id)"});
    for (const auto &i : idx)
        if (!have.contains(i.first))
            q.exec(QString("CREATE INDEX %1 ON reported %2").arg(i.first, i.second));   // an optimisation: errors ignored
    return true;
}

bool Store::save(const QVector<Spot> &spots, int *added)
{
    if (added)
        *added = 0;
    if (spots.isEmpty())
        return true;
    QSqlDatabase db = QSqlDatabase::database(m_name, false);
    if (!db.isOpen()) {
        m_error = "database not open";
        return false;
    }
    db.transaction();
    QSqlQuery q(db);
    // C#: INSERT IGNORE per row; NOT EXISTS as well, so a table without a key on id gets no duplicates
    q.prepare(QString("INSERT IGNORE INTO reported (%1) SELECT ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,? FROM DUAL "
                      "WHERE NOT EXISTS (SELECT 1 FROM reported WHERE id = ?)").arg(QString::fromLatin1(kColumns)));
    int n = 0;
    for (const Spot &s : spots) {
        const QVariantList v = {s.id, s.time.toUTC().toString("yyyy-MM-dd HH:mm:ss"), s.band, s.rxSign, s.rxLat, s.rxLon,
                                s.rxLoc, s.txSign, s.txLat, s.txLon, s.txLoc, s.distance, s.azimuth, s.rxAzimuth,
                                s.frequency, s.power, s.snr, s.drift, s.version, s.code, s.id};
        for (int i = 0; i < v.size(); ++i)
            q.bindValue(i, v[i]);
        if (!q.exec()) {
            m_error = q.lastError().text();
            db.rollback();
            return false;
        }
        if (q.numRowsAffected() > 0)
            ++n;
    }
    if (!db.commit()) {
        m_error = db.lastError().text();
        return false;
    }
    if (added)
        *added = n;
    return true;
}

static QVector<Spot> runOn(QSqlDatabase db, const QString &sql, const QVariantList &args, QString *err)
{
    QVector<Spot> out;
    QSqlQuery q(db);
    q.setForwardOnly(true);
    if (!q.prepare(sql)) {
        *err = q.lastError().text();
        return out;
    }
    for (int i = 0; i < args.size(); ++i)
        q.bindValue(i, args[i]);
    if (!q.exec()) {
        *err = q.lastError().text();
        return out;
    }
    err->clear();
    while (q.next()) {
        Spot s;
        s.id = q.value(0).toLongLong();
        s.time = utcFrom(q.value(1).toString());
        s.band = q.value(2).toInt();
        s.rxSign = q.value(3).toString();
        s.rxLat = q.value(4).toDouble();
        s.rxLon = q.value(5).toDouble();
        s.rxLoc = q.value(6).toString();
        s.txSign = q.value(7).toString();
        s.txLat = q.value(8).toDouble();
        s.txLon = q.value(9).toDouble();
        s.txLoc = q.value(10).toString();
        s.distance = q.value(11).toInt();
        s.azimuth = q.value(12).toInt();
        s.rxAzimuth = q.value(13).toInt();
        s.frequency = q.value(14).toInt();
        s.power = q.value(15).toInt();
        s.snr = q.value(16).toInt();
        s.drift = q.value(17).toInt();
        s.version = q.value(18).toString();
        s.code = q.value(19).toInt();
        out << s;
    }
    return out;
}

static QString selectColumns()
{
    // time as text: the MySQL driver would otherwise hand back a local-time QDateTime
    QString c = QString::fromLatin1(kColumns);
    c.replace("id, time,", "id, DATE_FORMAT(time, '%Y-%m-%d %H:%i:%s'),");
    return c;
}

static QVector<Spot> latestOn(QSqlDatabase db, int maxRows, QString *err)
{
    return runOn(db, QString("SELECT %1 FROM reported ORDER BY time DESC LIMIT %2").arg(selectColumns()).arg(qMax(1, maxRows)),
                 {}, err);
}

static bool hasIndex(QSqlDatabase db, const QString &name)
{
    QSqlQuery q(db);
    q.prepare("SELECT 1 FROM information_schema.statistics WHERE table_schema = DATABASE() "
              "AND table_name = 'reported' AND index_name = ? LIMIT 1");
    q.bindValue(0, name);
    return q.exec() && q.next();
}

// "WHERE rx_sign LIKE 'VK%' ORDER BY time DESC LIMIT n" on a big table: walking the time index from the
// newest end reads n * total / matches rows; reading the matches through the rx_sign index reads every
// one of them (113,000 VK spots out of 9 million, each from a different place on the disk: 35 s with
// XAMPP's small cache). With the (rx_sign, time, band, distance) index, the newest n are found and
// sorted in the index alone, and only those n rows are then read.
static QVector<Spot> selectOn(QSqlDatabase db, const Filter &f, int maxRows, QString *err)
{
    QStringList where;
    QVariantList args;
    bool useCallIndex = false;
    if (!f.version.isEmpty()) {
        where << "version = ?";
        args << f.version;
    } else {
        if (f.band != -2) {
            where << "band = ?";
            args << f.band;
        }
        if (f.useDates) {
            where << "time >= ?" << "time <= ?";
            args << f.from.toUTC().toString("yyyy-MM-dd HH:mm:ss") << f.to.toUTC().toString("yyyy-MM-dd HH:mm:ss");
        }
        if (!f.callPrefix.isEmpty()) {
            where << "rx_sign LIKE ?";
            QString p = f.callPrefix;
            p.remove('*').replace("%", "").replace("_", "\\_");
            const QString like = p + "%";
            args << like;
            useCallIndex = hasIndex(db, Store::kCallIndex);
        }
        if (f.minKm >= 0) {
            where << "distance >= ?";
            args << f.minKm;
        }
        if (f.maxKm >= 0) {
            where << "distance <= ?";
            args << f.maxKm;
        }
    }
    const QString limit = QString(" ORDER BY time DESC LIMIT %1").arg(qMax(1, maxRows));
    QString sql;
    if (useCallIndex)   // every condition here is in the index: the inner query never reads the table
        sql = "SELECT " + selectColumns() + " FROM reported JOIN (SELECT id AS kid FROM reported FORCE INDEX (`"
              + Store::kCallIndex + "`) WHERE " + where.join(" AND ") + limit + ") k ON id = k.kid" + limit;
    else {
        sql = "SELECT " + selectColumns() + " FROM reported";
        if (!where.isEmpty())
            sql += " WHERE " + where.join(" AND ");
        sql += limit;
    }
    return runOn(db, sql, args, err);
}

QVector<Spot> Store::run(const QString &sql, const QVariantList &args)
{
    return runOn(QSqlDatabase::database(m_name, false), sql, args, &m_error);
}

QVector<Spot> Store::latest(int maxRows)
{
    return latestOn(QSqlDatabase::database(m_name, false), maxRows, &m_error);
}

QVector<Spot> Store::select(const Filter &f, int maxRows)
{
    return selectOn(QSqlDatabase::database(m_name, false), f, maxRows, &m_error);
}

// a connection for this call only, in whichever thread calls it
static QVector<Spot> detached(const QString &host, const QString &user, const QString &password, QString *err,
                              const std::function<QVector<Spot>(QSqlDatabase)> &job)
{
    QVector<Spot> out;
    const QString name = "live_q_" + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", name);
        db.setHostName(host);
        db.setUserName(user);
        db.setPassword(password);
        db.setConnectOptions("MYSQL_OPT_CONNECT_TIMEOUT=5");
        db.setDatabaseName("wspr_rx");
        if (db.open())
            out = job(db);
        else
            *err = db.lastError().text();
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return out;
}

QVector<Spot> Store::latestDetached(int maxRows, QString *err) const
{
    return detached(m_host, m_user, m_password, err, [&](QSqlDatabase db) { return latestOn(db, maxRows, err); });
}

bool Store::buildCallIndexDetached(QString *err) const
{
    bool built = false;
    detached(m_host, m_user, m_password, err, [&](QSqlDatabase db) {
        if (hasIndex(db, kCallIndex))
            return QVector<Spot>();
        QSqlQuery q(db);
        // online: saving new spots and searching carry on while it is built
        const QString add = QString("ALTER TABLE reported ADD INDEX `%1` (rx_sign, time, band, distance)").arg(kCallIndex);
        built = q.exec(add + ", ALGORITHM=INPLACE, LOCK=NONE") || q.exec(add);
        if (!built)
            *err = q.lastError().text();
        return QVector<Spot>();
    });
    return built;
}

QVector<Spot> Store::selectDetached(const Filter &f, int maxRows, QString *err) const
{
    return detached(m_host, m_user, m_password, err, [&](QSqlDatabase db) { return selectOn(db, f, maxRows, err); });
}

int Store::count()
{
    QSqlQuery q(QSqlDatabase::database(m_name, false));
    if (!q.exec("SELECT COUNT(*) FROM reported") || !q.next()) {
        m_error = q.lastError().text();
        return -1;
    }
    return q.value(0).toInt();
}

} // namespace Live
