// ---------------------------------------------------------------------------
//  Тесты протокола и границ нормы.
//
//  Запуск:  ctest --test-dir <каталог сборки> --output-on-failure
//  либо просто исполняемым файлом cubesat_tests.
//
//  Намеренно без QTest: тестируются чистые функции, и одного QtCore хватает.
//  Проверять формат кадра особенно важно — раскладка задана в двух местах
//  (protocol.h и sim/sat_sim.py), и разойтись они могут незаметно.
// ---------------------------------------------------------------------------

#include "limitcheck.h"
#include "protocol.h"
#include "telemetry.h"

#include <QCoreApplication>
#include <QSet>
#include <QStringList>
#include <QTextStream>

#include <cmath>

namespace {

int checks   = 0;
int failures = 0;
QTextStream *out = nullptr;

void ok(bool condition, const QString &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        *out << "  ОШИБКА: " << what << "\n";
    }
}

void nearly(double got, double want, const QString &what, double eps = 1e-4)
{
    ok(std::fabs(got - want) < eps,
       QStringLiteral("%1: получено %2, ожидалось %3").arg(what).arg(got).arg(want));
}

QString stateName(LimitState s)
{
    switch (s) {
    case LimitState::NoLimits: return QStringLiteral("без границ");
    case LimitState::Nominal:  return QStringLiteral("норма");
    case LimitState::Warning:  return QStringLiteral("предупреждение");
    case LimitState::Critical: return QStringLiteral("авария");
    }
    return QStringLiteral("?");
}

void expectState(const QString &id, double value, LimitState want)
{
    const LimitState got = limits::check(id, value);
    ok(got == want, QStringLiteral("%1 = %2 дало «%3», ожидалось «%4»")
                        .arg(id).arg(value, 0, 'g', 6)
                        .arg(stateName(got), stateName(want)));
}

// Пакет с заметными значениями: перепутанные поля сразу видны.
TelemetryPacket makeReferencePacket()
{
    TelemetryPacket p{};
    p.timestamp = 1764000000u;

    p.eps.bus_voltage[0] = 3.25f;
    p.eps.bus_voltage[1] = 5.125f;
    p.eps.bus_voltage[2] = 8.375f;
    for (int i = 0; i < 3; ++i)
        p.eps.bus_current[i] = 0.25f + float(i) / 8.0f;
    p.eps.battery_voltage = 7.375f;
    p.eps.battery_temp    = 18.5f;
    p.eps.battery_current = -0.375f;
    p.eps.soc_percent     = 73;
    for (int i = 0; i < 12; ++i)
        p.eps.solar_power[i] = 0.5f * float(i);

    p.thermal.temp_cpu     = 41.5f;
    p.thermal.temp_battery = 18.5f;
    p.thermal.temp_antenna = -12.25f;
    p.thermal.temp_in      = 21.75f;
    for (int i = 0; i < 12; ++i)
        p.thermal.temp_solar[i] = -17.25f + 2.5f * float(i);

    for (int i = 0; i < 3; ++i) p.adcs.gyro[i] = -0.125f * float(i + 1);
    for (int i = 0; i < 3; ++i) p.adcs.magnetometer[i] = 20.5f + float(i);
    for (int i = 0; i < 6; ++i) p.adcs.sun_sensor[i] = 0.125f * float(i);
    p.adcs.quaternion[0] = 1.0f;
    p.adcs.quaternion[1] = 0.0f;
    p.adcs.quaternion[2] = 0.0f;
    p.adcs.quaternion[3] = 0.0625f;
    p.adcs.reaction_wheel_speed[0] = 1200.5f;
    p.adcs.reaction_wheel_speed[1] = 1234.5f;
    p.adcs.reaction_wheel_speed[2] = -1100.25f;

    p.obc.cpu_load_percent     = 91;
    p.obc.memory_used_kb       = 748;
    p.obc.reboot_count         = 7;
    p.obc.watchdog_reset_count = 2;
    p.obc.firmware_version     = 142;
    p.obc.last_error_code      = 34;

    p.comm.tx_power_dbm     = 19.5f;
    p.comm.rssi             = -113;
    p.comm.bit_error_rate   = 0.000125f;
    p.comm.antenna_deployed = true;

    p.gps.latitude      = 55.7512345;
    p.gps.longitude     = 37.6187654;
    p.gps.altitude_km   = 412.5f;
    p.gps.speed_kmh     = 27625.0f;
    p.gps.gps_timestamp = 1764000000u;

    return p;
}

