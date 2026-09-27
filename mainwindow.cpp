#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include "protocol.h"
#include "telemetry.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <QHeaderView>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QPieSeries>
#include <QPieSlice>
#include <QStackedLayout>
#include <QStatusBar>
#include <QChartView>
#include <cmath>

namespace {

// Через сколько секунд молчания показания считаются устаревшими.
constexpr int    kStaleAfterMs   = 5000;
constexpr int    kStaleCheckMs   = 500;
constexpr int    kJournalMaxRows = 500;
constexpr int    kTrendPoints    = 30; // сколько точек держим на графике температур
constexpr double kMemoryTotalKb  = 1024.0;

const QStringList &tabNames()
{
    static const QStringList names{
        QStringLiteral("EPS"),
        QStringLiteral("Терморегулирование"),
        QStringLiteral("ADCS"),
        QStringLiteral("OBC"),
        QStringLiteral("COMM"),
        QStringLiteral("GPS"),
    };
    return names;
}

// Базовые стили — без цвета текста: цвет добавляется по состоянию границ.
const char *kTileBaseStyle  = "";
const char *kDonutBaseStyle = "font-size: 40px; font-weight: bold; background: transparent;";

const QColor kStaleColor(0x8A, 0x8A, 0x8A);

// Цвет — это представление, поэтому он живёт здесь, а не в модуле границ:
// limitcheck.cpp остаётся чистой логикой без зависимости от QtGui.
QColor stateColor(LimitState state)
{
    switch (state) {
    case LimitState::Critical: return QColor(0xE0, 0x52, 0x52); // красный
    case LimitState::Warning:  return QColor(0xE8, 0xA3, 0x3D); // янтарный
    case LimitState::Nominal:  return QColor(0x1D, 0x9E, 0x75); // зелёный
    case LimitState::NoLimits: break;
    }
    return kStaleColor;
}

// Вкладки идут в том же порядке, что префиксы идентификаторов параметров, —
// так вкладка узнаёт о нарушениях внутри себя без отдельного учёта.
const char *kTabPrefixes[] = {"eps.", "thermal.", "adcs.", "obc.", "comm.", "gps."};

QString fmt(double value, int precision = 2)
{
    return QString::number(value, 'f', precision);
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("Телеметрия МКА"));

    const QString iconPath = QCoreApplication::applicationDirPath() + "/cub3.png";
    if (QFile::exists(iconPath))
        setWindowIcon(QIcon(iconPath));

    tileTitleFont.setPointSize(14);
    tileValueFont.setPointSize(16);

    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    // --- Канал приёма. Ничего не генерируем: пакеты приходят снаружи.
    link = new UdpTelemetryLink(this);
    connect(link, &UdpTelemetryLink::packetReceived, this, &MainWindow::onPacketReceived);
    connect(link, &UdpTelemetryLink::frameRejected, this, &MainWindow::onFrameRejected);
    connect(link, &UdpTelemetryLink::statsChanged, this, &MainWindow::updateStatusBar);

    staleTimer = new QTimer(this);
    staleTimer->setInterval(kStaleCheckMs);
    connect(staleTimer, &QTimer::timeout, this, &MainWindow::onStaleCheck);

    button = new QPushButton(QStringLiteral("Подключиться"));
    connect(button, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    button2 = new QPushButton(QStringLiteral("Отключиться"));
    connect(button2, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);
    button2->setEnabled(false);
    button3 = new QPushButton(QStringLiteral("Показать данные"));
    connect(button3, &QPushButton::clicked, this, &MainWindow::draw);

    table = new QTableWidget(0, 4);
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Время"), QStringLiteral("№ кадра"),
         QStringLiteral("Счётчик"), QStringLiteral("Статус")});
    table->horizontalHeader()->setStretchLastSection(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    portSpin = new QSpinBox();
    portSpin->setRange(1024, 65535);
    portSpin->setValue(protocol::kDefaultUdpPort);
    portSpin->setToolTip(QStringLiteral("UDP-порт, на котором ждём кадры телеметрии"));

    tab = new QTabWidget(central);
    for (const QString &name : tabNames()) {
        QLabel *label = new QLabel(QStringLiteral("Нет данных"), tab);
        label->setAlignment(Qt::AlignCenter);
        tab->addTab(label, name);
    }
    tab->resize(800, 500);

    mainLayout = new QVBoxLayout(central);
    hb = new QHBoxLayout();
    mainLayout->addWidget(tab, 2);

    gb = new QGroupBox(QStringLiteral("Канал"));
    vb = new QVBoxLayout(gb);
    fm = new QFormLayout();
    port = new QLabel(QStringLiteral("UDP-порт"));
    fm->addWidget(port);
    fm->addWidget(portSpin);
    fm->addWidget(button);
    fm->addWidget(button2);
    fm->addWidget(button3);
    vb->addLayout(fm);
    hb->addWidget(gb);

    gb2 = new QGroupBox(QStringLiteral("Журнал пакетов"));
    fm1 = new QFormLayout(gb2);
    fm1->addWidget(table);
    hb->addWidget(gb2, 2);

    // Отдельный список нарушений: в журнале пакетов они утонули бы, там
    // по строке на каждый принятый кадр.
    limitView = new QTableWidget(0, 4);
    limitView->setHorizontalHeaderLabels(
        {QStringLiteral("Параметр"), QStringLiteral("Значение"),
         QStringLiteral("Норма"), QStringLiteral("Состояние")});
    limitView->horizontalHeader()->setStretchLastSection(true);
    limitView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    limitView->verticalHeader()->setVisible(false);

    gb3 = new QGroupBox(QStringLiteral("Границы нормы"));
    QVBoxLayout *limitLayout = new QVBoxLayout(gb3);
    limitLayout->addWidget(limitView);
    hb->addWidget(gb3, 3);

    mainLayout->addLayout(hb);

    statusBar()->showMessage(QStringLiteral("Канал закрыт"));
}

