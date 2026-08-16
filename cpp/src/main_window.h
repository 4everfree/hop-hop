#pragma once

#include "client.h"
#include "detect.h"

#include <QMainWindow>
#include <vector>

class QCheckBox;
class QLabel;
class QSystemTrayIcon;
class QTableWidget;
class QTimer;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(std::vector<Target> targets, QWidget *parent = nullptr);

    void showAndRaise();

protected:
    void closeEvent(QCloseEvent *event) override;   // hides to the tray
    bool eventFilter(QObject *watched, QEvent *event) override;   // table resizes

private slots:
    void refresh();
    void activateSelected();
    void showContextMenu(const QPoint &pos);
    void onSectionResized(int column, int oldWidth, int newWidth);

private:
    const Client *currentClient() const;
    void updateTable();
    void sizeColumnsOnce();
    void fitDirectoryColumn();
    void fillRow(int row, const Client &client, int order);

    detect::Detector detector_;
    std::vector<Client> clients_;

    QLabel *summary_ = nullptr;
    QCheckBox *onlyTty_ = nullptr;
    QTableWidget *table_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    QTimer *timer_ = nullptr;
    bool columnsSized_ = false;      // the one-off measurement has run
    bool dirPinned_ = false;         // the user dragged Directory themselves
    bool adjustingColumns_ = false;  // we are the ones changing widths
};