// --- Протокол --------------------------------------------------------------

void testFrameSizes()
{
    *out << "формат кадра\n";
    ok(protocol::kPacketSize == 288, QStringLiteral("размер кадра должен быть 288"));
    ok(protocol::kPayloadSize == 280, QStringLiteral("размер payload должен быть 280"));

    const QByteArray frame = protocol::encode(makeReferencePacket(), 1337);
    ok(frame.size() == protocol::kPacketSize,
       QStringLiteral("собранный кадр %1 байт вместо %2")
           .arg(frame.size()).arg(protocol::kPacketSize));

    // Заголовок: версия 0, тип 0 (телеметрия), признак вторичного заголовка 0.
    const quint8 first = quint8(frame[0]);
    ok((first & 0xE0) == 0, QStringLiteral("версия пакета должна быть 0"));
    ok((first & 0x10) == 0, QStringLiteral("тип должен быть 0 (телеметрия)"));

    // Длина Data Field объявляется на единицу меньше фактической.
    const int declared = (quint8(frame[4]) << 8 | quint8(frame[5])) + 1;
    ok(declared == protocol::kPayloadSize + protocol::kCrcSize,
       QStringLiteral("объявленная длина Data Field %1").arg(declared));
}

void testRoundTrip()
{
    *out << "кадр туда и обратно\n";
    const TelemetryPacket sent = makeReferencePacket();
    const protocol::DecodeResult r = protocol::decode(protocol::encode(sent, 1337));

    ok(r.isOk(), QStringLiteral("разбор не удался: %1").arg(protocol::statusText(r.status)));
    ok(r.seq == 1337, QStringLiteral("счётчик последовательности потерялся"));
    ok(r.apid == protocol::kApidHousekeeping, QStringLiteral("APID потерялся"));

    const TelemetryPacket &got = r.packet;
    ok(got.timestamp == sent.timestamp, QStringLiteral("timestamp"));

    for (int i = 0; i < 3; ++i) {
        nearly(got.eps.bus_voltage[i], sent.eps.bus_voltage[i],
               QStringLiteral("eps.bus_voltage[%1]").arg(i));
        nearly(got.eps.bus_current[i], sent.eps.bus_current[i],
               QStringLiteral("eps.bus_current[%1]").arg(i));
    }
    nearly(got.eps.battery_voltage, sent.eps.battery_voltage, "eps.battery_voltage");
    nearly(got.eps.battery_temp, sent.eps.battery_temp, "eps.battery_temp");
    nearly(got.eps.battery_current, sent.eps.battery_current, "eps.battery_current");
    ok(got.eps.soc_percent == sent.eps.soc_percent, QStringLiteral("eps.soc_percent"));
    for (int i = 0; i < 12; ++i)
        nearly(got.eps.solar_power[i], sent.eps.solar_power[i],
               QStringLiteral("eps.solar_power[%1]").arg(i));

    nearly(got.thermal.temp_cpu, sent.thermal.temp_cpu, "thermal.temp_cpu");
    nearly(got.thermal.temp_battery, sent.thermal.temp_battery, "thermal.temp_battery");
    nearly(got.thermal.temp_antenna, sent.thermal.temp_antenna, "thermal.temp_antenna");
    nearly(got.thermal.temp_in, sent.thermal.temp_in, "thermal.temp_in");
    for (int i = 0; i < 12; ++i)
        nearly(got.thermal.temp_solar[i], sent.thermal.temp_solar[i],
               QStringLiteral("thermal.temp_solar[%1]").arg(i));

    for (int i = 0; i < 3; ++i)
        nearly(got.adcs.gyro[i], sent.adcs.gyro[i], QStringLiteral("adcs.gyro[%1]").arg(i));
    for (int i = 0; i < 3; ++i)
        nearly(got.adcs.magnetometer[i], sent.adcs.magnetometer[i],
               QStringLiteral("adcs.magnetometer[%1]").arg(i));
    for (int i = 0; i < 6; ++i)
        nearly(got.adcs.sun_sensor[i], sent.adcs.sun_sensor[i],
               QStringLiteral("adcs.sun_sensor[%1]").arg(i));
    for (int i = 0; i < 4; ++i)
        nearly(got.adcs.quaternion[i], sent.adcs.quaternion[i],
               QStringLiteral("adcs.quaternion[%1]").arg(i));
    for (int i = 0; i < 3; ++i)
        nearly(got.adcs.reaction_wheel_speed[i], sent.adcs.reaction_wheel_speed[i],
               QStringLiteral("adcs.reaction_wheel_speed[%1]").arg(i));

    ok(got.obc.cpu_load_percent == sent.obc.cpu_load_percent, "obc.cpu_load_percent");
    ok(got.obc.memory_used_kb == sent.obc.memory_used_kb, "obc.memory_used_kb");
    ok(got.obc.reboot_count == sent.obc.reboot_count, "obc.reboot_count");
    ok(got.obc.watchdog_reset_count == sent.obc.watchdog_reset_count, "obc.watchdog_reset_count");
    ok(got.obc.firmware_version == sent.obc.firmware_version, "obc.firmware_version");
    ok(got.obc.last_error_code == sent.obc.last_error_code, "obc.last_error_code");

    nearly(got.comm.tx_power_dbm, sent.comm.tx_power_dbm, "comm.tx_power_dbm");
    ok(got.comm.rssi == sent.comm.rssi, QStringLiteral("comm.rssi (знаковое поле)"));
    nearly(got.comm.bit_error_rate, sent.comm.bit_error_rate, "comm.bit_error_rate", 1e-9);
    ok(got.comm.antenna_deployed == sent.comm.antenna_deployed, "comm.antenna_deployed");

    // Широта и долгота — double: восемь байт должны пережить дорогу целиком.
    nearly(got.gps.latitude, sent.gps.latitude, "gps.latitude", 1e-9);
    nearly(got.gps.longitude, sent.gps.longitude, "gps.longitude", 1e-9);
    nearly(got.gps.altitude_km, sent.gps.altitude_km, "gps.altitude_km");
    nearly(got.gps.speed_kmh, sent.gps.speed_kmh, "gps.speed_kmh");
    ok(got.gps.gps_timestamp == sent.gps.gps_timestamp, "gps.gps_timestamp");
}