MainWindow::~MainWindow()
{
    delete ui;
}

// ---------------------------------------------------------------------------
//  Управление каналом
// ---------------------------------------------------------------------------

void MainWindow::onConnectClicked()
{
    const quint16 wanted = static_cast<quint16>(portSpin->value());

    QString error;
    if (!link->open(wanted, &error)) {
        QMessageBox::warning(this, QStringLiteral("Не удалось открыть порт"),
                             QStringLiteral("UDP-порт %1: %2").arg(wanted).arg(error));
        return;
    }

    link->resetStats();

    // Сырой журнал: пишем всё принятое до разбора, чтобы пролёт можно было
    // переиграть позже (sim/sat_sim.py --replay).
    const QString logDir  = QCoreApplication::applicationDirPath() + "/logs";
    const QString logName = QStringLiteral("telemetry_%1.bin")
                                .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    QString logError;
    if (!link->startLog(logDir + "/" + logName, &logError)) {
        statusBar()->showMessage(
            QStringLiteral("Журнал кадров не ведётся: %1").arg(logError), 5000);
    }

    havePacket = false;
    stale      = false;
    portSpin->setEnabled(false);
    button->setEnabled(false);
    button2->setEnabled(true);
    staleTimer->start();
    updateStatusBar();
}

void MainWindow::onDisconnectClicked()
{
    link->close();
    staleTimer->stop();

    table->setRowCount(0); // clearContents() оставил бы пустые строки
    havePacket = false;
    stale      = false;
    lastPacket = TelemetryPacket{};

    // Нарушения снимаются вместе с каналом: они относились к данным, которых
    // больше нет.
    currentValues.clear();
    currentStates.clear();
    warningCount  = 0;
    criticalCount = 0;
    limitView->setRowCount(0);

    portSpin->setEnabled(true);
    button->setEnabled(true);
    button2->setEnabled(false);

    if (viewsBuilt) {
        updateViews();
        applyVisuals();
    }
    statusBar()->showMessage(QStringLiteral("Канал закрыт"));
}

// ---------------------------------------------------------------------------
//  Приём
// ---------------------------------------------------------------------------

void MainWindow::onPacketReceived(const TelemetryPacket &packet, quint16 seq)
{
    const bool wasSilent = !havePacket || stale;

    lastPacket   = packet;
    lastSeq      = seq;
    havePacket   = true;
    lastPacketMs = QDateTime::currentMSecsSinceEpoch();

    if (wasSilent) {
        // AOS — начало связи. В реальных пультах это отдельное событие.
        appendJournalRow(QStringLiteral("AOS — связь установлена"), seq);
        stale = false;
    }

    // Проверка границ идёт до отрисовки: подсветка должна соответствовать
    // тому же кадру, что и сами значения.
    currentValues.clear();
    currentStates.clear();
    for (const ParameterSample &sample : limits::sample(packet)) {
        currentValues.insert(sample.id, sample.value);
        const LimitState state = limits::check(sample.id, sample.value);
        if (state != LimitState::NoLimits)
            currentStates.insert(sample.id, state);
    }
    updateLimitTable();

    appendJournalRow(QStringLiteral("OK"), seq);
    updateViews();
    applyVisuals();
    updateStatusBar();
}

