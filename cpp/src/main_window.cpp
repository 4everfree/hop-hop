#include "main_window.h"
#include "platform.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QItemSelection>
#include <QPushButton>
#include <QScrollBar>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kRefreshMs = 2000;   // polling interval
constexpr int kWarnLevel = 6;      // above this the tray icon turns red

enum Column { ColType, ColPid, ColDir, ColTty, ColUptime, ColCpu, ColRam, ColCmd };

// Rows carry their position in the collected list here. Ties are broken by
// it, so rows that compare equal — every client of the same type, say — keep
// a fixed order instead of shuffling on each refresh.
constexpr int kOrderRole = Qt::UserRole + 1;

// Sorts on the raw value stashed in UserRole when there is one, so "512 MB"
// and "1:03" order by size and duration rather than alphabetically.
class SortItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem &other) const override
    {
        const int cmp = compare(other);
        if (cmp != 0)
            return cmp < 0;
        return data(kOrderRole).toInt() < other.data(kOrderRole).toInt();
    }

private:
    int compare(const QTableWidgetItem &other) const
    {
        const QVariant mine = data(Qt::UserRole);
        const QVariant theirs = other.data(Qt::UserRole);
        if (mine.isValid() && theirs.isValid()) {
            const double a = mine.toDouble();
            const double b = theirs.toDouble();
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        const QVariant da = data(Qt::DisplayRole);
        const QVariant db = other.data(Qt::DisplayRole);
        if (da.typeId() == QMetaType::LongLong && db.typeId() == QMetaType::LongLong) {
            const qlonglong a = da.toLongLong();
            const qlonglong b = db.toLongLong();
            return a < b ? -1 : (a > b ? 1 : 0);
        }
        return QString::compare(text(), other.text(), Qt::CaseInsensitive);
    }
};

// A rabbit-eared badge carrying the client count: the number has to stay
// readable at 22 px, so the ears sit above the body rather than around it.
QIcon makeTrayIcon(int total, const QString &accent)
{
    constexpr int size = 64;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor bg = total == 0 ? QColor("#6E6E6E")
                    : total > kWarnLevel ? QColor("#C0392B")
                                         : QColor(accent);
    p.setBrush(bg);
    p.setPen(Qt::NoPen);

    const struct { qreal cx, angle; } ears[] = {{22, -10}, {42, 10}};
    for (const auto &ear : ears) {
        p.save();
        p.translate(ear.cx, 15);
        p.rotate(ear.angle);
        p.drawEllipse(QPointF(0, 0), 5.5, 12);
        p.restore();
    }

    const QRectF body(3, 20, 58, 41);
    p.drawRoundedRect(body, 12, 12);

    p.setPen(QColor("#FFFFFF"));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(total < 10 ? 32 : 24);
    p.setFont(f);
    p.drawText(body, Qt::AlignCenter, QString::number(total));
    p.end();
    return QIcon(pm);
}

} // namespace

MainWindow::MainWindow(std::vector<Target> targets, QWidget *parent)
    : QMainWindow(parent)
{
    detector_.setTargets(std::move(targets));

    setWindowTitle(tr("hop-hop — running AI clients"));
    resize(1000, 420);

    summary_ = new QLabel(this);
    summary_->setTextFormat(Qt::RichText);
    onlyTty_ = new QCheckBox(tr("With a terminal only"), this);
    auto *refreshButton = new QPushButton(tr("Refresh"), this);

    auto *top = new QHBoxLayout;
    top->addWidget(summary_);
    top->addStretch(1);
    top->addWidget(onlyTty_);
    top->addWidget(refreshButton);

    table_ = new QTableWidget(0, 8, this);
    table_->setHorizontalHeaderLabels({tr("Type"), tr("PID"), tr("Directory"),
                                       tr("TTY"), tr("Uptime"), tr("CPU %"),
                                       tr("RAM"), tr("Command")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSortingEnabled(true);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    table_->setToolTip(tr("Double-click — switch to the client's window"));
    table_->horizontalHeader()->setSectionResizeMode(ColDir, QHeaderView::Stretch);

    auto *body = new QVBoxLayout;
    body->addLayout(top);
    body->addWidget(table_);
    auto *root = new QWidget(this);
    root->setLayout(body);
    setCentralWidget(root);

    tray_ = new QSystemTrayIcon(makeTrayIcon(0, "#D97757"), this);
    auto *trayMenu = new QMenu(this);
    auto *actShow = trayMenu->addAction(tr("Show window"));
    trayMenu->addSeparator();
    auto *actQuit = trayMenu->addAction(tr("Quit"));
    tray_->setContextMenu(trayMenu);
    tray_->show();

    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refresh);
    connect(onlyTty_, &QCheckBox::toggled, this, &MainWindow::refresh);
    connect(table_, &QTableWidget::doubleClicked, this, &MainWindow::activateSelected);
    connect(table_, &QTableWidget::customContextMenuRequested,
            this, &MainWindow::showContextMenu);
    connect(actShow, &QAction::triggered, this, &MainWindow::showAndRaise);
    connect(actQuit, &QAction::triggered, qApp, &QApplication::quit);
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger)
                    showAndRaise();
            });

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::refresh);
    timer_->start(kRefreshMs);
    refresh();
}

