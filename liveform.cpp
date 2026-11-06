// WSPR Scheduler Live window - see liveform.h.
#include "liveform.h"
#include "ui_liveform.h"

#include "credentials.h"
#include "loaderror.h"
#include "livetablemodel.h"
#include "msg.h"

#include <memory>

#include <QApplication>
#include <QCheckBox>
#include <QSqlDatabase>
#include <QDialog>
#include <QDir>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QHeaderView>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QSortFilterProxyModel>
#include <QTimeZone>
#include <QThread>
#include <QTimer>

#ifndef WSPRLIVE_VERSION
#define WSPRLIVE_VERSION "0.0.0"   // set by CMakeLists.txt project(VERSION)
#endif

namespace {
const char *kTitleApp = "WSPR Scheduler Live  V." WSPRLIVE_VERSION "    GNU GPLv3 License";

// keeps the "please wait" panel in the middle of the window
class CentreOnParent : public QObject
{
public:
    CentreOnParent(QWidget *w) : QObject(w), m_w(w) { w->parentWidget()->installEventFilter(this); }
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() == QEvent::Resize)
            centre();
        return false;
    }
    void centre()
    {
        const QSize p = m_w->parentWidget()->size();
        m_w->move((p.width() - m_w->width()) / 2, (p.height() - m_w->height()) / 2);
    }

private:
    QWidget *m_w;
};
} // namespace