void MainWindow::onFrameRejected(const QString &reason, const QHostAddress &from)
{
    Q_UNUSED(from)
    // Битый кадр тоже событие канала: он попадает в журнал, но показания
    // не обновляет — данным из него верить нельзя.
    appendJournalRow(reason, 0);
}

void MainWindow::onStaleCheck()
{
    if (!havePacket || stale)
        return;

    const qint64 age = QDateTime::currentMSecsSinceEpoch() - lastPacketMs;
    if (age > kStaleAfterMs) {
        stale = true;
        applyVisuals();
        appendJournalRow(QStringLiteral("LOS — связь потеряна"), lastSeq);
        updateStatusBar();
    }
}

void MainWindow::appendJournalRow(const QString &status, quint16 seq)
{
    // Журнал не растим бесконечно: за длинный сеанс это съело бы память.
    while (table->rowCount() >= kJournalMaxRows)
        table->removeRow(0);

    const int row = table->rowCount();
    table->insertRow(row);
    table->setItem(row, 0,
                   new QTableWidgetItem(QTime::currentTime().toString("hh:mm:ss.zzz")));
    table->setItem(row, 1, new QTableWidgetItem(QString::number(link->stats().received)));
    table->setItem(row, 2, new QTableWidgetItem(QString::number(seq)));

    QTableWidgetItem *statusItem = new QTableWidgetItem(status);
    if (status != QStringLiteral("OK"))
        statusItem->setForeground(QColor(200, 60, 60));
    table->setItem(row, 3, statusItem);
    table->scrollToBottom();
}

void MainWindow::updateStatusBar()
{
    if (!link->isOpen()) {
        statusBar()->showMessage(QStringLiteral("Канал закрыт"));
        return;
    }

    const LinkStats &s = link->stats();
    QString message = QStringLiteral("Приём UDP :%1").arg(link->port());

    if (!havePacket) {
        message += QStringLiteral("  ·  ожидание кадров");
    } else {
        const double ageSec = (QDateTime::currentMSecsSinceEpoch() - lastPacketMs) / 1000.0;
        message += stale ? QStringLiteral("  ·  НЕТ СВЯЗИ")
                         : QStringLiteral("  ·  связь есть");
        message += QStringLiteral("  ·  последний кадр %1 с назад").arg(fmt(ageSec, 1));
    }

    message += QStringLiteral("  ·  принято %1  ·  CRC-ошибок %2  ·  мусор %3  ·  потеряно %4")
                   .arg(s.received).arg(s.crcErrors).arg(s.malformed).arg(s.lost);

    if (havePacket && !stale) {
        if (criticalCount > 0)
            message += QStringLiteral("  ·  АВАРИЙ %1").arg(criticalCount);
        if (warningCount > 0)
            message += QStringLiteral("  ·  предупреждений %1").arg(warningCount);
        if (criticalCount == 0 && warningCount == 0)
            message += QStringLiteral("  ·  все параметры в норме");
    }

    if (!link->logPath().isEmpty())
        message += QStringLiteral("  ·  журнал %1 кадр.").arg(link->loggedFrames());

    statusBar()->showMessage(message);
}

// ---------------------------------------------------------------------------
//  Построение показаний
// ---------------------------------------------------------------------------

void MainWindow::registerValueLabel(QLabel *label, const QString &baseStyle,
                                    const QString &parameterId, const QColor &nominalColor)
{
    valueVisuals.append(ValueVisual{label, baseStyle, parameterId, nominalColor});
}

LimitState MainWindow::limitStateFor(const QString &id) const
{
    return currentStates.value(id, LimitState::NoLimits);
}

QGroupBox *MainWindow::makeTile(const QString &title, std::function<QString()> format,
                                const QString &parameterId)
{
    QGroupBox *box = new QGroupBox();
    QVBoxLayout *layout = new QVBoxLayout(box);

    QLabel *titleLabel = new QLabel(title);
    titleLabel->setFont(tileTitleFont);
    layout->addWidget(titleLabel);

    QLabel *valueLabel = new QLabel(format());
    valueLabel->setFont(tileValueFont);
    layout->addWidget(valueLabel);

    // В норме плитка красится цветом темы, а не жёстко чёрным: иначе на
    // тёмной теме текст исчезнет.
    registerValueLabel(valueLabel, QString::fromLatin1(kTileBaseStyle), parameterId,
                       palette().color(QPalette::WindowText));
    valueUpdaters.append([valueLabel, format]() { valueLabel->setText(format()); });
    return box;
}

