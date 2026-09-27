#ifndef LIMITCHECK_H
#define LIMITCHECK_H

#include "telemetry.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <limits>

// ---------------------------------------------------------------------------
//  Границы нормы (limit checking).
//
//  Так работают настоящие пульты: у каждого параметра заданы два коридора —
//  предупредительный и аварийный. Выход за первый означает «обрати внимание»,
//  за второй — «борт в опасности, нужно действие».
//
//      critLow      warnLow                 warnHigh      critHigh
//   ─────┼─────────────┼──────── норма ────────┼─────────────┼─────
//   АВАРИЯ  ПРЕДУПРЕЖД.                    ПРЕДУПРЕЖД.   АВАРИЯ
//
//  Границы заданы ОДНОЙ таблицей в limits.cpp, а не условиями по коду. Это
//  важно: в реальных системах (SCOS-2000, Yamcs) пределы описываются данными,
//  и при смене режима полёта правят таблицу, а не исходники. Наша таблица —
//  упрощённый аналог того, что в XTCE задаётся элементом <AlarmConditions>.
// ---------------------------------------------------------------------------

enum class LimitState {
    NoLimits, // для параметра границы не заданы
    Nominal,  // в норме
    Warning,  // вне предупредительного коридора
    Critical  // вне аварийного коридора
};

struct ParameterLimits
{
    QString name; // как показывать оператору
    QString unit;
    double  criticalLow;
    double  warningLow;
    double  warningHigh;
    double  criticalHigh;

    LimitState evaluate(double value) const;
};

// Значение одного параметра, выдернутое из пакета.
struct ParameterSample
{
    QString id;
    double  value;
};

namespace limits {

// Границу можно не задавать — тогда с этой стороны параметр не проверяется.
constexpr double kNoLow  = -std::numeric_limits<double>::infinity();
constexpr double kNoHigh = std::numeric_limits<double>::infinity();

const QHash<QString, ParameterLimits> &table();
const ParameterLimits *find(const QString &id);
LimitState check(const QString &id, double value);

// Разворачивает пакет в плоский список «идентификатор — значение».
// Кроме полей пакета сюда попадают производные параметры (максимум по
// массиву, норма кватерниона) — их тоже проверяют в реальных системах.
QVector<ParameterSample> sample(const TelemetryPacket &packet);

QString    stateText(LimitState state);
LimitState worse(LimitState a, LimitState b);

} // namespace limits

#endif // LIMITCHECK_H
