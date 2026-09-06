#pragma once

#include "common/Literals.hpp"

#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QTextStream>

using namespace chatterino::literals;

namespace chatterino {

/// Thread-safe file logger for GIF debugging.
/// Writes to D:\Programming\Chatterino\gif_debug.log
/// Truncates the file on first call (new session = fresh log).
inline void gifLog(const QString &msg)
{
    static QFile file(u"D:\\Programming\\Chatterino\\gif_debug.log"_s);
    static QMutex mutex;
    static bool firstCall = true;
    QMutexLocker lock(&mutex);

    if (!file.isOpen())
    {
        if (firstCall)
        {
            file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
            firstCall = false;
        }
        else
        {
            file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        }
    }

    QTextStream stream(&file);
    stream << QDateTime::currentDateTime().toString(
                  u"HH:mm:ss.zzz"_s)
           << u" "_s << msg << u"\n"_s;
    stream.flush();
}

}  // namespace chatterino