QWidget *MainWindow::makeDonut(const QString &title, std::function<DonutValue()> value,
                               const QString &parameterId)
{
    const DonutValue initial = value();

    QPieSeries *series = new QPieSeries();
    series->setHoleSize(0.65);

    QPieSlice *filled = series->append(QString(), qMax(0.001, initial.percent));
    filled->setColor(initial.color);
    filled->setBorderWidth(0);
    filled->setLabelVisible(false);

    QPieSlice *rest = series->append(QString(), qMax(0.001, 100.0 - initial.percent));
    rest->setColor(QColor(60, 60, 60));
    rest->setBorderWidth(0);
    rest->setLabelVisible(false);

    QFont titleFont;
    titleFont.setPointSize(16);

    QChart *chart = new QChart();
    chart->addSeries(series);
    chart->legend()->setVisible(false);
    chart->setTitle(title);
    chart->setTitleFont(titleFont);
    chart->setTitleBrush(QColor(255, 255, 255));
    chart->setBackgroundVisible(false);

    QChartView *chartView = new QChartView(chart);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setStyleSheet("background: transparent;");

    QLabel *centerLabel = new QLabel(initial.text);
    centerLabel->setAlignment(Qt::AlignCenter);
    registerValueLabel(centerLabel, QString::fromLatin1(kDonutBaseStyle), parameterId,
                       QColor(Qt::white));

    QWidget *container = new QWidget();
    QStackedLayout *stack = new QStackedLayout(container);
    stack->setStackingMode(QStackedLayout::StackAll);
    stack->addWidget(chartView);
    stack->addWidget(centerLabel);

    valueUpdaters.append([filled, rest, centerLabel, value]() {
        const DonutValue v = value();
        // Доли всегда в пределах 0..100: иначе отрицательный сектор ломает кольцо.
        const double percent = qBound(0.0, v.percent, 100.0);
        filled->setValue(qMax(0.001, percent));
        rest->setValue(qMax(0.001, 100.0 - percent));
        filled->setColor(v.color);
        centerLabel->setText(v.text);
    });

    return container;
}

void MainWindow::clearTabs()
{
    // Сначала забываем указатели: дальше владеющие ими виджеты будут удалены.
    valueUpdaters.clear();
    valueVisuals.clear();
    solartemp.clear();
    solarChart     = nullptr;
    solarBarSeries = nullptr;
    solarBarSet    = nullptr;
    ctemp          = nullptr;
    axisXt         = nullptr;
    axisYt         = nullptr;
    index          = 0;

    // QTabWidget::removeTab() страницу не удаляет — без delete виджеты
    // остались бы в памяти после каждого нажатия «Показать данные».
    while (tab->count() > 0) {
        QWidget *page = tab->widget(0);
        tab->removeTab(0);
        delete page;
    }
}

void MainWindow::draw()
{
    clearTabs();

    buildEpsTab();
    buildThermalTab();
    buildAdcsTab();
    buildObcTab();
    buildCommTab();
    buildGpsTab();

    viewsBuilt = true;
    updateViews();
    applyVisuals();
}