void testSequenceWrap()
{
    *out << "счётчик последовательности\n";
    // Счётчик 14-битный: 16383 — максимум, дальше по кругу.
    const protocol::DecodeResult max =
        protocol::decode(protocol::encode(makeReferencePacket(), 16383));
    ok(max.seq == 16383, QStringLiteral("максимальный счётчик"));

    // Значение сверх разрядности должно обрезаться маской, а не сломать кадр.
    const protocol::DecodeResult wrapped =
        protocol::decode(protocol::encode(makeReferencePacket(), 16384));
    ok(wrapped.isOk(), QStringLiteral("кадр со счётчиком 16384 не разобрался"));
    ok(wrapped.seq == 0, QStringLiteral("счётчик 16384 должен обрезаться в 0"));
}

void testCorruptionDetected()
{
    *out << "обнаружение порчи и мусора\n";
    const QByteArray good = protocol::encode(makeReferencePacket(), 42);

    // Любой перевёрнутый бит в payload обязан поймать CRC.
    int missed = 0;
    for (int byte = protocol::kHeaderSize; byte < good.size() - protocol::kCrcSize; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            QByteArray broken = good;
            broken[byte] = char(quint8(broken[byte]) ^ (1u << bit));
            if (protocol::decode(broken).status != protocol::Status::CrcError)
                ++missed;
        }
    }
    ok(missed == 0, QStringLiteral("CRC пропустила %1 одиночных битовых ошибок").arg(missed));

    // Обрезанный кадр: объявленная длина не совпадёт с фактической.
    ok(protocol::decode(good.left(100)).status == protocol::Status::LengthMismatch,
       QStringLiteral("обрезанный кадр должен давать ДЛИНА"));

    // Совсем короткий обрывок.
    ok(protocol::decode(good.left(4)).status == protocol::Status::TooShort,
       QStringLiteral("четыре байта должны давать КОРОТКИЙ КАДР"));

    // Чужой APID — пакет не нашей подсистемы.
    const QByteArray foreign = protocol::encode(makeReferencePacket(), 42, 0x100);
    ok(protocol::decode(foreign).status == protocol::Status::UnknownApid,
       QStringLiteral("APID 0x100 должен отвергаться"));

    // Ненулевая версия пакета: в канал попало что-то не наше.
    QByteArray badVersion = good;
    badVersion[0] = char(quint8(badVersion[0]) | 0x20);
    ok(protocol::decode(badVersion).status == protocol::Status::BadVersion,
       QStringLiteral("версия не 0 должна отвергаться"));

    // Пустой ввод не должен приводить к падению.
    ok(protocol::decode(QByteArray()).status == protocol::Status::TooShort,
       QStringLiteral("пустой кадр должен давать КОРОТКИЙ КАДР"));
}

