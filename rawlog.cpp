#include "rawlog.h"

#include <QDir>
#include <QFileInfo>

namespace {

void appendBe(QByteArray &out, quint32 value)
{
    out.append(static_cast<char>(value >> 24));
    out.append(static_cast<char>(value >> 16));
    out.append(static_cast<char>(value >> 8));
    out.append(static_cast<char>(value));
}

void appendBe(QByteArray &out, quint64 value)
{
    appendBe(out, static_cast<quint32>(value >> 32));
    appendBe(out, static_cast<quint32>(value & 0xFFFFFFFFu));
}

} // namespace

RawLogWriter::~RawLogWriter()
{
    close();
}

bool RawLogWriter::open(const QString &path, QString *error)
{
    close();

    const QDir dir = QFileInfo(path).absoluteDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error) *error = QStringLiteral("не удалось создать каталог %1").arg(dir.absolutePath());
        return false;
    }

    file_.setFileName(path);
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = file_.errorString();
        return false;
    }

    QByteArray header;
    header.append("CSTL", 4);
    header.append(static_cast<char>(rawlog::kVersion >> 8));
    header.append(static_cast<char>(rawlog::kVersion & 0xFF));
    header.append('\0');
    header.append('\0');

    if (file_.write(header) != header.size()) {
        if (error) *error = file_.errorString();
        file_.close();
        return false;
    }

    framesWritten_ = 0;
    return true;
}

void RawLogWriter::close()
{
    if (file_.isOpen()) {
        file_.flush();
        file_.close();
    }
    framesWritten_ = 0;
}

bool RawLogWriter::write(const QByteArray &frame, qint64 recvTimeMs)
{
    if (!file_.isOpen())
        return false;

    QByteArray record;
    record.reserve(12 + frame.size());
    appendBe(record, static_cast<quint64>(recvTimeMs));
    appendBe(record, static_cast<quint32>(frame.size()));
    record.append(frame);

    if (file_.write(record) != record.size())
        return false;

    // Сбрасываем на диск сразу: если приложение упадёт посреди пролёта,
    // принятое до падения должно остаться в файле.
    file_.flush();
    ++framesWritten_;
    return true;
}
