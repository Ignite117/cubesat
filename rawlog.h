#ifndef RAWLOG_H
#define RAWLOG_H

#include <QByteArray>
#include <QFile>
#include <QString>

// ---------------------------------------------------------------------------
//  Журнал сырых кадров.
//
//  В наземных комплексах принято писать принятое на диск ДО разбора и проверок:
//  битый кадр тоже данные, по нему видно, что происходило в канале, и его
//  всегда можно переразобрать новой версией парсера. Поэтому сюда попадает
//  каждая принятая датаграмма, включая те, что не прошли CRC.
//
//  Формат файла (big-endian):
//
//    Заголовок файла — 8 байт
//      magic  "CSTL"                   4 байта
//      version = 1                     2 байта
//      reserved = 0                    2 байта
//
//    Далее записи подряд, каждая:
//      recv_time_ms (Unix-время приёма) 8 байт
//      frame_len                        4 байта
//      frame                            frame_len байт
//
//  Длина у каждой записи своя, потому что в файле нет границ датаграмм —
//  в отличие от UDP, где границу держит транспорт.
//
//  Прочитать и переиграть такой файл умеет sim/sat_sim.py --replay.
// ---------------------------------------------------------------------------

namespace rawlog {

constexpr quint16 kVersion    = 1;
constexpr int     kHeaderSize = 8;

} // namespace rawlog

class RawLogWriter
{
public:
    RawLogWriter() = default;
    ~RawLogWriter();

    // Открывает новый файл журнала. Существующий файл перезаписывается.
    bool open(const QString &path, QString *error = nullptr);
    void close();

    bool isOpen() const { return file_.isOpen(); }
    QString path() const { return file_.fileName(); }
    quint64 framesWritten() const { return framesWritten_; }
    qint64  bytesWritten() const { return file_.isOpen() ? file_.size() : 0; }

    // Записывает кадр с отметкой времени приёма.
    bool write(const QByteArray &frame, qint64 recvTimeMs);

private:
    QFile   file_;
    quint64 framesWritten_ = 0;
};

#endif // RAWLOG_H