// --- Границы нормы ---------------------------------------------------------

void testLimitBoundaries()
{
    *out << "границы нормы\n";
    ok(!limits::table().isEmpty(), QStringLiteral("таблица границ пуста"));

    // SOC: предупреждение ниже 30, авария ниже 15. Границы включаются в норму.
    expectState("eps.soc_percent", 87, LimitState::Nominal);
    expectState("eps.soc_percent", 30, LimitState::Nominal);
    expectState("eps.soc_percent", 29, LimitState::Warning);
    expectState("eps.soc_percent", 15, LimitState::Warning);
    expectState("eps.soc_percent", 14, LimitState::Critical);

    // Двусторонний коридор.
    expectState("eps.bus_voltage[0]", 3.30, LimitState::Nominal);
    expectState("eps.bus_voltage[0]", 3.15, LimitState::Warning);
    expectState("eps.bus_voltage[0]", 3.05, LimitState::Critical);
    expectState("eps.bus_voltage[0]", 3.45, LimitState::Warning);
    expectState("eps.bus_voltage[0]", 3.60, LimitState::Critical);

    // Односторонний: RSSI чем выше, тем лучше, сверху не ограничен.
    expectState("comm.rssi", -50.0,  LimitState::Nominal);
    expectState("comm.rssi", -113.0, LimitState::Warning);
    expectState("comm.rssi", -125.0, LimitState::Critical);

    // Дискретный параметр как число.
    expectState("comm.antenna_deployed", 1.0, LimitState::Nominal);
    expectState("comm.antenna_deployed", 0.0, LimitState::Critical);

    // Любой ненулевой код ошибки — повод посмотреть.
    expectState("obc.last_error_code", 0.0,  LimitState::Nominal);
    expectState("obc.last_error_code", 34.0, LimitState::Warning);

    // Параметра нет в таблице — проверять нечего.
    expectState("eps.solar_power[3]", 5.0, LimitState::NoLimits);
    expectState("такого.параметра.нет", 1.0, LimitState::NoLimits);

    // Отсутствующее значение — тоже нештатная ситуация.
    expectState("thermal.temp_cpu", std::nan(""), LimitState::Critical);
}