void MainWindow::buildEpsTab()
{
    QGroupBox *page = new QGroupBox();
    QVBoxLayout *column = new QVBoxLayout(page);
    QHBoxLayout *donuts = new QHBoxLayout();
    QHBoxLayout *tiles  = new QHBoxLayout();

    // Цвет кольца берётся из состояния границ, а не из порогов, вписанных
    // в этот код: пороги живут в таблице в limitcheck.cpp.
    donuts->addWidget(makeDonut(QStringLiteral("SOC батареи"), [this]() {
        const double soc = lastPacket.eps.soc_percent;
        return DonutValue{soc, QString::number(lastPacket.eps.soc_percent) + "%",
                          stateColor(limitStateFor(QStringLiteral("eps.soc_percent")))};
    }, QStringLiteral("eps.soc_percent")));

    donuts->addWidget(makeDonut(QStringLiteral("Ток батареи"), [this]() {
        // Ток бывает отрицательным — это разряд. Кольцо показывает модуль
        // тока по шкале до 1 А, а цвет — состояние границ.
        const double current = lastPacket.eps.battery_current;
        const double percent = qBound(0.0, std::abs(current) * 100.0, 100.0);
        const LimitState state = limitStateFor(QStringLiteral("eps.battery_current"));
        // В норме направление тока важнее: синий — заряд, голубой — разряд.
        QColor color = stateColor(state);
        if (state == LimitState::Nominal)
            color = current < 0.0 ? QColor(90, 160, 200) : QColor(0, 191, 255);
        const QString sign = current < 0.0 ? QString() : QStringLiteral("+");
        return DonutValue{percent, sign + fmt(current) + " A", color};
    }, QStringLiteral("eps.battery_current")));

    tiles->addWidget(makeTile(QStringLiteral("Шина 3.3В"),
                              [this]() { return fmt(lastPacket.eps.bus_voltage[0]) + " В"; },
                              QStringLiteral("eps.bus_voltage[0]")));
    tiles->addWidget(makeTile(QStringLiteral("Шина 5В"),
                              [this]() { return fmt(lastPacket.eps.bus_voltage[1]) + " В"; },
                              QStringLiteral("eps.bus_voltage[1]")));
    tiles->addWidget(makeTile(QStringLiteral("Шина 8.4В"),
                              [this]() { return fmt(lastPacket.eps.bus_voltage[2]) + " В"; },
                              QStringLiteral("eps.bus_voltage[2]")));
    tiles->addWidget(makeTile(QStringLiteral("Напряжение батареи"),
                              [this]() { return fmt(lastPacket.eps.battery_voltage) + " В"; },
                              QStringLiteral("eps.battery_voltage")));

    solarChart = new QChart();
    solarChart->setTitle(QStringLiteral("Мощность солнечных панелей"));
    solarBarSet = new QBarSet(QStringLiteral("Мощность, Вт"));
    for (int i = 0; i < 12; ++i)
        *solarBarSet << lastPacket.eps.solar_power[i];

    solarBarSeries = new QBarSeries();
    solarBarSeries->append(solarBarSet);
    solarChart->addSeries(solarBarSeries);

    QStringList categories;
    for (int i = 1; i <= 12; ++i)
        categories << QStringLiteral("П%1").arg(i);

    QBarCategoryAxis *axisX = new QBarCategoryAxis();
    axisX->append(categories);
    solarChart->addAxis(axisX, Qt::AlignBottom);
    solarBarSeries->attachAxis(axisX);

    QValueAxis *axisY = new QValueAxis();
    axisY->setRange(0, 7);
    solarChart->addAxis(axisY, Qt::AlignLeft);
    solarBarSeries->attachAxis(axisY);
    solarChart->legend()->setVisible(false);

    QChartView *chartView = new QChartView(solarChart);
    chartView->setRenderHint(QPainter::Antialiasing);

    valueUpdaters.append([this]() {
        if (!solarBarSet)
            return;
        for (int i = 0; i < 12; ++i)
            solarBarSet->replace(i, lastPacket.eps.solar_power[i]);
    });

    column->addLayout(donuts);
    column->addLayout(tiles);
    column->addWidget(chartView);
    tab->addTab(page, tabNames()[0]);
}

void MainWindow::buildThermalTab()
{
    QGroupBox *page = new QGroupBox();
    QVBoxLayout *column = new QVBoxLayout(page);
    QHBoxLayout *tiles  = new QHBoxLayout();

    tiles->addWidget(makeTile(QStringLiteral("Температура CPU"),
                              [this]() { return fmt(lastPacket.thermal.temp_cpu) + " °C"; },
                              QStringLiteral("thermal.temp_cpu")));
    tiles->addWidget(makeTile(QStringLiteral("Температура батареи"),
                              [this]() { return fmt(lastPacket.thermal.temp_battery) + " °C"; },
                              QStringLiteral("thermal.temp_battery")));
    tiles->addWidget(makeTile(QStringLiteral("Температура антенны"),
                              [this]() { return fmt(lastPacket.thermal.temp_antenna) + " °C"; },
                              QStringLiteral("thermal.temp_antenna")));
    tiles->addWidget(makeTile(QStringLiteral("Температура внутри корпуса"),
                              [this]() { return fmt(lastPacket.thermal.temp_in) + " °C"; },
                              QStringLiteral("thermal.temp_in")));
    // Самая горячая панель из двенадцати: ограничивать каждую по отдельности
    // незачем, важно, что перегрелась хоть одна.
    tiles->addWidget(makeTile(QStringLiteral("Панели, макс."), [this]() {
        double hottest = lastPacket.thermal.temp_solar[0];
        for (int i = 1; i < 12; ++i)
            hottest = qMax(hottest, double(lastPacket.thermal.temp_solar[i]));
        return fmt(hottest, 1) + " °C";
    }, QStringLiteral("thermal.temp_solar_max")));
    column->addLayout(tiles);

    ctemp = new QChart();
    ctemp->setTitle(QStringLiteral("Температура солнечных панелей"));

    axisXt = new QValueAxis();
    axisXt->setTitleText(QStringLiteral("Принятые кадры"));
    axisXt->setRange(0, 20);
    ctemp->addAxis(axisXt, Qt::AlignBottom);

    axisYt = new QValueAxis();
    axisYt->setTitleText(QStringLiteral("Температура, °C"));
    axisYt->setRange(0, 50);
    ctemp->addAxis(axisYt, Qt::AlignLeft);

    for (int i = 0; i < 12; ++i) {
        QLineSeries *series = new QLineSeries();
        series->setName(QStringLiteral("Панель %1").arg(i + 1));
        ctemp->addSeries(series);
        series->attachAxis(axisXt);
        series->attachAxis(axisYt);
        solartemp.append(series);
    }
    ctemp->legend()->setVisible(true);

    QChartView *chartView = new QChartView(ctemp);
    chartView->setRenderHint(QPainter::Antialiasing);
    column->addWidget(chartView);

    tab->addTab(page, tabNames()[1]);
}

