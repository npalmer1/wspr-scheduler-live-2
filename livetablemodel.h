#pragma once
// WSPR Scheduler Live's grid (C#: LiveForm.dataGridView1, 15 columns). Like RxTableModel: rows keep the
// raw values, numbers sort as numbers, and changing the CW/SSB comparison power updates every row.
#include "livecore.h"

#include <QAbstractTableModel>
#include <QVector>

class LiveTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Date, Call, Frequency, Snr, Drift, TxLoc, DBm, Reporter, RxLoc, Km, Miles, Az, Cw, Ssb, Version, ColumnCount };
    static constexpr int SortRole = Qt::UserRole + 1;

    explicit LiveTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;

    void setRows(const QVector<Live::Spot> &rows);
    const Live::Spot &spot(int row) const { return m_rows.at(row); }
    void setCompareWatts(int watts);
    int compareWatts() const { return m_watts; }
    QString cellText(int row, int col) const;

private:
    QVector<Live::Spot> m_rows;
    int m_watts = 100;
    int m_compareDbm = 50;
};
