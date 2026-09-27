#include "udplink.h"

#include <QDateTime>
#include <QNetworkDatagram>
#include <QUdpSocket>

UdpTelemetryLink::UdpTelemetryLink(QObject *parent)
    : QObject(parent)
    , socket_(new QUdpSocket(this))
{
    connect(socket_, &QUdpSocket::readyRead, this, &UdpTelemetryLink::onReadyRead);
}

UdpTelemetryLink::~UdpTelemetryLink() = default;

bool UdpTelemetryLink::open(quint16 port, QString *error)
{
    close();

    if (!socket_->bind(QHostAddress::Any, port, QUdpSocket::ShareAddress)) {
        if (error) *error = socket_->errorString();
        return false;
    }

    port_    = port;
    haveSeq_ = false;
    lastSeq_ = 0;
    return true;
}

void UdpTelemetryLink::close()
{
    if (socket_->state() != QAbstractSocket::UnconnectedState)
        socket_->close();
    stopLog();
    port_    = 0;
    haveSeq_ = false;
}

bool UdpTelemetryLink::isOpen() const
{
    return socket_->state() == QAbstractSocket::BoundState;
}

void UdpTelemetryLink::resetStats()
{
    stats_.reset();
    haveSeq_ = false;
    emit statsChanged();
}

bool UdpTelemetryLink::startLog(const QString &path, QString *error)
{
    return log_.open(path, error);
}

void UdpTelemetryLink::stopLog()
{
    log_.close();
}

void UdpTelemetryLink::onReadyRead()
{
    while (socket_->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = socket_->receiveDatagram();
        if (!datagram.isValid())
            continue;
        handleDatagram(datagram.data(), datagram.senderAddress());
    }
}

void UdpTelemetryLink::handleDatagram(const QByteArray &frame, const QHostAddress &from)
{
    // Сырой журнал — первым делом, до любых проверок.
    if (log_.isOpen())
        log_.write(frame, QDateTime::currentMSecsSinceEpoch());

    const protocol::DecodeResult result = protocol::decode(frame);

    if (!result.isOk()) {
        if (result.status == protocol::Status::CrcError) {
            ++stats_.crcErrors;
            // Счётчик из битого кадра ненадёжен, но заголовок обычно цел,
            // поэтому пропуск по последовательности всё равно учитываем —
            // иначе один битый кадр потом посчитается как потерянный.
            accountSequence(result.seq);
        } else {
            ++stats_.malformed;
        }
        emit frameRejected(protocol::statusText(result.status), from);
        emit statsChanged();
        return;
    }

    ++stats_.received;
    accountSequence(result.seq);
    emit packetReceived(result.packet, result.seq);
    emit statsChanged();
}

void UdpTelemetryLink::accountSequence(quint16 seq)
{
    if (!haveSeq_) {
        haveSeq_ = true;
        lastSeq_ = seq;
        return;
    }

    const quint16 expected = static_cast<quint16>((lastSeq_ + 1) & protocol::kSeqCountMask);
    const quint16 gap = static_cast<quint16>((seq - expected) & protocol::kSeqCountMask);

    // Половина диапазона трактуется как «пришло с опозданием, не потеря»:
    // UDP не гарантирует порядок, и переставленные кадры считать пропавшими
    // неправильно.
    if (gap > 0 && gap < (protocol::kSeqCountMask / 2)) {
        stats_.lost += gap;
        emit framesLost(static_cast<int>(gap));
    }

    if (gap < (protocol::kSeqCountMask / 2))
        lastSeq_ = seq;
}
