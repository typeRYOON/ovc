#pragma once
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QVariant>

namespace ovc::utils {

// Fire-and-forget property animation (deletes itself when stopped).
inline QPropertyAnimation* propertyAnimate(QObject* object, const QByteArray& property,
                                           const QVariant& startValue, const QVariant& endValue,
                                           qint32 durationMs,
                                           QEasingCurve curve = QEasingCurve::Linear)
{
    auto* a = new QPropertyAnimation(object, property, object);
    a->setDuration(durationMs);
    a->setEasingCurve(curve);
    a->setStartValue(startValue);
    a->setEndValue(endValue);
    a->start(QAbstractAnimation::DeleteWhenStopped);
    return a;
}

} // namespace ovc::utils
