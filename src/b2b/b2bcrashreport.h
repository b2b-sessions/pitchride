#pragma once

#include <QString>
#include <QStringList>

namespace mixxx::b2b {

/// If the app crashes, write what it was doing (the crashing thread's call
/// stack, and on Windows a minidump of every thread) to
/// <settings>/crash-reports/, so the next start can offer to send it.
/// Call once, as early as possible.
void installCrashHandler(const QString& settingsPath);

/// The folder crash reports go to.
QString crashReportFolder(const QString& settingsPath);

/// Crash reports written since the last call to markCrashReportsSeen()
/// (newest last): the .txt files.
QStringList unseenCrashReports(const QString& settingsPath);
void markCrashReportsSeen(const QString& settingsPath);

} // namespace mixxx::b2b