void MainWindow::refresh()
{
    clients_ = detector_.collect(onlyTty_->isChecked());

    QMap<QString, int> counts;
    for (const Target &t : detector_.targets())
        counts[t.kind] = 0;
    for (const Client &c : clients_)
        counts[c.kind] += 1;

    QStringList parts;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        QString color = QStringLiteral("#888888");
        for (const Target &t : detector_.targets()) {
            if (t.kind == it.key()) {
                color = t.color;
                break;
            }
        }
        parts << QStringLiteral("<span style='color:%1'><b>%2</b>: %3</span>")
                     .arg(color, it.key()).arg(it.value());
    }
    const int total = static_cast<int>(clients_.size());
    summary_->setText(parts.join(QStringLiteral("&nbsp;&nbsp;&nbsp;"))
                      + QStringLiteral("&nbsp;&nbsp;&nbsp;total: <b>%1</b>").arg(total));

    const QString accent = detector_.targets().empty() ? QStringLiteral("#D97757")
                                                       : detector_.targets().front().color;
    tray_->setIcon(makeTrayIcon(total, accent));
    QStringList tip;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        tip << QStringLiteral("%1: %2").arg(it.key()).arg(it.value());
    tray_->setToolTip(tip.join(QStringLiteral("  ")));

    updateTable();
}

// Cells are updated in place whenever the set of clients is unchanged. A full
// teardown every two seconds is what used to throw away the current cell and
// the scroll position, which reads as the view jumping under your hands.
void MainWindow::updateTable()
{
    // remember where the user is looking before touching anything
    const int scrollV = table_->verticalScrollBar()->value();
    const int scrollH = table_->horizontalScrollBar()->value();
    const int currentColumn = qMax(0, table_->currentColumn());
    qint64 currentPid = -1;
    if (auto *item = table_->item(table_->currentRow(), ColPid))
        currentPid = item->data(Qt::DisplayRole).toLongLong();
    QSet<qint64> selected;
    for (const QModelIndex &idx : table_->selectionModel()->selectedRows()) {
        if (auto *item = table_->item(idx.row(), ColPid))
            selected.insert(item->data(Qt::DisplayRole).toLongLong());
    }

    QHash<qint64, int> rowOfPid;
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (auto *item = table_->item(row, ColPid))
            rowOfPid.insert(item->data(Qt::DisplayRole).toLongLong(), row);
    }

    bool sameClients = rowOfPid.size() == static_cast<int>(clients_.size());
    if (sameClients) {
        for (const Client &c : clients_) {
            if (!rowOfPid.contains(c.pid)) {
                sameClients = false;
                break;
            }
        }
    }

    table_->setSortingEnabled(false);
    if (sameClients) {
        for (int i = 0; i < static_cast<int>(clients_.size()); ++i)
            fillRow(rowOfPid.value(clients_[i].pid), clients_[i], i);
    } else {
        table_->setRowCount(static_cast<int>(clients_.size()));
        for (int i = 0; i < static_cast<int>(clients_.size()); ++i)
            fillRow(i, clients_[i], i);
    }
    table_->setSortingEnabled(true);   // re-sorts by the current indicator

    // only fight the column widths when the row set actually changed;
    // otherwise a widening CPU or RAM value shifts the whole table sideways
    if (!sameClients) {
        table_->resizeColumnsToContents();
        table_->horizontalHeader()->setSectionResizeMode(ColDir, QHeaderView::Stretch);
    }

    // sorting moved the rows around, so look the pids up again
    rowOfPid.clear();
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (auto *item = table_->item(row, ColPid))
            rowOfPid.insert(item->data(Qt::DisplayRole).toLongLong(), row);
    }

    if (currentPid > 0 && rowOfPid.contains(currentPid)) {
        table_->setCurrentCell(rowOfPid.value(currentPid), currentColumn,
                               QItemSelectionModel::NoUpdate);
    }
    if (!selected.isEmpty()) {
        QItemSelection restored;
        for (qint64 pid : selected) {
            if (!rowOfPid.contains(pid))
                continue;
            const int row = rowOfPid.value(pid);
            restored.select(table_->model()->index(row, 0),
                            table_->model()->index(row, table_->columnCount() - 1));
        }
        table_->selectionModel()->select(restored, QItemSelectionModel::ClearAndSelect);
    }

    table_->verticalScrollBar()->setValue(scrollV);
    table_->horizontalScrollBar()->setValue(scrollH);
}