void MainWindow::buildAdcsTab()
{
    QGroupBox *page = new QGroupBox();
    QVBoxLayout *column = new QVBoxLayout(page);
    QHBoxLayout *row1 = new QHBoxLayout();
    QHBoxLayout *row2 = new QHBoxLayout();

    // Границы проверяются по максимальной из трёх осей — параметр
    // adcs.gyro_max в таблице.
    row1->addWidget(makeTile(QStringLiteral("Гироскоп X/Y/Z"), [this]() {
        const float *g = lastPacket.adcs.gyro;
        return fmt(g[0], 3) + " / " + fmt(g[1], 3) + " / " + fmt(g[2], 3) + " °/с";
    }, QStringLiteral("adcs.gyro_max")));
    row1->addWidget(makeTile(QStringLiteral("Магнитометр X/Y/Z"), [this]() {
        const float *m = lastPacket.adcs.magnetometer;
        return fmt(m[0]) + " / " + fmt(m[1]) + " / " + fmt(m[2]) + " µT";
    }));
    // Норма кватерниона обязана быть единицей — это проверка качества самих
    // данных, а не состояния борта.
    row1->addWidget(makeTile(QStringLiteral("Кватернион"), [this]() {
        const float *q = lastPacket.adcs.quaternion;
        return fmt(q[0], 3) + " + " + fmt(q[1], 3) + "i + " + fmt(q[2], 3) + "j + "
               + fmt(q[3], 3) + "k";
    }, QStringLiteral("adcs.quaternion_norm")));

    row2->addWidget(makeTile(QStringLiteral("Скорость маховиков"), [this]() {
        const float *w = lastPacket.adcs.reaction_wheel_speed;
        return fmt(w[0], 0) + " / " + fmt(w[1], 0) + " / " + fmt(w[2], 0) + " об/мин";
    }, QStringLiteral("adcs.wheel_max")));
    row2->addWidget(makeTile(QStringLiteral("Датчики Солнца"), [this]() {
        QStringList parts;
        for (int i = 0; i < 6; ++i)
            parts << fmt(lastPacket.adcs.sun_sensor[i], 2);
        return parts.join(" / ");
    }));

    column->addLayout(row1);
    column->addLayout(row2);
    column->addStretch(1);
    tab->addTab(page, tabNames()[2]);
}

void MainWindow::buildObcTab()
{
    QGroupBox *page = new QGroupBox();
    QVBoxLayout *column = new QVBoxLayout(page);
    QHBoxLayout *donuts = new QHBoxLayout();
    QHBoxLayout *tiles  = new QHBoxLayout();

    donuts->addWidget(makeDonut(QStringLiteral("Загрузка CPU"), [this]() {
        const double load = lastPacket.obc.cpu_load_percent;
        const LimitState state = limitStateFor(QStringLiteral("obc.cpu_load_percent"));
        const QColor color = state == LimitState::Nominal ? QColor(255, 165, 0)
                                                          : stateColor(state);
        return DonutValue{load, QString::number(lastPacket.obc.cpu_load_percent) + "%", color};
    }, QStringLiteral("obc.cpu_load_percent")));

    donuts->addWidget(makeDonut(QStringLiteral("Использование памяти"), [this]() {
        const double percent = lastPacket.obc.memory_used_kb / kMemoryTotalKb * 100.0;
        const LimitState state = limitStateFor(QStringLiteral("obc.memory_used_kb"));
        const QColor color = state == LimitState::Nominal ? QColor(128, 0, 128)
                                                          : stateColor(state);
        return DonutValue{percent,
                          QString::number(lastPacket.obc.memory_used_kb) + " КБ", color};
    }, QStringLiteral("obc.memory_used_kb")));

    tiles->addWidget(makeTile(QStringLiteral("Перезагрузки"), [this]() {
        return QString::number(lastPacket.obc.reboot_count);
    }, QStringLiteral("obc.reboot_count")));
    tiles->addWidget(makeTile(QStringLiteral("Сбросов watchdog"), [this]() {
        return QString::number(lastPacket.obc.watchdog_reset_count);
    }, QStringLiteral("obc.watchdog_reset_count")));
    tiles->addWidget(makeTile(QStringLiteral("Версия прошивки"), [this]() {
        // Прошивка закодирована числом: 142 -> v1.4.2
        const quint16 v = lastPacket.obc.firmware_version;
        return QStringLiteral("v%1.%2.%3").arg(v / 100).arg((v / 10) % 10).arg(v % 10);
    }));
    tiles->addWidget(makeTile(QStringLiteral("Код последней ошибки"), [this]() {
        const quint8 code = lastPacket.obc.last_error_code;
        return code == 0 ? QStringLiteral("нет") : QStringLiteral("0x%1").arg(code, 2, 16, QChar('0'));
    }, QStringLiteral("obc.last_error_code")));

    column->addLayout(donuts);
    column->addLayout(tiles);
    tab->addTab(page, tabNames()[3]);
}

