#pragma once
// WSPR Scheduler Live, without the window: reports of this station's transmissions fetched from
// wspr.live and kept in wspr_rx.reported (which WSPR Scheduler Map reads for its TX map).
// C#: WSPR_Live LiveForm - process_data, get_results, Save_Received, find_received, find_selected,
// getCW / getSSB / convertTodBm, getCall / saveCall (~/LiveCall).
#include <QDateTime>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>
#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace Live {

// one row of wspr.live's wspr.rx table (C#: RX_data)
struct Spot
{
    qint64 id = 0;
    QDateTime time;          // UTC
    int band = 0;            // wspr.live band number (14 = 20 m, -1 = LF, 0 = MF)
    QString rxSign;          // the station that heard us
    double rxLat = 0, rxLon = 0;
    QString rxLoc;
    QString txSign;
    double txLat = 0, txLon = 0;
    QString txLoc;
    int distance = 0;        // km
    int azimuth = 0, rxAzimuth = 0;
    int frequency = 0;       // Hz
    int power = 0;           // dBm
    int snr = 0, drift = 0;
    QString version;
    int code = 0;
};

// C#: process_data - a tab-separated row in wspr.rx column order; nullopt if it isn't one
std::optional<Spot> parseRow(const QString &line);

// The wspr.live query (C#: get_results): spots for a call in the last `minutes`, up to 2 minutes ago
QString liveQuery(const QString &call, int minutes, int limit);
bool validCall(const QString &call);   // letters, digits and / only (it goes into the query)

// C#: convertTodBm, getCW, getSSB, convert_to_miles, get_band
int wattsToDbm(int watts);
QString cwRating(int snr, int spotDbm, int compareDbm);
QString ssbRating(int snr, int spotDbm, int compareDbm);
int kmToMiles(int km);
int milesToKm(int miles);
int bandForListIndex(int index);       // band list row -> band number, -2 = all
QString bandName(int band);            // 14 -> "20m"

// ~/LiveCall: own call on the first line, "yes" on the second if it replaces WSPR Scheduler's call
struct LiveCall
{
    QString call;
    bool preferred = false;
};
std::optional<LiveCall> readLiveCall(const QString &homeDir = QString());
bool writeLiveCall(const LiveCall &c, const QString &homeDir = QString());

// ---- wspr.live --------------------------------------------------------------------------------

class Fetcher : public QObject
{
    Q_OBJECT
public:
    explicit Fetcher(QObject *parent = nullptr);
    void setBaseUrl(const QString &url) { m_base = url; }   // tests
    QString baseUrl() const { return m_base; }
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    bool busy() const { return m_reply != nullptr; }
    // spots for `call` in the last `minutes`; finished() follows (never blocks)
    void fetch(const QString &call, int minutes, int limit = 1000);
    void testConnection();     // finished-style answer in tested()
    void cancel();

signals:
    void fetched(bool ok, const QVector<Live::Spot> &spots, const QString &error);
    void tested(bool ok, const QString &answer);

private:
    QNetworkReply *get(const QString &sql);
    QNetworkAccessManager *m_net;
    QNetworkReply *m_reply = nullptr;
    QString m_base = QStringLiteral("http://db1.wspr.live/");
    int m_timeoutMs = 20000;
};

// ---- wspr_rx.reported --------------------------------------------------------------------------

struct Filter
{
    bool useDates = false;
    QDateTime from, to;      // UTC
    int band = -2;           // -2 = all
    QString callPrefix;      // reporter starts with
    int minKm = -1, maxKm = -1;
    QString version;         // only this version (version search)
};

class Store
{
public:
    Store();
    ~Store();
    // C#: 127.0.0.1 / admin / the DBcredential password
    bool open(const QString &host, const QString &user, const QString &password);
    bool isOpen() const;
    void close();
    bool ensureTable();       // creates wspr_rx.reported if it isn't there, and the indexes (C#: EnsureIndexes)
    // C#: Save_Received for each spot - already-saved spots (same id) are skipped; *added = new rows
    bool save(const QVector<Spot> &spots, int *added = nullptr);
    QVector<Spot> latest(int maxRows);                       // C#: find_received
    QVector<Spot> select(const Filter &f, int maxRows);      // C#: find_selected
    // the same two, but safe to call from another thread: they use a connection of their own (opened
    // and closed for the call), so a slow search doesn't hold up the window; *err = "" when it worked
    QVector<Spot> latestDetached(int maxRows, QString *err) const;
    QVector<Spot> selectDetached(const Filter &f, int maxRows, QString *err) const;
    // the index that makes callsign searches quick (rx_sign, time, band, distance): built once, which on
    // a big table takes a minute or so - so call this from another thread. true: it was built just now
    bool buildCallIndexDetached(QString *err) const;
    static constexpr const char *kCallIndex = "idx_reported_call";
    int count();
    QString lastError() const { return m_error; }
    QString connectionName() const { return m_name; }

private:
    QVector<Spot> run(const QString &sql, const QVariantList &args);
    QString m_name;
    QString m_host, m_user, m_password;   // for the detached connections
    QString m_error;
};

} // namespace Live

Q_DECLARE_METATYPE(Live::Spot)
