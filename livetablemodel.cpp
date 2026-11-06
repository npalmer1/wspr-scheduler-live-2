#include "livetablemodel.h"

namespace {
const char *kHeadings[] = {"Date", "Call", "Frequency", "SNR", "Drift", "TX loc", "dBm", "Reporter", "RX Loc",
                           "km", "miles", "Az", "CW @100W", "SSB @100W", "Version"};
}

LiveTableModel::LiveTableModel(QObject *parent) : QAbstractTableModel(parent) {}

int LiveTableModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_rows.size(); }
int LiveTableModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : ColumnCount; }

void LiveTableModel::setRows(const QVector<Live::Spot> &rows)
{
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

void LiveTableModel::setCompareWatts(int watts)
{
    m_watts = watts > 0 ? watts : 100;
    m_compareDbm = Live::wattsToDbm(m_watts);
    emit headerDataChanged(Qt::Horizontal, Cw, Ssb);
    if (!m_rows.isEmpty())
        emit dataChanged(index(0, Cw), index(m_rows.size() - 1, Ssb));
}

QString LiveTableModel::cellText(int row, int col) const
{
    const Live::Spot &s = m_rows.at(row);
    switch (col) {
    case Date: return s.time.toString("yyyy-MM-dd HH:mm");
    case Call: return s.txSign;
    case Frequency: return QString::number(s.frequency / 1e6, 'f', 6);
    case Snr: return s.snr > 0 ? "+" + QString::number(s.snr) : QString::number(s.snr);
    case Drift: return QString::number(s.drift);
    case TxLoc: return s.txLoc;
    case DBm: return QString::number(s.power);
    case Reporter: return s.rxSign;
    case RxLoc: return s.rxLoc;
    case Km: return QString::number(s.distance);
    case Miles: return QString::number(Live::kmToMiles(s.distance));
    case Az: return QString::number(s.azimuth);
    case Cw: return Live::cwRating(s.snr, s.power, m_compareDbm);
    case Ssb: return Live::ssbRating(s.snr, s.power, m_compareDbm);
    case Version: return s.version;
    default: return QString();
    }
}

QVariant LiveTableModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const int col = index.column();
    if (role == Qt::DisplayRole)
        return cellText(index.row(), col);
    if (role == Qt::TextAlignmentRole) {   // C#: SNR right, Drift and dBm centred
        if (col == Snr)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        if (col == Drift || col == DBm)
            return int(Qt::AlignCenter);
        return int(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::ToolTipRole && col == Version)
        return QString("Click to show only reports from this version");
    if (role == SortRole) {   // numbers sort as numbers (C# sorted the text)
        const Live::Spot &s = m_rows.at(index.row());
        switch (col) {
        case Date: return s.time;
        case Frequency: return s.frequency;
        case Snr: return s.snr;
        case Drift: return s.drift;
        case DBm: return s.power;
        case Km: case Miles: return s.distance;
        case Az: return s.azimuth;
        case Cw: case Ssb: return s.snr - s.power;
        default: return cellText(index.row(), col);
        }
    }
    return {};
}

QVariant LiveTableModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o == Qt::Vertical)
        return role == Qt::DisplayRole ? QVariant(QString()) : QVariant();
    if (role != Qt::DisplayRole || section < 0 || section >= ColumnCount)
        return {};
    if (section == Cw)
        return QString("CW @%1W").arg(m_watts);
    if (section == Ssb)
        return QString("SSB @%1W").arg(m_watts);
    return QString::fromLatin1(kHeadings[section]);
}
