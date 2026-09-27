#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "limitcheck.h"
#include "telemetry.h"
#include "udplink.h"

#include <QBarCategoryAxis>
#include <QBarSeries>
#include <QBarSet>
#include <QChart>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineSeries>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QVector>

#include <functional>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void draw();                 // построить вкладки по текущему пакету
    void onPacketReceived(const TelemetryPacket &packet, quint16 seq);
    void onFrameRejected(const QString &reason, const QHostAddress &from);
    void onStaleCheck();
    void updateStatusBar();

private:
    // Значение, которое показывает кольцевая диаграмма.
    struct DonutValue
    {
        double  percent; // заполнение кольца, 0..100
        QString text;    // подпись в центре
        QColor  color;
    };

    // Показание, которое умеет краснеть при выходе за границы нормы.
    struct ValueVisual
    {
        QLabel *label;
        QString baseStyle;    // шрифт и фон, без цвета текста
        QString parameterId;  // пусто — границы для этого показания не заданы
        QColor  nominalColor; // цвет, когда всё в норме
    };

    // parameterId связывает показание с таблицей границ в limitcheck.cpp.
    QGroupBox *makeTile(const QString &title, std::function<QString()> format,
                        const QString &parameterId = QString());
    QWidget   *makeDonut(const QString &title, std::function<DonutValue()> value,
                         const QString &parameterId = QString());
    void       registerValueLabel(QLabel *label, const QString &baseStyle,
                                  const QString &parameterId, const QColor &nominalColor);

    LimitState limitStateFor(const QString &id) const;
    void       updateLimitTable();
    void       updateTabColors();

    void buildEpsTab();
    void buildThermalTab();
    void buildAdcsTab();
    void buildObcTab();
    void buildCommTab();
    void buildGpsTab();

    void updateViews();
    void rescaleTemperatureAxis();
    void appendJournalRow(const QString &status, quint16 seq);
    void applyVisuals();
    void clearTabs();

    Ui::MainWindow *ui;

    // --- Канал приёма. Телеметрия приходит только оттуда.
    UdpTelemetryLink *link;
    QTimer           *staleTimer;
    TelemetryPacket   lastPacket{};
    bool              havePacket   = false;
    qint64            lastPacketMs = 0;
    bool              stale        = false;
    quint16           lastSeq      = 0;

    // --- Панель управления
    QSpinBox    *portSpin;
    QPushButton *button, *button2, *button3;
    QTableWidget *table;
    QTableWidget *limitView; // нарушения границ нормы
    QTabWidget   *tab;
    QLabel       *port;
    QGroupBox    *gb, *gb2, *gb3;
    QVBoxLayout  *mainLayout;
    QHBoxLayout  *hb;
    QVBoxLayout  *vb;
    QFormLayout  *fm, *fm1;

    // --- Обновление показаний без пересборки вкладок.
    QVector<std::function<void()>> valueUpdaters;
    QVector<ValueVisual>           valueVisuals;
    bool                           viewsBuilt = false;

    // --- Границы нормы: текущие значения параметров и их состояние.
    QHash<QString, double>     currentValues;
    QHash<QString, LimitState> currentStates;
    int warningCount  = 0;
    int criticalCount = 0;

    QFont tileTitleFont;
    QFont tileValueFont;

    // --- Графики
    QChart     *solarChart     = nullptr;
    QBarSeries *solarBarSeries = nullptr;
    QBarSet    *solarBarSet    = nullptr;
    QChart     *ctemp          = nullptr;
    QValueAxis *axisXt         = nullptr;
    QValueAxis *axisYt         = nullptr;
    QVector<QLineSeries *> solartemp;
    int index = 0;
};

#endif // MAINWINDOW_H