LiveForm::LiveForm(QWidget *parent)
    : QWidget(parent), ui(std::make_unique<Ui::LiveForm>()), m_model(new LiveTableModel(this)),
      m_proxy(new QSortFilterProxyModel(this)), m_fetch(new Live::Fetcher(this)), m_tick(new QTimer(this)),
      m_lockout(new QTimer(this))
{
    ui->setupUi(this);
    // background: the standard window grey, as the C# WSPR Live (SystemColors.Control)
    const QString testUrl = qEnvironmentVariable("WSPRLIVE_URL");   // tests only
    if (!testUrl.isEmpty())
        m_fetch->setBaseUrl(testUrl);

    // grid
    m_proxy->setSourceModel(m_model);
    m_proxy->setSortRole(LiveTableModel::SortRole);
    ui->dataGridView1->setModel(m_proxy);
    ui->dataGridView1->sortByColumn(LiveTableModel::Date, Qt::DescendingOrder);
    const int widths[] = {110, 100, 87, 44, 45, 64, 44, 110, 68, 52, 52, 45, 87, 87, 90};
    for (int c = 0; c < LiveTableModel::ColumnCount; ++c)
        ui->dataGridView1->setColumnWidth(c, widths[c]);
    ui->dataGridView1->horizontalHeader()->setStretchLastSection(true);
    ui->dataGridView1->verticalHeader()->setDefaultSectionSize(20);
    ui->dataGridView1->verticalHeader()->setMinimumWidth(18);
    connect(ui->dataGridView1, &QAbstractItemView::clicked, this, [this](const QModelIndex &i) {
        // C#: dataGridView1_CellContentClick - the version column searches by version
        if (i.column() != LiveTableModel::Version || !m_ownCall)
            return;
        const QString v = m_proxy->data(i).toString().trimmed();
        if (!v.isEmpty() && Msg::yesNo(this, "Search by version (Y/N)?", "Version"))
            applyFilter(v);
    });

    // filters (times are UTC, as in the database)
    for (QDateTimeEdit *e : {ui->dateTimePicker1, ui->dateTimePicker2}) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        e->setTimeZone(QTimeZone::utc());
#else
        e->setTimeSpec(Qt::UTC);
#endif
    }
    const QDateTime now = QDateTime::currentDateTimeUtc();
    ui->dateTimePicker1->setDateTime(now.addDays(-1));
    ui->dateTimePicker2->setDateTime(now);
    ui->label2->setText("From (UTC):");
    auto upper = [](QLineEdit *e) {
        QObject::connect(e, &QLineEdit::textEdited, e, [e](const QString &t) {
            const int pos = e->cursorPosition();
            e->setText(t.toUpper());   // C#: CharacterCasing.Upper
            e->setCursorPosition(pos);
        });
    };
    ui->callFiltertextBox->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9./*_-]*"), this));
    ui->calltextBox->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9/]{0,15}"), this));
    upper(ui->callFiltertextBox);
    upper(ui->calltextBox);
    updateDistanceLimits();
    ui->bandlistBox->setCurrentIndex(0);
    ui->PlistBox->setCurrentIndex(0);
    ui->CWSSBlistBox->setCurrentText("100");
    ui->disabledlabel->setVisible(false);
    ui->statuslabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    // the "please wait" panel (C#: panel1 / Waitlabel)
    m_wait = new QLabel(this);
    m_wait->setObjectName("Waitlabel");
    m_wait->setAlignment(Qt::AlignCenter);
    m_wait->setStyleSheet("QLabel#Waitlabel { background-color: seashell; border: 1px solid #b0a090; padding: 20px; }");
    m_wait->setFixedSize(320, 110);
    m_wait->hide();
    auto *centre = new CentreOnParent(m_wait);
    centre->centre();
    m_loadError = new LoadErrorLabel("WSPR Scheduler Live", this);

    connect(ui->filterbutton, &QPushButton::clicked, this, [this] { applyFilter(); });
    connect(ui->Clearbutton, &QPushButton::clicked, this, [this] { showResults(); });
    connect(ui->updatebutton, &QPushButton::clicked, this, [this] { showResults(); });
    connect(ui->Nowbutton, &QPushButton::clicked, this, &LiveForm::Nowbutton_Click);
    connect(ui->testDBbutton, &QPushButton::clicked, this, &LiveForm::testDBbutton_Click);
    connect(ui->configbutton, &QPushButton::clicked, this, &LiveForm::openConfig);
    connect(ui->othercheckBox, &QCheckBox::toggled, this, &LiveForm::othercheckBox_CheckedChanged);
    connect(ui->kmcheckBox, &QCheckBox::toggled, this, [this] { updateDistanceLimits(); });
    connect(ui->updatecheckBox, &QCheckBox::toggled, this, [this](bool on) {
        ui->disabledlabel->setVisible(on);
        status(on ? "Updates from wspr.live disabled" : "Updates from wspr.live enabled");
    });
    connect(ui->CWSSBlistBox, &QComboBox::currentTextChanged, this, [this](const QString &t) {
        m_model->setCompareWatts(t.toInt());   // C#: CWSSBlistBox_SelectedValueChanged
    });
    for (QLineEdit *e : {ui->DFromtextBox, ui->DTotextBox, ui->callFiltertextBox})
        connect(e, &QLineEdit::returnPressed, this, [this] { applyFilter(); });
    // C#: calltextBox_TextChanged / calltextBox_KeyDown
    connect(ui->calltextBox, &QLineEdit::textChanged, this, [this](const QString &t) {
        m_call = t.trimmed().isEmpty() ? m_originalCall : t.trimmed().toUpper();
        setHeader();
    });
    connect(ui->calltextBox, &QLineEdit::returnPressed, this, [this] {
        if (ui->calltextBox->text().trimmed().isEmpty())
            return;
        m_lockout->stop();
        ui->Nowbutton->setText("Update now");
        Nowbutton_Click();
    });

    connect(m_fetch, &Live::Fetcher::fetched, this, &LiveForm::fetched);
    connect(m_fetch, &Live::Fetcher::tested, this, [this](bool ok, const QString &answer) {
        if (ok)
            Msg::ok(this, "SQL version: " + answer + " - successful GET", "wspr.live");
        else
            Msg::ok(this, "Error connecting to wspr.live: " + answer, "wspr.live");
    });
    m_tick->setInterval(60000);
    connect(m_tick, &QTimer::timeout, this, &LiveForm::tick);
    m_lockout->setSingleShot(true);
    m_lockout->setInterval(60000);
    connect(m_lockout, &QTimer::timeout, this, [this] { ui->Nowbutton->setText("Update now"); });   // C#: timer1_Tick

    resize(1137, 620);
    setHeader();
    QTimer::singleShot(0, this, &LiveForm::start);
}

LiveForm::~LiveForm()
{
    if (m_indexThread) {   // closed while the index is being built: let MySQL finish it
        m_indexThread->wait();
        delete m_indexThread;
    }
}

// C#: LiveForm_Load
void LiveForm::start()
{
    if (m_started)
        return;
    m_started = true;
    readCallsigns();
    setHeader();
    m_tick->start();
    if (!QSqlDatabase::isDriverAvailable("QMYSQL")) {
        status("Qt's MySQL driver is missing", true);
        Msg::ok(this,
                "Qt's MySQL driver is missing, so the database can't be opened (the server itself may be fine).\n\n"
                "Put qsqlmysql.dll (built for this Qt version and compiler) in:\n"
                + QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/sqldrivers")
                + "\nand libmysql.dll, libcrypto-3-x64.dll, libssl-3-x64.dll in:\n"
                + QDir::toNativeSeparators(QCoreApplication::applicationDirPath()),
                "MySQL driver not found");
    } else if (!connectDatabase()) {
        m_loadError->showError(m_store.lastError(), true);   // C#: LiveForm_Shown - LoadError
    }
    if (m_call.isEmpty()) {
        status("No callsign: set yours with Config", true);
        Msg::timed(this, "No callsign configured - see Config", "Callsign configuration", 3000);
        return;
    }
    if (m_dbOk)
        showResults();
    fetch(30, true);   // C#: the last 30 minutes at start-up
}

bool LiveForm::connectDatabase()
{
    m_dbPassword = Security::readDbPassword(m_home).value_or("wspr");
    m_dbOk = m_store.open("127.0.0.1", "admin", m_dbPassword) && m_store.ensureTable();
    if (!m_dbOk)
        status("Database not available (" + m_store.lastError().left(120) + ") - trying again every minute", true);
    else
        buildCallIndex();
    return m_dbOk;
}

// once per run, in the background: the index for quick callsign searches (nothing to do once it's there)
void LiveForm::buildCallIndex()
{
    if (m_indexThread)
        return;
    struct State { bool built = false, announced = false; QString err; };
    const auto st = std::make_shared<State>();
    m_indexThread = QThread::create([this, st] { st->built = m_store.buildCallIndexDetached(&st->err); });
    connect(m_indexThread, &QThread::finished, this, [this, st] {
        if (st->built && st->announced)
            status("Callsign search index ready");
        else if (!st->built && !st->err.isEmpty())
            status("Couldn't make the callsign search index (" + st->err.left(120) + "): callsign searches may be slow", true);
    });
    m_indexThread->start();
    // only say so when it takes a while (it's there already, or the table is small: nothing to see)
    QTimer::singleShot(1500, this, [this, st] {
        if (m_indexThread->isRunning()) {
            st->announced = true;
            status("Building the callsign search index (only once - a minute or two on a big table) ...");
        }
    });
}

// C#: getUserandPassword reads "db_call:" (WSPR Scheduler's call); getCall reads ~/LiveCall and uses
// its call instead when "Use this call instead of the one from WSPR Scheduler" was ticked
void LiveForm::readCallsigns()
{
    m_dbCall = Security::readDbCall(m_home).value_or(QString());
    m_call = m_dbCall;
    if (const auto lc = Live::readLiveCall(m_home))
        if (lc->preferred || m_call.isEmpty())
            m_call = lc->call;
    m_originalCall = m_call;
}

void LiveForm::setHeader()
{
    const QString call = m_call.isEmpty() ? QString("(no callsign)") : m_call;
    setWindowTitle("Received transmissions for: " + call + "                " + QString::fromLatin1(kTitleApp));
}

void LiveForm::status(const QString &text, bool error)
{
    ui->statuslabel->setText(QDateTime::currentDateTimeUtc().toString("HH:mm") + " UTC  " + text);
    ui->statuslabel->setStyleSheet(error ? "color: #b00000;" : QString());
}

void LiveForm::showWait(const QString &text)
{
    m_wait->setText(text);
    m_wait->show();
    m_wait->raise();
    m_wait->repaint();   // database reads block: paint the panel before they start
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}


void LiveForm::hideWait()
{
    m_wait->hide();
}

// C#: findPeriod (from PlistBox)
int LiveForm::periodMinutes() const
{
    static const int minutes[] = {10, 20, 30, 60, 180, 360, 720, 1440};
    const int i = ui->PlistBox->currentIndex();
    return i >= 0 && i < 8 ? minutes[i] : 10;
}

// C#: timer2_Tick / updatePassandCall - every minute; an update from wspr.live every 5 minutes, or at the
// next minute after one failed. Also reconnects the database if it wasn't there.
void LiveForm::tick()
{
    if (!m_dbOk && QSqlDatabase::isDriverAvailable("QMYSQL") && connectDatabase()) {
        m_loadError->hide();   // the database is there now
        status("Database connected");
        if (!m_filtered && m_ownCall)
            showResults();
        m_retrySoon = true;
    }
    if (m_call.isEmpty() || ui->updatecheckBox->isChecked())
        return;   // C# put up "Updates disabled" every minute
    if (++m_ticks >= kAutoEveryTicks || m_retrySoon) {
        m_ticks = 0;
        fetch(periodMinutes(), false);
    }
}

void LiveForm::fetch(int minutes, bool manual)
{
    if (m_fetch->busy())
        return;
    if (m_call.isEmpty()) {
        status("No callsign: set yours with Config", true);
        return;
    }
    m_manualFetch = manual;
    showWait("Retrieving live data ... please wait");
    status(QString("Asking wspr.live for reports of %1 in the last %2 minutes ...").arg(m_call).arg(minutes));
    m_fetch->fetch(m_call, minutes, kLiveLimit);
}

void LiveForm::fetched(bool ok, const QVector<Live::Spot> &spots, const QString &error)
{
    const bool manual = m_manualFetch;
    m_manualFetch = false;
    hideWait();
    if (!ok) {
        m_retrySoon = true;   // the power-cut case: keep trying, every minute
        status("wspr.live not reachable (" + error.left(120) + ") - trying again in a minute", true);
        if (manual)
            Msg::timed(this, "Error - unable to connect to wspr.live", "Connection error", 2000);
        return;
    }
    m_retrySoon = false;
    if (!m_ownCall) {   // another call: shown, not saved
        m_model->setRows(spots);
        ui->dataGridView1->sortByColumn(LiveTableModel::Date, Qt::DescendingOrder);
        status(QString("%1 reports of %2 from wspr.live (not saved)").arg(spots.size()).arg(m_call));
        if (spots.isEmpty() && manual)
            Msg::timed(this, "Unable to find live reports for " + m_call, "Data for call " + m_call, 3000);
        return;
    }
    if (spots.isEmpty()) {
        status("No new reports from wspr.live");
        if (manual)
            Msg::timed(this, "No live reports for " + m_call + " in this period", "Data for call " + m_call, 3000);
        return;
    }
    if (!m_dbOk) {
        status(QString("%1 reports from wspr.live, but the database isn't available to save them").arg(spots.size()), true);
        m_model->setRows(spots);
        return;
    }
    int added = 0;
    if (!m_store.save(spots, &added)) {
        status("Reports not saved: " + m_store.lastError().left(150), true);
        m_dbOk = m_store.isOpen();
        return;
    }
    status(QString("%1 reports from wspr.live, %2 new").arg(spots.size()).arg(added));
    if (!m_filtered)   // C# replaced the grid even in the middle of a search
        showResults();
}

QVector<Live::Spot> LiveForm::readInBackground(const std::function<QVector<Live::Spot>(QString *)> &job, QString *err)
{
    QVector<Live::Spot> rows;
    m_querying = true;
    QEventLoop loop;
    QThread *t = QThread::create([&] { rows = job(err); });
    connect(t, &QThread::finished, &loop, &QEventLoop::quit);
    t->start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);   // paints (the wait panel) and timers; clicks wait till after
    t->wait();
    delete t;
    m_querying = false;
    return rows;
}

// C#: show_results / find_received
void LiveForm::showResults()
{
    if (m_querying) {   // e.g. new reports arrived while a search was running: do it afterwards
        m_refreshPending = true;
        return;
    }
    m_refreshPending = false;
    m_filtered = false;
    if (!m_dbOk && !connectDatabase())
        return;
    showWait("Retrieving local data ... please wait");   // C#: updateResults
    QString err;
    const auto rows = readInBackground([this](QString *e) { return m_store.latestDetached(kMaxRows, e); }, &err);
    if (m_fetch->busy())
        m_wait->setText("Retrieving live data ... please wait");
    else
        hideWait();
    if (!err.isEmpty()) {
        status("Error retrieving data: " + err.left(150), true);
        return;
    }
    m_model->setRows(rows);
    ui->dataGridView1->sortByColumn(LiveTableModel::Date, Qt::DescendingOrder);
}

// C#: filter_results / find_selected
void LiveForm::applyFilter(const QString &version)
{
    if (m_querying)
        return;   // one search at a time
    if (!m_dbOk && !connectDatabase())
        return;
    Live::Filter f;
    f.version = version;
    f.band = Live::bandForListIndex(ui->bandlistBox->currentIndex());
    f.useDates = ui->datecheckBox->isChecked();
    f.from = ui->dateTimePicker1->dateTime().toUTC();
    f.to = ui->dateTimePicker2->dateTime().toUTC();
    QString call = ui->callFiltertextBox->text().trimmed();
    call.remove('*');
    ui->callFiltertextBox->setText(call);   // C#: removed the * from the box too
    f.callPrefix = call;
    const bool km = ui->kmcheckBox->isChecked();
    auto dist = [km](const QString &t) {
        bool ok = false;
        const int v = t.trimmed().toInt(&ok);
        return ok ? (km ? v : Live::milesToKm(v)) : -1;
    };
    f.minKm = dist(ui->DFromtextBox->text());
    f.maxKm = dist(ui->DTotextBox->text());
    showWait("Searching ... please wait");   // C#: filterbutton_Click / version search
    QString err;
    QElapsedTimer took;
    took.start();
    const auto rows = readInBackground([this, f](QString *e) { return m_store.selectDetached(f, kMaxRows, e); }, &err);
    const QString secs = QString::number(took.elapsed() / 1000.0, 'f', 1);
    hideWait();
    if (!err.isEmpty()) {
        Msg::timed(this, "Error retrieving data - please try again", "Database error", 2000);
        if (m_refreshPending)
            showResults();
        return;
    }
    m_filtered = true;
    m_model->setRows(rows);
    ui->dataGridView1->sortByColumn(LiveTableModel::Date, Qt::DescendingOrder);
    status(QString("Filter: %1 reports%2 in %3 s").arg(rows.size()).arg(version.isEmpty() ? QString() : " from " + version, secs)
           + " (Clear to show the latest again)");
}

void LiveForm::updateDistanceLimits()
{
    const bool km = ui->kmcheckBox->isChecked();
    ui->Ulabel->setText(km ? "km" : "mls");
    ui->Dlabel->setText(km ? "0 - 20,035" : "0 - 12,453");   // half way round the earth
    const int max = km ? 20035 : 12453;
    for (QLineEdit *e : {ui->DFromtextBox, ui->DTotextBox})
        e->setValidator(new QIntValidator(0, max, e));
}

// C#: Nowbutton_Click - the update period now, then no second press for a minute
void LiveForm::Nowbutton_Click()
{
    if (ui->updatecheckBox->isChecked()) {
        Msg::timed(this, "Updates disabled", "", 2000);
        return;
    }
    if (m_lockout->isActive())
        return;   // "Wait ..."
    fetch(periodMinutes(), true);
    ui->PlistBox->setCurrentIndex(0);   // C#: back to 10 minutes for the automatic updates
    m_ticks = 0;
    m_lockout->start();
    ui->Nowbutton->setText("Wait ...");
}

// C#: othercheckBox_CheckedChanged
void LiveForm::othercheckBox_CheckedChanged(bool checked)
{
    m_lockout->stop();
    ui->Nowbutton->setText("Update now");
    ui->calltextBox->setEnabled(checked);
    m_ownCall = !checked;
    ui->filterbutton->setVisible(!checked);
    ui->Clearbutton->setVisible(!checked);
    if (checked) {
        if (!ui->calltextBox->text().trimmed().isEmpty()) {
            m_call = ui->calltextBox->text().trimmed().toUpper();
            setHeader();
            fetch(periodMinutes(), true);
        }
    } else {
        m_call = m_originalCall;
        setHeader();
        showResults();
        fetch(periodMinutes(), true);
    }
}

void LiveForm::testDBbutton_Click()
{
    m_fetch->testConnection();
}

// C#: configbutton_Click / savebutton_Click / saveCall
void LiveForm::openConfig()
{
    QDialog d(this);
    d.setObjectName("configgroupBox");
    d.setWindowTitle("Configuration");
    d.setStyleSheet("QDialog#configgroupBox { background-color: cornsilk; }");
    auto *lay = new QFormLayout(&d);
    auto *call = new QLineEdit(&d);
    call->setObjectName("owncalltextBox");
    call->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9/]{0,15}"), call));
    const auto lc = Live::readLiveCall(m_home);
    call->setText(lc ? lc->call : m_originalCall);
    auto *pref = new QCheckBox("Use this call instead of the one from WSPR Scheduler", &d);
    pref->setObjectName("prefcheckBox");
    pref->setChecked(lc && lc->preferred);
    auto *note = new QLabel(m_dbCall.isEmpty() ? QString("WSPR Scheduler hasn't saved a callsign (DBcredential)")
                                               : "WSPR Scheduler's callsign: " + m_dbCall, &d);
    lay->addRow("Own callsign:", call);
    lay->addRow(QString(), pref);
    lay->addRow(QString(), note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &d);
    lay->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted)
        return;

    Live::LiveCall c{call->text().trimmed().toUpper(), pref->isChecked()};
    if (c.preferred && !Msg::yesNo(this, "Use this call as default (Y/N)?", "Save preferences"))
        c.preferred = false;
    if (c.preferred && !Live::validCall(c.call)) {
        Msg::ok(this, "Enter a valid callsign", "Callsign");
        return;
    }
    if (!Live::writeLiveCall(c, m_home)) {
        Msg::timed(this, "Unable to save callsign", "Callsign", 2000);
        return;
    }
    const QString before = m_originalCall;
    m_originalCall = c.preferred ? c.call : (m_dbCall.isEmpty() ? c.call : m_dbCall);
    if (m_ownCall)
        m_call = m_originalCall;
    setHeader();
    if (m_originalCall != before && m_ownCall) {
        Msg::timed(this, "Saving and updating - please wait", "Saving", 2000);
        fetch(30, true);
    }
}