// Writes one client into an existing row, creating cells only when missing.
void MainWindow::fillRow(int row, const Client &c, int order)
{
    if (row < 0 || row >= table_->rowCount())
        return;

    const auto cell = [this, row, order](int column) {
        auto *item = table_->item(row, column);
        if (!item) {
            item = new SortItem;
            table_->setItem(row, column, item);
        }
        item->setData(kOrderRole, order);
        return item;
    };
    const auto setText = [](QTableWidgetItem *item, const QString &text) {
        if (item->text() != text)
            item->setText(text);
    };

    auto *type = cell(ColType);
    setText(type, c.kind);
    if (!type->font().bold()) {
        QFont bold = type->font();
        bold.setBold(true);
        type->setFont(bold);
    }
    for (const Target &t : detector_.targets()) {
        if (t.kind == c.kind) {
            const QColor color(t.color);
            if (type->foreground().color() != color)
                type->setForeground(color);
            break;
        }
    }

    auto *pid = cell(ColPid);
    if (pid->data(Qt::DisplayRole).toLongLong() != c.pid) {
        pid->setData(Qt::DisplayRole, static_cast<qlonglong>(c.pid));
        pid->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    setText(cell(ColDir), c.cwd);
    setText(cell(ColTty), c.tty.isEmpty() ? QStringLiteral("—") : c.tty);

    auto *uptime = cell(ColUptime);
    setText(uptime, detect::formatUptime(c.uptime));
    uptime->setData(Qt::UserRole, c.uptime);

    auto *cpu = cell(ColCpu);
    setText(cpu, QString::number(c.cpu, 'f', 1));
    cpu->setData(Qt::UserRole, c.cpu);
    cpu->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto *ram = cell(ColRam);
    setText(ram, QStringLiteral("%1 MB").arg(c.rss / 1048576));
    ram->setData(Qt::UserRole, static_cast<double>(c.rss));
    ram->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    setText(cell(ColCmd), c.cmd);
}

const Client *MainWindow::currentClient() const
{
    const int row = table_->currentRow();
    if (row < 0)
        return nullptr;
    auto *item = table_->item(row, ColPid);
    if (!item)
        return nullptr;
    const qint64 pid = item->data(Qt::DisplayRole).toLongLong();
    for (const Client &c : clients_) {
        if (c.pid == pid)
            return &c;
    }
    return nullptr;
}

void MainWindow::activateSelected()
{
    const Client *c = currentClient();
    if (!c)
        return;
    // the window belongs to an ancestor — a terminal, an IDE, a browser
    platform::activateWindow(detect::ancestorPids(c->pid, detector_.lastSnapshot()));
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    const Client *c = currentClient();
    if (!c)
        return;
    const qint64 pid = c->pid;
    const QString cwd = c->cwd;
    const QString cmd = c->cmd;
    const QString kind = c->kind;

    QMenu menu(this);
    QAction *actWindow = menu.addAction(tr("Switch to window"));
    menu.addSeparator();
    QAction *actDir = menu.addAction(tr("Open directory"));
    QAction *actPid = menu.addAction(tr("Copy PID"));
    QAction *actCmd = menu.addAction(tr("Copy command"));
    menu.addSeparator();
    QAction *actTerm = menu.addAction(tr("Terminate (SIGTERM)"));
    QAction *actKill = menu.addAction(tr("Kill (SIGKILL)"));
    actDir->setEnabled(!cwd.isEmpty());

    QAction *chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;

    if (chosen == actWindow) {
        activateSelected();
    } else if (chosen == actDir) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(cwd));
    } else if (chosen == actPid) {
        QApplication::clipboard()->setText(QString::number(pid));
    } else if (chosen == actCmd) {
        QApplication::clipboard()->setText(cmd);
    } else if (chosen == actTerm || chosen == actKill) {
        const bool force = (chosen == actKill);
        const auto answer = QMessageBox::question(
            this, tr("Confirm"),
            tr("Send %1 to process %2 (%3)?\n%4")
                .arg(force ? tr("SIGKILL") : tr("SIGTERM"))
                .arg(pid).arg(kind, cwd));
        if (answer == QMessageBox::Yes) {
            const QString error = platform::terminateProcess(pid, force);
            if (!error.isEmpty())
                QMessageBox::warning(this, tr("Failed"), error);
            refresh();
        }
    }
}

void MainWindow::showAndRaise()
{
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (tray_ && tray_->isVisible()) {
        event->ignore();
        hide();
    } else {
        event->accept();
    }
}
