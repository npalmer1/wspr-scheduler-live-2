#pragma once
// WSPR Scheduler Live's window (C#: WSPR_Live.LiveForm). Reports of this station's transmissions from
// wspr.live, saved in wspr_rx.reported and shown in a grid like WSPR Scheduler's receive window.
//
// Differences from the C# version:
//  - keeps trying when MySQL or the internet isn't there yet (C# gave up if they were missing at start,
//    e.g. after a power cut, until it was restarted);
//  - all window updates happen on the UI thread (C# showed timed messages from a background thread,
//    the likely cause of "Destination array was not long enough ... WmDestroy");
//  - no pop-up every few minutes when updates are disabled or nothing new was found - the status line says so;
//  - an automatic update doesn't replace a filtered grid (C# did, hence "Disable updates");
//  - the filter values are passed to MySQL as parameters, not pasted into the SQL.
#include "livecore.h"

#include <QWidget>
#include <memory>

namespace Ui { class LiveForm; }
class LiveTableModel;
class QSortFilterProxyModel;
class QLabel;
class QTimer;

class LiveForm : public QWidget
{
    Q_OBJECT
public:
    static constexpr int kMaxRows = 5000;        // C#: maxrows
    static constexpr int kLiveLimit = 1000;      // C#: liveLimit - most spots asked of wspr.live at once
    static constexpr int kAutoEveryTicks = 5;    // C#: startCountMax - automatic update every 5 minutes

    explicit LiveForm(QWidget *parent = nullptr);
    ~LiveForm() override;

    // for tests
    void setHomeDir(const QString &dir) { m_home = dir; }
    Live::Fetcher *fetcher() const { return m_fetch; }
    LiveTableModel *model() const { return m_model; }
    QString callsign() const { return m_call; }
    void tick();                 // the once-a-minute timer (C#: timer2_Tick)
    void start();                // C#: LiveForm_Load (called once the window is shown)

private:
    bool connectDatabase();      // C#: checkDB + getUserandPassword + EnsureIndexes
    void readCallsigns();        // C#: getUserandPassword (db_call) + getCall (~/LiveCall)
    void setHeader();            // C#: set_header
    void fetch(int minutes, bool manual);   // C#: get_results
    void fetched(bool ok, const QVector<Live::Spot> &spots, const QString &error);
    void showResults();          // C#: show_results - the latest reports from the database
    void applyFilter(const QString &version = QString());   // C#: filter_results / find_selected
    int periodMinutes() const;   // C#: findPeriod
    void showWait(const QString &text);
    void hideWait();
    void status(const QString &text, bool error = false);
    void updateDistanceLimits();
    void openConfig();           // C#: configbutton / savebutton / saveCall

    // C# handlers
    void Nowbutton_Click();
    void othercheckBox_CheckedChanged(bool checked);
    void testDBbutton_Click();

    std::unique_ptr<Ui::LiveForm> ui;
    LiveTableModel *m_model;
    QSortFilterProxyModel *m_proxy;
    Live::Fetcher *m_fetch;
    Live::Store m_store;
    QTimer *m_tick;              // C#: timer2, once a minute
    QTimer *m_lockout;           // C#: timer1 - "Update now" can't be pressed again for a minute
    QLabel *m_wait = nullptr;    // C#: panel1 / Waitlabel
    QString m_home;              // where DBcredential and LiveCall are (tests)
    QString m_call;              // C#: Callsign
    QString m_originalCall;      // C#: originalcall - this station
    QString m_dbCall;            // C#: prev_call - the call WSPR Scheduler saved in DBcredential
    QString m_dbPassword;
    bool m_ownCall = true;       // C#: owncall (false: "Allow other call")
    bool m_dbOk = false;
    bool m_filtered = false;     // the grid shows a filter: automatic updates leave it alone
    bool m_retrySoon = false;    // last update failed: try again at the next minute
    bool m_manualFetch = false;
    bool m_started = false;
    int m_ticks = 0;             // C#: startCount
};
