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

private slots:
    void refresh();
    void activateSelected();
    void showContextMenu(const QPoint &pos);

private:
    const Client *currentClient() const;
    void updateTable();
    void fillRow(int row, const Client &client, int order);

    detect::Detector detector_;
    std::vector<Client> clients_;

    QLabel *summary_ = nullptr;
    QCheckBox *onlyTty_ = nullptr;
    QTableWidget *table_ = nullptr;
    QSystemTrayIcon *tray_ = nullptr;
    QTimer *timer_ = nullptr;
};
