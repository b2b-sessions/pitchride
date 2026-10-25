#include "b2b/b2blinks.h"

#include <QTimer>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QtDebug>
#include <cstring>

#include "util/cmdlineargs.h"

namespace mixxx::b2b {

namespace {
const char kScheme[] = "pitchride";
QStringList s_pending;

// One per user and settings folder (two test copies with their own
// settings each get their own).
QString socketName() {
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty()) {
        user = qEnvironmentVariable("USER");
    }
    const QByteArray id = (user + QLatin1Char('|') + CmdlineArgs::Instance().getSettingsPath()).toUtf8();
    return QStringLiteral("pitchride-links-") +
            QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha1).toHex().left(12));
}

bool isLink(const char* arg) {
    return arg && std::strncmp(arg, "pitchride:", 10) == 0;
}

// What starts this app again the way it was started: its program, and the
// folders it was told to use.
QStringList launchArgs() {
    QStringList args;
    const CmdlineArgs& cmd = CmdlineArgs::Instance();
    if (!cmd.getResourcePath().isEmpty()) {
        args << QStringLiteral("--resourcePath") << QDir(cmd.getResourcePath()).absolutePath();
    }
    if (cmd.getSettingsPathSet()) {
        args << QStringLiteral("--settingsPath") << QDir(cmd.getSettingsPath()).absolutePath();
    }
    return args;
}
} // namespace

namespace links {

QStringList takeFromArgs(int* pArgc, char** argv) {
    QStringList found;
    int out = 1;
    for (int i = 1; i < *pArgc; ++i) {
        if (isLink(argv[i])) {
            found << QString::fromLocal8Bit(argv[i]);
        } else {
            argv[out++] = argv[i];
        }
    }
    *pArgc = out;
    argv[out] = nullptr;
    return found;
}

bool forwardToRunningApp(const QStringList& links) {
    QLocalSocket socket;
    socket.connectToServer(socketName());
    if (!socket.waitForConnected(800)) {
        return false;
    }
    for (const QString& link : links) {
        socket.write(link.toUtf8() + '\n');
    }
    socket.flush();
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    qInfo() << "B2B: passed" << links.size() << "link(s) to the app that's already open";
    return true;
}

void setPending(const QStringList& links) {
    s_pending = links;
}

QStringList takePending() {
    QStringList out;
    out.swap(s_pending);
    return out;
}

void registerScheme() {
    const QString program = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QStringList args = launchArgs();
#if defined(Q_OS_WIN)
    QString command = QLatin1Char('"') + program + QLatin1Char('"');
    for (const QString& a : args) {
        command += QStringLiteral(" \"") + QDir::toNativeSeparators(a) + QLatin1Char('"');
    }
    command += QStringLiteral(" \"%1\"");
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\") + QLatin1String(kScheme),
            QSettings::NativeFormat);
    reg.setValue(QStringLiteral("Default"), QStringLiteral("URL:PitchRide"));
    reg.setValue(QStringLiteral("URL Protocol"), QString());
    reg.setValue(QStringLiteral("shell/open/command/Default"), command);
    reg.sync();
#elif defined(Q_OS_LINUX)
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    if (dir.isEmpty()) {
        return;
    }
    QDir().mkpath(dir);
    auto quote = [](QString s) -> QString {
        s.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        return QLatin1Char('"') + s + QLatin1Char('"');
    };
    QString exec = quote(program);
    for (const QString& a : args) {
        exec += QLatin1Char(' ') + quote(a);
    }
    exec += QStringLiteral(" %u");
    const QString name = QStringLiteral("pitchride-link.desktop");
    const QString contents = QStringLiteral(
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=PitchRide\n"
            "Comment=Opens PitchRide links (Go live from the website)\n"
            "Exec=%1\n"
            "NoDisplay=true\n"
            "Terminal=false\n"
            "MimeType=x-scheme-handler/pitchride;\n")
                                     .arg(exec);
    QFile file(QDir(dir).filePath(name));
    if (file.open(QIODevice::ReadOnly) && file.readAll() == contents.toUtf8()) {
        return; // already registered, as now
    }
    file.close();
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(contents.toUtf8());
        file.close();
        QProcess::startDetached(QStringLiteral("xdg-mime"),
                {QStringLiteral("default"), name, QStringLiteral("x-scheme-handler/pitchride")});
        QProcess::startDetached(QStringLiteral("update-desktop-database"), {dir});
        qInfo() << "B2B: registered pitchride:// links:" << exec;
    }
#else
    Q_UNUSED(program);
    Q_UNUSED(args);
#endif
}

} // namespace links

LinkServer::LinkServer(QObject* pParent)
        : QObject(pParent) {
    const QString name = socketName();
    m_pServer = new QLocalServer(this);
    m_pServer->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_pServer->listen(name)) {
        // A stale socket file from a crash (not on Windows): take it over.
        QLocalServer::removeServer(name);
        if (!m_pServer->listen(name)) {
            qWarning() << "B2B: can't take pitchride:// links:" << m_pServer->errorString();
            return;
        }
    }
    connect(m_pServer, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* pSocket = m_pServer->nextPendingConnection()) {
            connect(pSocket, &QLocalSocket::readyRead, this, [this, pSocket] {
                QStringList links;
                while (pSocket->canReadLine()) {
                    const QString link = QString::fromUtf8(pSocket->readLine()).trimmed();
                    if (link.startsWith(QLatin1String(kScheme) + QLatin1Char(':'))) {
                        links.append(link);
                    }
                }
                // Handled after this returns: opening a link asks first (a
                // dialog with its own event loop), and meanwhile the other
                // copy of the app hangs up and the socket goes.
                for (const QString& link : std::as_const(links)) {
                    QTimer::singleShot(0, this, [this, link] {
                        emit linkReceived(link);
                    });
                }
            });
            connect(pSocket, &QLocalSocket::disconnected, pSocket, &QObject::deleteLater);
        }
    });
}

LinkServer::~LinkServer() = default;

} // namespace mixxx::b2b

#include "moc_b2blinks.cpp"
