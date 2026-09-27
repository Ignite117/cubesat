#ifndef UDPLINK_H
#define UDPLINK_H

#include "protocol.h"
#include "rawlog.h"
#include "telemetry.h"

#include <QHostAddress>
#include <QObject>
#include <QString>

class QUdpSocket;

// Счётчики качества канала. В реальных пультах они всегда на виду: по ним
// оператор понимает, связь плохая или борт молчит.
struct LinkStats
{
    quint64 received  = 0; // кадров разобрано успешно
    quint64 crcErrors = 0; // пришли, но побились в канале
    quint64 malformed = 0; // не похожи на наш пакет вовсе
    quint64 lost      = 0; // пропуски по счётчику последовательности

    void reset() { *this = LinkStats{}; }
};

// ---------------------------------------------------------------------------
//  Приём телеметрии по UDP.
//
//  Одна датаграмма — один кадр: границы посылок держит транспорт, поэтому
//  искать синхрослово в потоке не нужно. При переходе на serial здесь
//  появился бы буфер и поиск маркера 0x1ACFFC1D.
//
//  Класс ничего не генерирует. Он только принимает, пишет сырой журнал,
//  проверяет CRC, считает потери и отдаёт разобранный пакет наверх.
// ---------------------------------------------------------------------------
class UdpTelemetryLink : public QObject
{
    Q_OBJECT

public:
    explicit UdpTelemetryLink(QObject *parent = nullptr);
    ~UdpTelemetryLink() override;

    // Начинает слушать порт на всех интерфейсах.
    bool open(quint16 port, QString *error = nullptr);
    void close();
    bool isOpen() const;

    quint16 port() const { return port_; }
    const LinkStats &stats() const { return stats_; }
    void resetStats();

    // Журнал сырых кадров. Пишется всё принятое, до проверки CRC.
    bool startLog(const QString &path, QString *error = nullptr);
    void stopLog();
    QString logPath() const { return log_.isOpen() ? log_.path() : QString(); }
    quint64 loggedFrames() const { return log_.framesWritten(); }

signals:
    // Кадр принят и разобран.
    void packetReceived(const TelemetryPacket &packet, quint16 seq);
    // Кадр пришёл, но разобрать не удалось: текст для журнала пакетов.
    void frameRejected(const QString &reason, const QHostAddress &from);
    // Обнаружен пропуск в счётчике последовательности.
    void framesLost(int count);
    // Любое изменение счётчиков.
    void statsChanged();

private slots:
    void onReadyRead();

private:
    void handleDatagram(const QByteArray &frame, const QHostAddress &from);
    void accountSequence(quint16 seq);

    QUdpSocket   *socket_ = nullptr;
    RawLogWriter  log_;
    LinkStats     stats_;
    quint16       port_    = 0;
    bool          haveSeq_ = false;
    quint16       lastSeq_ = 0;
};

#endif // UDPLINK_H
