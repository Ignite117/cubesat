#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "telemetry.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <cstring>

// ---------------------------------------------------------------------------
//  Формат кадра телеметрии — в стиле CCSDS Space Packet (133.0-B).
//  Порядок байт: big-endian (network order), как принято в CCSDS.
//
//  +---------------------------------------------------------------+
//  | Primary Header — 6 байт                                       |
//  |   version(3)=0  type(1)=0  sec_hdr(1)=0  APID(11)     2 байта |
//  |   seq_flags(2)=0b11 (unsegmented)  seq_count(14)      2 байта |
//  |   packet_data_length(16) = длина Data Field минус 1   2 байта |
//  +---------------------------------------------------------------+
//  | Data Field                                                    |
//  |   payload: поля TelemetryPacket                     280 байт  |
//  |   CRC-16-CCITT (Packet Error Control)                 2 байта |
//  +---------------------------------------------------------------+
//  Итого 288 байт.
//
//  CRC считается по всему кадру от первого байта заголовка до поля CRC.
//
//  Важно: структура НЕ передаётся как сырая память (memcpy). Каждое поле
//  пишется и читается явно, потому что выравнивание полей структуры зависит
//  от компилятора и архитектуры, а порядок байт у борта и у земли в общем
//  случае разный.
//
//  Байтовая раскладка payload описана в PROTOCOL.md — тот же порядок полей
//  реализован в sim/sat_sim.py. Меняешь здесь — меняй в обоих местах.
// ---------------------------------------------------------------------------

namespace protocol {

constexpr int kHeaderSize  = 6;
constexpr int kPayloadSize = 280;
constexpr int kCrcSize     = 2;
constexpr int kPacketSize  = kHeaderSize + kPayloadSize + kCrcSize; // 288

// APID — идентификатор источника данных на борту. У нас один сводный пакет
// housekeeping; в реальных миссиях каждая подсистема шлёт свой APID.
constexpr quint16 kApidHousekeeping = 0x064;

constexpr quint16 kSeqCountMask   = 0x3FFF; // счётчик 14-битный, переполняется
constexpr quint16 kDefaultUdpPort = 5005;

// --- CRC-16-CCITT: полином 0x1021, начальное значение 0xFFFF, старшим битом
// вперёд. Именно этот вариант CCSDS использует как Packet Error Control.
inline quint16 crc16Ccitt(const char *data, int len)
{
    quint16 crc = 0xFFFF;
    for (int i = 0; i < len; ++i) {
        crc ^= static_cast<quint16>(static_cast<quint8>(data[i])) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? static_cast<quint16>((crc << 1) ^ 0x1021)
                                 : static_cast<quint16>(crc << 1);
        }
    }
    return crc;
}

// --- Чтение big-endian полей с контролем границ буфера.
class Reader
{
public:
    Reader(const char *data, int size)
        : p_(reinterpret_cast<const quint8 *>(data))
        , end_(reinterpret_cast<const quint8 *>(data) + size)
    {}

    bool ok() const { return ok_; }

    quint8 u8() { return take(1) ? *p_++ : quint8(0); }

    quint16 u16()
    {
        if (!take(2)) return 0;
        const quint16 v = static_cast<quint16>(quint16(p_[0]) << 8 | p_[1]);
        p_ += 2;
        return v;
    }

    quint32 u32()
    {
        if (!take(4)) return 0;
        const quint32 v = quint32(p_[0]) << 24 | quint32(p_[1]) << 16
                          | quint32(p_[2]) << 8 | quint32(p_[3]);
        p_ += 4;
        return v;
    }

    qint16  i16() { return static_cast<qint16>(u16()); }
    quint64 u64() { const quint64 hi = u32(); const quint64 lo = u32(); return hi << 32 | lo; }