void testDerivedParameters()
{
    *out << "производные параметры\n";
    TelemetryPacket p{};
    p.adcs.gyro[0] = 0.3f;
    p.adcs.gyro[1] = -7.5f; // максимум по модулю, знак не важен
    p.adcs.gyro[2] = 0.1f;
    p.adcs.reaction_wheel_speed[2] = 5500.0f;
    p.adcs.quaternion[0] = 1.0f;
    p.thermal.temp_solar[7] = 103.0f; // перегрелась одна панель из двенадцати
    p.comm.antenna_deployed = true;

    QHash<QString, double> values;
    for (const ParameterSample &s : limits::sample(p))
        values.insert(s.id, s.value);

    nearly(values.value("adcs.gyro_max"), 7.5, "adcs.gyro_max берёт модуль");
    nearly(values.value("adcs.wheel_max"), 5500.0, "adcs.wheel_max");
    nearly(values.value("thermal.temp_solar_max"), 103.0, "самая горячая панель");
    nearly(values.value("adcs.quaternion_norm"), 1.0, "норма единичного кватерниона");

    expectState("adcs.gyro_max", values.value("adcs.gyro_max"), LimitState::Critical);
    expectState("thermal.temp_solar_max", values.value("thermal.temp_solar_max"),
                LimitState::Critical);
    expectState("adcs.wheel_max", values.value("adcs.wheel_max"), LimitState::Warning);

    // Испорченный кватернион: это проверка качества данных, а не борта.
    p.adcs.quaternion[0] = 0.5f;
    double norm = 0.0;
    for (const ParameterSample &s : limits::sample(p))
        if (s.id == "adcs.quaternion_norm")
            norm = s.value;
    nearly(norm, 0.5, "норма испорченного кватерниона");
    expectState("adcs.quaternion_norm", norm, LimitState::Critical);

    // Каждый параметр из пакета должен иметь уникальный идентификатор.
    const QVector<ParameterSample> all = limits::sample(p);
    QSet<QString> ids;
    for (const ParameterSample &s : all)
        ids.insert(s.id);
    ok(ids.size() == all.size(), QStringLiteral("идентификаторы параметров повторяются"));

    // И каждая запись таблицы границ должна кем-то заполняться, иначе она
    // никогда не сработает.
    QStringList orphans;
    for (auto it = limits::table().constBegin(); it != limits::table().constEnd(); ++it)
        if (!ids.contains(it.key()))
            orphans << it.key();
    ok(orphans.isEmpty(),
       QStringLiteral("границы заданы для параметров, которых нет в пакете: %1")
           .arg(orphans.join(", ")));
}

void testWorse()
{
    *out << "сравнение серьёзности\n";
    ok(limits::worse(LimitState::Nominal, LimitState::Warning) == LimitState::Warning,
       QStringLiteral("предупреждение важнее нормы"));
    ok(limits::worse(LimitState::Critical, LimitState::Warning) == LimitState::Critical,
       QStringLiteral("авария важнее предупреждения"));
    ok(limits::worse(LimitState::NoLimits, LimitState::Nominal) == LimitState::Nominal,
       QStringLiteral("норма важнее отсутствия границ"));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream stream(stdout);
    out = &stream;

    testFrameSizes();
    testRoundTrip();
    testSequenceWrap();
    testCorruptionDetected();
    testLimitBoundaries();
    testDerivedParameters();
    testWorse();

    stream << "\nпроверок: " << checks << ", ошибок: " << failures << "\n";
    stream << (failures == 0 ? "ВСЁ СОШЛОСЬ\n" : "ЕСТЬ ОШИБКИ\n");
    return failures == 0 ? 0 : 1;
}
