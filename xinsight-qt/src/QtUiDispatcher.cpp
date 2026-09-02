#include "QtUiDispatcher.h"

#include <QCoreApplication>
#include <QMetaObject>

void QtUiDispatcher::post(std::function<void()> fn) {
    QMetaObject::invokeMethod(QCoreApplication::instance(), std::move(fn), Qt::QueuedConnection);
}