    float f32()
    {
        const quint32 bits = u32();
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    double f64()
    {
        const quint64 bits = u64();
        double v = 0.0;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

private:
    bool take(int n)
    {
        if (end_ - p_ < n) { ok_ = false; return false; }
        return true;
    }

    const quint8 *p_;
    const quint8 *end_;
    bool ok_ = true;
};

// --- Запись big-endian полей.
class Writer
{
public:
    explicit Writer(QByteArray &out) : out_(out) {}

    void u8(quint8 v)   { out_.append(static_cast<char>(v)); }
    void u16(quint16 v) { u8(quint8(v >> 8)); u8(quint8(v & 0xFF)); }
    void i16(qint16 v)  { u16(static_cast<quint16>(v)); }
    void u32(quint32 v) { u16(quint16(v >> 16)); u16(quint16(v & 0xFFFF)); }
    void u64(quint64 v) { u32(quint32(v >> 32)); u32(quint32(v & 0xFFFFFFFFu)); }

    void f32(float v)  { quint32 b = 0; std::memcpy(&b, &v, sizeof(v)); u32(b); }
    void f64(double v) { quint64 b = 0; std::memcpy(&b, &v, sizeof(v)); u64(b); }

private:
    QByteArray &out_;
};

// --- Порядок полей payload. Единственное место, где он задан для C++.
inline void writePayload(Writer &w, const TelemetryPacket &p)
{
    w.u32(p.timestamp);

    // EPS
    for (int i = 0; i < 3; ++i) w.f32(p.eps.bus_voltage[i]);
    for (int i = 0; i < 3; ++i) w.f32(p.eps.bus_current[i]);
    w.f32(p.eps.battery_voltage);
    w.f32(p.eps.battery_temp);
    w.f32(p.eps.battery_current);
    w.u8(p.eps.soc_percent);
    for (int i = 0; i < 12; ++i) w.f32(p.eps.solar_power[i]);

    // Thermal
    w.f32(p.thermal.temp_cpu);
    w.f32(p.thermal.temp_battery);
    w.f32(p.thermal.temp_antenna);
    w.f32(p.thermal.temp_in);
    for (int i = 0; i < 12; ++i) w.f32(p.thermal.temp_solar[i]);

    // ADCS
    for (int i = 0; i < 3; ++i) w.f32(p.adcs.gyro[i]);
    for (int i = 0; i < 3; ++i) w.f32(p.adcs.magnetometer[i]);
    for (int i = 0; i < 6; ++i) w.f32(p.adcs.sun_sensor[i]);
    for (int i = 0; i < 4; ++i) w.f32(p.adcs.quaternion[i]);
    for (int i = 0; i < 3; ++i) w.f32(p.adcs.reaction_wheel_speed[i]);

    // OBC
    w.u8(p.obc.cpu_load_percent);
    w.u32(p.obc.memory_used_kb);
    w.u16(p.obc.reboot_count);
    w.u16(p.obc.watchdog_reset_count);
    w.u16(p.obc.firmware_version);
    w.u8(p.obc.last_error_code);

    // Comm
    w.f32(p.comm.tx_power_dbm);
    w.i16(p.comm.rssi);
    w.f32(p.comm.bit_error_rate);
    w.u8(p.comm.antenna_deployed ? 1 : 0);

    // GPS
    w.f64(p.gps.latitude);
    w.f64(p.gps.longitude);
    w.f32(p.gps.altitude_km);
    w.f32(p.gps.speed_kmh);
    w.u32(p.gps.gps_timestamp);
}

inline TelemetryPacket readPayload(Reader &r)
{
    TelemetryPacket p{};
    p.timestamp = r.u32();

    // EPS
    for (int i = 0; i < 3; ++i) p.eps.bus_voltage[i] = r.f32();
    for (int i = 0; i < 3; ++i) p.eps.bus_current[i] = r.f32();
    p.eps.battery_voltage = r.f32();
    p.eps.battery_temp    = r.f32();
    p.eps.battery_current = r.f32();
    p.eps.soc_percent     = r.u8();
    for (int i = 0; i < 12; ++i) p.eps.solar_power[i] = r.f32();

    // Thermal
    p.thermal.temp_cpu     = r.f32();
    p.thermal.temp_battery = r.f32();
    p.thermal.temp_antenna = r.f32();
    p.thermal.temp_in      = r.f32();
    for (int i = 0; i < 12; ++i) p.thermal.temp_solar[i] = r.f32();

    // ADCS
    for (int i = 0; i < 3; ++i) p.adcs.gyro[i] = r.f32();
    for (int i = 0; i < 3; ++i) p.adcs.magnetometer[i] = r.f32();
    for (int i = 0; i < 6; ++i) p.adcs.sun_sensor[i] = r.f32();
    for (int i = 0; i < 4; ++i) p.adcs.quaternion[i] = r.f32();
    for (int i = 0; i < 3; ++i) p.adcs.reaction_wheel_speed[i] = r.f32();

    // OBC
    p.obc.cpu_load_percent     = r.u8();
    p.obc.memory_used_kb       = r.u32();
    p.obc.reboot_count         = r.u16();
    p.obc.watchdog_reset_count = r.u16();
    p.obc.firmware_version     = r.u16();
    p.obc.last_error_code      = r.u8();

    // Comm
    p.comm.tx_power_dbm     = r.f32();
    p.comm.rssi             = r.i16();
    p.comm.bit_error_rate   = r.f32();
    p.comm.antenna_deployed = r.u8() != 0;

    // GPS
    p.gps.latitude      = r.f64();
    p.gps.longitude     = r.f64();
    p.gps.altitude_km   = r.f32();
    p.gps.speed_kmh     = r.f32();
    p.gps.gps_timestamp = r.u32();

    p.crc = 0; // заполняется на уровне кадра, в payload не входит
    return p;
}

// --- Сборка кадра. Нужна для тестов и для повторной отправки записанных
// пакетов; штатно кадры собирает борт (sim/sat_sim.py).
inline QByteArray encode(const TelemetryPacket &packet, quint16 seqCount,
                         quint16 apid = kApidHousekeeping)
{
    QByteArray payload;
    payload.reserve(kPayloadSize);
    Writer payloadWriter(payload);
    writePayload(payloadWriter, packet);

    QByteArray frame;
    frame.reserve(kPacketSize);
    Writer frameWriter(frame);

    // version=0, type=0 (телеметрия), secondary header flag=0
    frameWriter.u16(static_cast<quint16>(apid & 0x07FF));
    // seq_flags=0b11 — пакет не сегментирован
    frameWriter.u16(static_cast<quint16>(0xC000 | (seqCount & kSeqCountMask)));
    frameWriter.u16(static_cast<quint16>(payload.size() + kCrcSize - 1));

    frame.append(payload);
    frameWriter.u16(crc16Ccitt(frame.constData(), frame.size()));
    return frame;
}

// --- Разбор кадра.
enum class Status {
    Ok,
    TooShort,        // не хватает даже на заголовок с CRC
    BadVersion,      // не CCSDS-пакет версии 0 — скорее всего мусор в канале
    LengthMismatch,  // объявленная длина не совпала с фактической
    CrcError,        // кадр побился в канале
    UnknownApid,     // пакет не от той подсистемы, которую мы умеем разбирать
    Truncated        // payload короче, чем требует раскладка
};

struct DecodeResult
{
    Status status = Status::TooShort;
    quint16 apid = 0;
    quint16 seq  = 0;
    TelemetryPacket packet{};

    bool isOk() const { return status == Status::Ok; }
};

inline QString statusText(Status s)
{
    switch (s) {
    case Status::Ok:             return QStringLiteral("OK");
    case Status::TooShort:       return QStringLiteral("КОРОТКИЙ КАДР");
    case Status::BadVersion:     return QStringLiteral("НЕ CCSDS");
    case Status::LengthMismatch: return QStringLiteral("ДЛИНА");
    case Status::CrcError:       return QStringLiteral("CRC ERR");
    case Status::UnknownApid:    return QStringLiteral("ЧУЖОЙ APID");
    case Status::Truncated:      return QStringLiteral("ОБРЫВ PAYLOAD");
    }
    return QStringLiteral("?");
}

inline DecodeResult decode(const QByteArray &frame)
{
    DecodeResult res;

    if (frame.size() < kHeaderSize + kCrcSize) {
        res.status = Status::TooShort;
        return res;
    }

    Reader head(frame.constData(), frame.size());
    const quint16 word0 = head.u16();
    const quint16 word1 = head.u16();
    const quint16 dataLengthField = head.u16();

    const quint8 version = static_cast<quint8>((word0 >> 13) & 0x07);
    res.apid = static_cast<quint16>(word0 & 0x07FF);
    res.seq  = static_cast<quint16>(word1 & kSeqCountMask);

    if (version != 0) {
        res.status = Status::BadVersion;
        return res;
    }

    const int dataFieldSize = int(dataLengthField) + 1;
    if (kHeaderSize + dataFieldSize != frame.size()) {
        res.status = Status::LengthMismatch;
        return res;
    }

    // CRC проверяется до разбора полей: содержимому битого кадра верить нельзя.
    const int crcOffset = frame.size() - kCrcSize;
    const quint16 expected = crc16Ccitt(frame.constData(), crcOffset);
    const quint16 actual = static_cast<quint16>(
        quint16(quint8(frame[crcOffset])) << 8 | quint8(frame[crcOffset + 1]));
    if (expected != actual) {
        res.status = Status::CrcError;
        return res;
    }

    if (res.apid != kApidHousekeeping) {
        res.status = Status::UnknownApid;
        return res;
    }
    if (dataFieldSize - kCrcSize != kPayloadSize) {
        res.status = Status::LengthMismatch;
        return res;
    }

    Reader body(frame.constData() + kHeaderSize, kPayloadSize);
    res.packet = readPayload(body);
    res.status = body.ok() ? Status::Ok : Status::Truncated;
    return res;
}

} // namespace protocol

#endif // PROTOCOL_H