void MainWindow::buildCommTab()
{
    QGroupBox *page = new QGroupBox();
    QVBoxLayout *column = new QVBoxLayout(page);
    QHBoxLayout *tiles  = new QHBoxLayout();

    tiles->addWidget(makeTile(QStringLiteral("Мощность передатчика"),
                              [this]() { return fmt(lastPacket.comm.tx_power_dbm, 1) + " дБм"; },
                              QStringLiteral("comm.tx_power_dbm")));
    tiles->addWidget(makeTile(QStringLiteral("RSSI"),
                              [this]() { return QString::number(lastPacket.comm.rssi) + " дБм"; },
                              QStringLiteral("comm.rssi")));
    tiles->addWidget(makeTile(QStringLiteral("Частота ошибок (BER)"), [this]() {
        return QString::number(lastPacket.comm.bit_error_rate, 'e', 2);
    }, QStringLiteral("comm.bit_error_rate")));
    tiles->addWidget(makeTile(QStringLiteral("Антенна"), [this]() {
        return lastPacket.comm.antenna_deployed ? QStringLiteral("раскрыта")
                                                : QStringLiteral("НЕ РАСКРЫТА");
    }, QStringLiteral("comm.antenna_deployed")));

    column->addLayout(tiles);
    column->addStretch(1);
    tab->addTab(page, tabNames()[4]);
}

void MainWindow::buildGpsTab()
{
    QGroupBox *page = new QGroupBox();
    QVBoxLayout *column = new QVBoxLayout(page);
    QHBoxLayout *tiles  = new QHBoxLayout();

    tiles->addWidget(makeTile(QStringLiteral("Широта"),
                              [this]() { return fmt(lastPacket.gps.latitude, 5) + "°"; }));
    tiles->addWidget(makeTile(QStringLiteral("Долгота"),
                              [this]() { return fmt(lastPacket.gps.longitude, 5) + "°"; }));
    tiles->addWidget(makeTile(QStringLiteral("Высота"),
                              [this]() { return fmt(lastPacket.gps.altitude_km, 1) + " км"; },
                              QStringLiteral("gps.altitude_km")));
    tiles->addWidget(makeTile(QStringLiteral("Скорость"),
                              [this]() { return fmt(lastPacket.gps.speed_kmh, 0) + " км/ч"; },
                              QStringLiteral("gps.speed_kmh")));

    QHBoxLayout *timeRow = new QHBoxLayout();
    timeRow->addWidget(makeTile(QStringLiteral("Время на борту (UTC)"), [this]() {
        if (lastPacket.gps.gps_timestamp == 0)
            return QStringLiteral("—");
        return QDateTime::fromSecsSinceEpoch(lastPacket.gps.gps_timestamp, QTimeZone::UTC)
            .toString("yyyy-MM-dd hh:mm:ss");
    }));
    timeRow->addStretch(1);

    column->addLayout(tiles);
    column->addLayout(timeRow);
    column->addStretch(1);
    tab->addTab(page, tabNames()[5]);
}

void MainWindow::updateViews()
{
    for (const auto &update : valueUpdaters)
        update();

    // График температур ведём по принятым кадрам: одна точка на кадр.
    if (ctemp && havePacket && solartemp.size() == 12) {
        for (int i = 0; i < 12; ++i) {
            solartemp[i]->append(index, lastPacket.thermal.temp_solar[i]);
            if (solartemp[i]->count() > kTrendPoints)
                solartemp[i]->remove(0);
        }
        // Счётчик двигаем один раз на кадр, а не на каждую панель.
        ++index;
        if (axisXt)
            axisXt->setRange(qMax(0, index - 20), qMax(20, index));
    }
}

