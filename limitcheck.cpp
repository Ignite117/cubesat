#include "limitcheck.h"

#include <cmath>

LimitState ParameterLimits::evaluate(double value) const
{
    if (std::isnan(value))
        return LimitState::Critical; // отсутствие значения — тоже нештатная ситуация
    if (value < criticalLow || value > criticalHigh)
        return LimitState::Critical;
    if (value < warningLow || value > warningHigh)
        return LimitState::Warning;
    return LimitState::Nominal;
}

namespace limits {

// ---------------------------------------------------------------------------
//  Таблица границ. Единственное место, где они заданы.
//
//  Значения подобраны под типовой 1U/3U CubeSat: литий-ионная сборка 2S,
//  шины 3.3/5/8.4 В, орбита около 400 км. При смене аппарата правится только
//  эта таблица.
//
//  kNoLow / kNoHigh означают, что с этой стороны параметр не ограничен.
//  Дискретные параметры проверяются как числа: «антенна раскрыта» — это 1,
//  и criticalLow = 1 превращает 0 в аварию.
// ---------------------------------------------------------------------------
const QHash<QString, ParameterLimits> &table()
{
    static const QHash<QString, ParameterLimits> limits = {
        // --- EPS: питание ---------------------------------------------------
        //                              имя                          ед.     критНиз  предНиз  предВыс  критВыс
        {"eps.bus_voltage[0]",       {"Шина 3.3 В",                  "В",     3.10,    3.20,    3.40,    3.50}},
        {"eps.bus_voltage[1]",       {"Шина 5 В",                    "В",     4.70,    4.85,    5.15,    5.30}},
        {"eps.bus_voltage[2]",       {"Шина 8.4 В",                  "В",     8.00,    8.20,    8.60,    8.80}},
        {"eps.bus_current[0]",       {"Ток шины 3.3 В",              "А",   kNoLow,  kNoLow,    1.00,    1.50}},
        {"eps.bus_current[1]",       {"Ток шины 5 В",                "А",   kNoLow,  kNoLow,    1.00,    1.50}},
        {"eps.bus_current[2]",       {"Ток шины 8.4 В",              "А",   kNoLow,  kNoLow,    1.00,    1.50}},
        // Сборка 2S: 6.0 В — глубокий разряд, 8.4 В — полный заряд.
        {"eps.battery_voltage",      {"Напряжение батареи",           "В",     6.60,    6.90,    8.30,    8.45}},
        // Литий-ионные элементы нельзя заряжать на морозе и греть выше 45 °C.
        {"eps.battery_temp",         {"Температура батареи",         "°C",   -10.0,     0.0,    40.0,    50.0}},
        {"eps.battery_current",      {"Ток батареи",                  "А",    -1.50,   -1.00,    1.00,    1.50}},
        {"eps.soc_percent",          {"Заряд батареи (SOC)",          "%",    15.0,    30.0,  kNoHigh, kNoHigh}},

        // --- Терморегулирование ---------------------------------------------
        {"thermal.temp_cpu",         {"Температура CPU",             "°C",   -35.0,   -20.0,    60.0,    80.0}},
        {"thermal.temp_battery",     {"Температура батареи",         "°C",   -10.0,     0.0,    40.0,    50.0}},
        {"thermal.temp_antenna",     {"Температура антенны",         "°C",   -55.0,   -40.0,    60.0,    80.0}},
        {"thermal.temp_in",          {"Температура внутри корпуса",  "°C",   -25.0,   -10.0,    45.0,    60.0}},
        {"thermal.temp_solar_max",   {"Температура панелей (макс.)", "°C",   kNoLow,  kNoLow,    85.0,   100.0}},

        // --- ADCS: ориентация -----------------------------------------------
        // Выше 5 °/с аппарат считается закрученным, нужен режим гашения.
        {"adcs.gyro_max",            {"Угловая скорость (макс.)",   "°/с",   kNoLow,  kNoLow,     2.0,     5.0}},
        {"adcs.wheel_max",           {"Обороты маховиков (макс.)", "об/мин", kNoLow,  kNoLow,  5000.0,  6000.0}},
        // Норма кватерниона обязана равняться единице. Отклонение означает
        // сбой вычислителя ориентации или порчу данных — это проверка качества
        // самих данных, а не состояния борта.
        {"adcs.quaternion_norm",     {"Норма кватерниона",             "",     0.85,    0.95,    1.05,    1.15}},

        // --- OBC: бортовой компьютер ----------------------------------------
        {"obc.cpu_load_percent",     {"Загрузка CPU",                 "%",   kNoLow,  kNoLow,    80.0,    95.0}},
        {"obc.memory_used_kb",       {"Использование памяти",        "КБ",   kNoLow,  kNoLow,   800.0,   950.0}},
        {"obc.reboot_count",         {"Перезагрузки",                  "",   kNoLow,  kNoLow,    10.0,    25.0}},
        {"obc.watchdog_reset_count", {"Сбросов watchdog",              "",   kNoLow,  kNoLow,     5.0,    10.0}},
        // Любой ненулевой код ошибки — повод посмотреть, что случилось.
        {"obc.last_error_code",      {"Код последней ошибки",          "",   kNoLow,  kNoLow,     0.0, kNoHigh}},

        // --- COMM: радиосвязь -----------------------------------------------
        {"comm.tx_power_dbm",        {"Мощность передатчика",       "дБм",    15.0,    17.0,    23.0,    25.0}},
        // Чем выше RSSI, тем лучше, поэтому ограничен только снизу.
        {"comm.rssi",                {"Уровень сигнала (RSSI)",     "дБм",  -120.0,  -110.0,  kNoHigh, kNoHigh}},
        {"comm.bit_error_rate",      {"Частота ошибок (BER)",          "",   kNoLow,  kNoLow,   1.0e-3,  1.0e-2}},
        // Дискретный параметр: 1 — раскрыта, 0 — нет. Нераскрытая антенна
        // означает, что связь скоро пропадёт совсем.
        {"comm.antenna_deployed",    {"Антенна раскрыта",              "",     1.0,     1.0,  kNoHigh, kNoHigh}},

        // --- GPS ------------------------------------------------------------
        {"gps.altitude_km",          {"Высота орбиты",                "км",   350.0,   380.0,   450.0,   500.0}},
        {"gps.speed_kmh",            {"Орбитальная скорость",       "км/ч", 26500.0, 27000.0, 28200.0, 28800.0}},
    };
    return limits;
}

const ParameterLimits *find(const QString &id)
{
    const auto &all = table();
    const auto it = all.constFind(id);
    return it == all.constEnd() ? nullptr : &it.value();
}

LimitState check(const QString &id, double value)
{
    const ParameterLimits *limit = find(id);
    return limit ? limit->evaluate(value) : LimitState::NoLimits;
}

QVector<ParameterSample> sample(const TelemetryPacket &p)
{
    QVector<ParameterSample> out;
    out.reserve(40);

    const auto add = [&out](const QString &id, double value) {
        out.append(ParameterSample{id, value});
    };

    // --- EPS
    for (int i = 0; i < 3; ++i) {
        add(QStringLiteral("eps.bus_voltage[%1]").arg(i), p.eps.bus_voltage[i]);
        add(QStringLiteral("eps.bus_current[%1]").arg(i), p.eps.bus_current[i]);
    }
    add(QStringLiteral("eps.battery_voltage"), p.eps.battery_voltage);
    add(QStringLiteral("eps.battery_temp"), p.eps.battery_temp);
    add(QStringLiteral("eps.battery_current"), p.eps.battery_current);
    add(QStringLiteral("eps.soc_percent"), p.eps.soc_percent);

    // --- Терморегулирование
    add(QStringLiteral("thermal.temp_cpu"), p.thermal.temp_cpu);
    add(QStringLiteral("thermal.temp_battery"), p.thermal.temp_battery);
    add(QStringLiteral("thermal.temp_antenna"), p.thermal.temp_antenna);
    add(QStringLiteral("thermal.temp_in"), p.thermal.temp_in);

    // Производный параметр: самая горячая панель. Ограничивать каждую из
    // двенадцати по отдельности незачем — важно, что перегрелась хоть одна.
    double hottestPanel = p.thermal.temp_solar[0];
    for (int i = 1; i < 12; ++i)
        hottestPanel = std::fmax(hottestPanel, double(p.thermal.temp_solar[i]));
    add(QStringLiteral("thermal.temp_solar_max"), hottestPanel);

    // --- ADCS
    double gyroMax = 0.0;
    for (int i = 0; i < 3; ++i)
        gyroMax = std::fmax(gyroMax, std::fabs(double(p.adcs.gyro[i])));
    add(QStringLiteral("adcs.gyro_max"), gyroMax);

    double wheelMax = 0.0;
    for (int i = 0; i < 3; ++i)
        wheelMax = std::fmax(wheelMax, std::fabs(double(p.adcs.reaction_wheel_speed[i])));
    add(QStringLiteral("adcs.wheel_max"), wheelMax);

    double quatSquares = 0.0;
    for (int i = 0; i < 4; ++i)
        quatSquares += double(p.adcs.quaternion[i]) * double(p.adcs.quaternion[i]);
    add(QStringLiteral("adcs.quaternion_norm"), std::sqrt(quatSquares));

    // --- OBC
    add(QStringLiteral("obc.cpu_load_percent"), p.obc.cpu_load_percent);
    add(QStringLiteral("obc.memory_used_kb"), p.obc.memory_used_kb);
    add(QStringLiteral("obc.reboot_count"), p.obc.reboot_count);
    add(QStringLiteral("obc.watchdog_reset_count"), p.obc.watchdog_reset_count);
    add(QStringLiteral("obc.last_error_code"), p.obc.last_error_code);

    // --- COMM
    add(QStringLiteral("comm.tx_power_dbm"), p.comm.tx_power_dbm);
    add(QStringLiteral("comm.rssi"), p.comm.rssi);
    add(QStringLiteral("comm.bit_error_rate"), p.comm.bit_error_rate);
    add(QStringLiteral("comm.antenna_deployed"), p.comm.antenna_deployed ? 1.0 : 0.0);

    // --- GPS
    add(QStringLiteral("gps.altitude_km"), p.gps.altitude_km);
    add(QStringLiteral("gps.speed_kmh"), p.gps.speed_kmh);

    return out;
}

QString stateText(LimitState state)
{
    switch (state) {
    case LimitState::Critical: return QStringLiteral("АВАРИЯ");
    case LimitState::Warning:  return QStringLiteral("ПРЕДУПРЕЖДЕНИЕ");
    case LimitState::Nominal:  return QStringLiteral("норма");
    case LimitState::NoLimits: break;
    }
    return QStringLiteral("—");
}

LimitState worse(LimitState a, LimitState b)
{
    const auto rank = [](LimitState s) {
        switch (s) {
        case LimitState::NoLimits: return 0;
        case LimitState::Nominal:  return 1;
        case LimitState::Warning:  return 2;
        case LimitState::Critical: return 3;
        }
        return 0;
    };
    return rank(a) >= rank(b) ? a : b;
}

} // namespace limits