void MainWindow::applyVisuals()
{
    // Устаревание важнее границ: про данные пятиминутной давности нельзя
    // говорить, что параметр «сейчас» вне нормы. Реальные пульты по той же
    // причине снимают аварию, когда значение перестаёт обновляться.
    const bool grey = !havePacket || stale;

    for (const ValueVisual &visual : valueVisuals) {
        QColor color = visual.nominalColor;
        if (grey) {
            color = kStaleColor;
        } else if (!visual.parameterId.isEmpty()) {
            const LimitState state = limitStateFor(visual.parameterId);
            if (state == LimitState::Warning || state == LimitState::Critical)
                color = stateColor(state);
        }
        visual.label->setStyleSheet(visual.baseStyle
                                    + QStringLiteral("color: %1;").arg(color.name()));
    }

    updateTabColors();
}

void MainWindow::updateTabColors()
{
    // По цвету заголовка видно, в какой подсистеме проблема, — не нужно
    // щёлкать по вкладкам, чтобы это выяснить.
    const bool grey = !havePacket || stale;
    const int tabs = int(sizeof(kTabPrefixes) / sizeof(kTabPrefixes[0]));

    for (int i = 0; i < tab->count() && i < tabs; ++i) {
        LimitState worst = LimitState::NoLimits;
        if (!grey) {
            const QString prefix = QString::fromLatin1(kTabPrefixes[i]);
            for (auto it = currentStates.constBegin(); it != currentStates.constEnd(); ++it) {
                if (it.key().startsWith(prefix))
                    worst = limits::worse(worst, it.value());
            }
        }

        const bool alarming = worst == LimitState::Warning || worst == LimitState::Critical;
        tab->tabBar()->setTabTextColor(i, alarming ? stateColor(worst) : QColor());
    }
}

void MainWindow::updateLimitTable()
{
    // Показываем только нарушения: строка «всё в норме» оператору не нужна,
    // а шестьдесят таких строк мешают увидеть единственную важную.
    QVector<ParameterSample> offenders;
    for (auto it = currentStates.constBegin(); it != currentStates.constEnd(); ++it) {
        if (it.value() == LimitState::Warning || it.value() == LimitState::Critical)
            offenders.append(ParameterSample{it.key(), currentValues.value(it.key())});
    }

    // Аварии наверх, внутри одной серьёзности — по имени, чтобы строки не
    // прыгали от кадра к кадру.
    std::sort(offenders.begin(), offenders.end(),
              [this](const ParameterSample &a, const ParameterSample &b) {
                  const LimitState sa = limitStateFor(a.id);
                  const LimitState sb = limitStateFor(b.id);
                  if (sa != sb)
                      return sa == LimitState::Critical;
                  return a.id < b.id;
              });

    warningCount  = 0;
    criticalCount = 0;
    for (const ParameterSample &sample : offenders) {
        if (limitStateFor(sample.id) == LimitState::Critical)
            ++criticalCount;
        else
            ++warningCount;
    }

    limitView->setRowCount(offenders.size());
    for (int row = 0; row < offenders.size(); ++row) {
        const ParameterSample &sample = offenders[row];
        const ParameterLimits *limit = limits::find(sample.id);
        const LimitState state = limitStateFor(sample.id);
        const QColor color = stateColor(state);

        const QString name = limit ? limit->name : sample.id;
        const QString unit = limit && !limit->unit.isEmpty() ? " " + limit->unit : QString();

        // Показываем тот коридор, который нарушен, — оператору нужно знать,
        // насколько далеко ушло значение, а не всю четвёрку границ.
        QString nominal = QStringLiteral("—");
        if (limit) {
            const bool tooLow = sample.value < limit->warningLow;
            if (tooLow && limit->warningLow > limits::kNoLow)
                nominal = QStringLiteral("не ниже %1").arg(fmt(limit->warningLow, 2));
            else if (!tooLow && limit->warningHigh < limits::kNoHigh)
                nominal = QStringLiteral("не выше %1").arg(fmt(limit->warningHigh, 2));
        }

        const auto cell = [&color](const QString &text) {
            QTableWidgetItem *item = new QTableWidgetItem(text);
            item->setForeground(color);
            return item;
        };

        limitView->setItem(row, 0, cell(name));
        limitView->setItem(row, 1, cell(fmt(sample.value, 2) + unit));
        limitView->setItem(row, 2, cell(nominal));
        limitView->setItem(row, 3, cell(limits::stateText(state)));
    }
}
