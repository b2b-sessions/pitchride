#include "b2b/b2bupdater.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QtGlobal>

#include "b2b/b2bsite.h"
#include "b2b/b2bupdates.h"
#include "moc_b2bupdater.cpp"

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace mixxx::b2b {

namespace {
constexpr int kRetryMs = 30 * 60 * 1000;  // a failed background download: again later
constexpr int kQuietMs = 60 * 1000;       // no data for this long: the download gave up
#ifdef Q_OS_WIN
const char* const kScript = "pitchride-update.cmd";
#else
const char* const kScript = "pitchride-update.sh";
#endif

/// A version we'd put in a file name: digits and dots only.
bool plainVersion(const QString& version) {
    if (version.isEmpty() || version.size() > 20) {
        return false;
    }
    for (const QChar c : version) {
        if (!c.isDigit() && c != QLatin1Char('.')) {
            return false;
        }
    }
    return true;
}

/// The link the site handed us goes to the installers' bucket (or, when
/// testing against a local site, to that site).
bool trustedLink(const QUrl& link, const QUrl& site) {
    if (!link.isValid() || !link.userInfo().isEmpty()) {
        return false;
    }
    const QString host = link.host().toLower();
    if (link.scheme() == QLatin1String("https") && host.endsWith(QLatin1String(".r2.cloudflarestorage.com"))) {
        return true;
    }
    return link.scheme() == site.scheme() && host == site.host().toLower() && link.port() == site.port();
}

/// The sha256 of a file on disk, as 64 lowercase hex characters ("" if it
/// can't be read).
QString sha256OfFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return QString();
    }
    return QString::fromLatin1(hash.result().toHex());
}

/// In a batch file, a % is written %%.
QString batchText(QString text) {
    return text.replace(QLatin1Char('%'), QStringLiteral("%%"));
}

/// The script that installs the new version: it waits for this app (pid)
/// to close, runs the installer next to it, then opens PitchRide again
/// (where it was, or where the installer puts it) and deletes itself.
QString windowsScript(qint64 pid, const QString& installer, const QString& app) {
    const QString exe = QFileInfo(app).fileName();
    const QString p = QString::number(pid);
    QStringList lines{
            QStringLiteral("@echo off"),
            QStringLiteral("chcp 65001 >nul"),
            QStringLiteral("rem PitchRide's updater: waits for the app to close, installs the new"),
            QStringLiteral("rem version, opens PitchRide again, then deletes itself."),
            // (Windows' own tools, by their full path: a "find" from elsewhere can't stand in.)
            QStringLiteral("set w=%SystemRoot%\\System32"),
            QStringLiteral("set /a n=0"),
            QStringLiteral(":wait"),
            QStringLiteral("\"%w%\\tasklist.exe\" /fi \"PID eq %1\" /nh 2>nul | \"%w%\\find.exe\" \" %1 \" >nul || goto install")
                    .arg(p),
            QStringLiteral("set /a n+=1"),
            QStringLiteral("if %n% geq 120 goto install"),
            // (timeout needs a console; ping waits about a second without one)
            QStringLiteral("\"%w%\\ping.exe\" -n 2 127.0.0.1 >nul"),
            QStringLiteral("goto wait"),
            QStringLiteral(":install"),
            QStringLiteral("start \"\" /wait \"%w%\\msiexec.exe\" /i \"%~dp0%1\" /passive /norestart")
                    .arg(batchText(installer)),
            QStringLiteral("if exist \"%1\" (start \"\" \"%1\") else (start \"\" \"%ProgramFiles%\\PitchRide\\%2\")")
                    .arg(batchText(QDir::toNativeSeparators(app)), batchText(exe)),
            QStringLiteral("(goto) 2>nul & del \"%~f0\""),
    };
    return lines.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
}
#ifdef Q_OS_MACOS
/// The Mac app this copy runs from (.../PitchRide.app), or "" if it isn't
/// one we can replace: not in an app bundle, or macOS is running it from a
/// read-only copy because it was opened straight from Downloads
/// ("App Translocation") instead of from Applications.
QString macAppBundle() {
    QDir dir(QCoreApplication::applicationDirPath()); // .../PitchRide.app/Contents/MacOS
    if (!dir.cdUp() || !dir.cdUp() || !dir.dirName().endsWith(QLatin1String(".app"))) {
        return QString();
    }
    const QString path = dir.absolutePath();
    if (path.contains(QLatin1String("/AppTranslocation/"))) {
        return QString();
    }
    return path;
}

/// The script that installs the new version on a Mac. Run as
///   pitchride-update.sh <pid> <installer.dmg> <PitchRide.app>
/// it waits for this app (pid) to close, opens the disk image, checks the
/// app inside is signed by the same developer as the one it replaces (and
/// that Apple's check passes), swaps it in, and opens PitchRide again. If
/// anything doesn't work, the old app stays where it was and the disk image
/// is opened in Finder instead, to drag across by hand. Paths come in as
/// arguments, so nothing in them is ever read as part of the script.
QString macScript() {
    return QStringLiteral(R"SH(#!/bin/bash
# PitchRide's updater: waits for the app to close, puts the new version in
# its place, opens PitchRide again, then deletes itself.
PID="$1"; DMG="$2"; APP="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
exec >>"$HERE/update.log" 2>&1
echo "== $(date) updating $APP from $DMG"
PATH=/usr/bin:/bin:/usr/sbin:/sbin
by_hand() {
    echo "couldn't update it here: $1. opening the installer to drag across instead."
    [ -n "$MNT" ] && hdiutil detach -quiet "$MNT"
    open "$DMG"
    rm -f "$0"
    exit 1
}
n=0
while kill -0 "$PID" 2>/dev/null && [ "$n" -lt 120 ]; do sleep 1; n=$((n + 1)); done
MNT="$(mktemp -d "${TMPDIR:-/tmp}/pitchride-update.XXXXXX")" || by_hand "no place to open it"
yes | PAGER=cat hdiutil attach -nobrowse -noautoopen -readonly -mountpoint "$MNT" "$DMG" >/dev/null || { rmdir "$MNT"; MNT=""; by_hand "the installer wouldn't open"; }
NEW="$(find "$MNT" -maxdepth 1 -name '*.app' -type d | head -1)"
[ -n "$NEW" ] || by_hand "no app in the installer"
# Signed by the same developer as the app it replaces, and Apple's check passes.
TEAM="$(codesign -dv "$APP" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
NEWTEAM="$(codesign -dv "$NEW" 2>&1 | sed -n 's/^TeamIdentifier=//p')"
{ [ -n "$TEAM" ] && [ "$TEAM" != "not set" ] && [ "$TEAM" = "$NEWTEAM" ]; } || by_hand "it isn't signed by the same developer ($NEWTEAM, not $TEAM)"
codesign --verify --deep --strict "$NEW" || by_hand "its signature didn't check out"
spctl --assess --type execute "$NEW" || by_hand "macOS didn't accept it"
PARENT="$(dirname "$APP")"
[ -w "$PARENT" ] || by_hand "$PARENT can't be written to by this account"
STAGE="$PARENT/.pitchride-update-new.app"; OLD="$PARENT/.pitchride-update-old.app"
rm -rf "$STAGE" "$OLD"
ditto "$NEW" "$STAGE" || { rm -rf "$STAGE"; by_hand "copying it failed"; }
hdiutil detach -quiet "$MNT"; rmdir "$MNT" 2>/dev/null; MNT=""
mv "$APP" "$OLD" || { rm -rf "$STAGE"; by_hand "the old version couldn't be moved"; }
if ! mv "$STAGE" "$APP"; then mv "$OLD" "$APP"; rm -rf "$STAGE"; by_hand "the new version couldn't be put in place"; fi
rm -rf "$OLD"
echo "updated."
open "$APP"
rm -f "$DMG" "$0"
)SH");
}
#endif // Q_OS_MACOS
} // namespace

Updater::Updater(Site* pSite, UpdateCheck* pCheck, QObject* pParent)
        : QObject(pParent),
          m_pSite(pSite),
          m_pCheck(pCheck),
          m_pNet(new QNetworkAccessManager(this)),
          m_hash(QCryptographicHash::Sha256) {
    m_retry.setSingleShot(true);
    m_retry.setInterval(kRetryMs);
    connect(&m_retry, &QTimer::timeout, this, &Updater::get);
    connect(m_pCheck, &UpdateCheck::changed, this, &Updater::checkChanged);
    // Signing in (or being let in) can make a download possible.
    connect(m_pSite, &Site::signedInChanged, this, &Updater::checkChanged);
    connect(m_pSite, &Site::meChanged, this, &Updater::checkChanged);
}

Updater::~Updater() {
    cancel();
}

bool Updater::supported() {
#if defined(Q_OS_WIN)
    return true;
#elif defined(Q_OS_MACOS)
    // An app in a folder we can swap it in (not run straight from the
    // downloaded disk image or Downloads: macOS runs those read-only).
    return !macAppBundle().isEmpty();
#else
    return qEnvironmentVariableIsSet("B2B_UPDATER_TEST"); // (testing on Linux)
#endif
}

bool Updater::canGet() const {
    return supported() && m_pSite->canPlay() && !m_pCheck->newerVersion().isEmpty() &&
            plainVersion(m_pCheck->newerVersion()) && m_pCheck->newerSize() > 0 &&
            !m_pCheck->newerSha256().isEmpty();
}

QString Updater::folder() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/pitchride-update");
}

QString Updater::fileFor(const QString& version) {
    if (!plainVersion(version)) {
        return QString();
    }
#if defined(Q_OS_WIN)
    return QStringLiteral("pitchride-%1.msi").arg(version);
#elif defined(Q_OS_MACOS)
    return QStringLiteral("pitchride-%1.dmg").arg(version);
#else
    return QStringLiteral("pitchride-%1.installer").arg(version); // (testing)
#endif
}

void Updater::checkChanged() {
    const QString newer = m_pCheck->newerVersion();
    if (newer.isEmpty()) {
        if (!m_pCheck->checked()) {
            return; // not known yet
        }
        // Up to date (just updated, perhaps): nothing to keep.
        cancel();
        m_version.clear();
        setState(State::Idle);
        if (supported()) {
            tidy(QString());
        }
        return;
    }
    if (newer != m_version || m_sha256 != m_pCheck->newerSha256()) {
        // Another version than the one we have or are getting: start again.
        cancel();
        m_version = newer;
        m_sha256 = m_pCheck->newerSha256();
        m_size = m_pCheck->newerSize();
        setState(State::Idle);
    }
    if (m_state == State::Idle && canGet()) {
        get();
    }
}

void Updater::get() {
    m_retry.stop();
    if (!canGet() || m_state == State::Getting || m_state == State::Ready) {
        return;
    }
    m_version = m_pCheck->newerVersion();
    m_sha256 = m_pCheck->newerSha256();
    m_size = m_pCheck->newerSize();
    m_problem.clear();
    if (alreadyHere()) {
        qInfo() << "B2B: PitchRide" << m_version << "is downloaded and checked";
        tidy(fileFor(m_version));
        setState(State::Ready);
        return;
    }
    m_percent = 0;
    setState(State::Getting);
    const QString wanted = m_version;
    m_pSite->downloadLink(UpdateCheck::platform(), [this, wanted](const QJsonObject& reply, const QString& error) {
        if (m_state != State::Getting || m_version != wanted) {
            return; // something newer came along meanwhile
        }
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        gotLink(reply.value(QStringLiteral("url")).toString(), reply.value(QStringLiteral("version")).toString());
    });
}

void Updater::gotLink(const QString& url, const QString& version) {
    const QUrl link(url);
    if (!trustedLink(link, m_pSite->url())) {
        qWarning() << "B2B: the update link isn't one of ours:" << link.host();
        fail(tr("the download link looked wrong, so it wasn't used."));
        return;
    }
    if (version != m_version) {
        // The site and latest.json disagree for a moment (a new version
        // going up): try again later rather than check against the wrong one.
        fail(tr("a new version is going up right now. try again in a few minutes."));
        return;
    }
    if (!QDir().mkpath(folder())) {
        fail(tr("couldn't make a place to download it."));
        return;
    }
    tidy(fileFor(m_version));
    m_part.setFileName(folder() + QLatin1Char('/') + fileFor(m_version) + QStringLiteral(".part"));
    if (!m_part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail(tr("couldn't save the download."));
        return;
    }
    m_hash.reset();
    m_got = 0;
    QNetworkRequest request(link);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(kQuietMs);
    m_pReply = m_pNet->get(request);
    connect(m_pReply, &QNetworkReply::readyRead, this, &Updater::readSome);
    connect(m_pReply, &QNetworkReply::finished, this, &Updater::finished);
    qInfo() << "B2B: getting PitchRide" << m_version;
}

void Updater::readSome() {
    if (!m_pReply) {
        return;
    }
    const QByteArray data = m_pReply->readAll();
    m_got += data.size();
    if (m_got > m_size) {
        fail(tr("the download was bigger than it should be, so it was thrown away."));
        return;
    }
    if (m_part.write(data) != data.size()) {
        fail(tr("couldn't save the download. is the disk full?"));
        return;
    }
    m_hash.addData(data);
    const int percent = static_cast<int>(m_got * 100 / m_size);
    if (percent != m_percent) {
        m_percent = percent;
        emit changed();
    }
}

void Updater::finished() {
    if (!m_pReply) {
        return;
    }
    readSome();
    if (!m_pReply) {
        return; // (readSome failed it)
    }
    QNetworkReply* pReply = m_pReply;
    m_pReply.clear();
    pReply->deleteLater();
    if (pReply->error() != QNetworkReply::NoError) {
        qWarning() << "B2B: the update download stopped:" << pReply->errorString();
        fail(tr("the download stopped. check your internet connection."));
        return;
    }
    m_part.close();
    const QString sha = QString::fromLatin1(m_hash.result().toHex());
    if (m_got != m_size || sha != m_sha256) {
        qWarning() << "B2B: the update didn't match latest.json:" << m_got << "bytes, sha256" << sha;
        m_part.remove();
        fail(tr("the download didn't match the new version, so it was thrown away."));
        return;
    }
    const QString path = folder() + QLatin1Char('/') + fileFor(m_version);
    QFile::remove(path);
    if (!m_part.rename(path)) {
        m_part.remove();
        fail(tr("couldn't save the download."));
        return;
    }
    qInfo() << "B2B: PitchRide" << m_version << "is downloaded and checked";
    setState(State::Ready);
}

bool Updater::alreadyHere() {
    const QString name = fileFor(m_version);
    if (name.isEmpty()) {
        return false;
    }
    const QString path = folder() + QLatin1Char('/') + name;
    const QFileInfo info(path);
    return info.isFile() && info.size() == m_size && sha256OfFile(path) == m_sha256;
}

void Updater::fail(const QString& problem) {
    cancel();
    m_problem = problem;
    qWarning() << "B2B: the update didn't work:" << problem;
    setState(State::Failed);
    m_retry.start(); // quietly, later (the website still works meanwhile)
}

void Updater::cancel() {
    m_retry.stop();
    if (m_pReply) {
        QNetworkReply* pReply = m_pReply;
        m_pReply.clear();
        pReply->disconnect(this);
        pReply->abort();
        pReply->deleteLater();
    }
    if (m_part.isOpen()) {
        m_part.close();
        m_part.remove();
    }
}

void Updater::setState(State state) {
    if (state != State::Getting) {
        m_percent = 0;
    }
    m_state = state;
    emit changed();
}

void Updater::tidy(const QString& keep) {
    QDir dir(folder());
    if (!dir.exists()) {
        return;
    }
    const QStringList old = dir.entryList({QStringLiteral("pitchride-*")}, QDir::Files);
    for (const QString& name : old) {
        if (name != keep) {
            dir.remove(name);
        }
    }
}

bool Updater::install() {
    if (m_state != State::Ready || !alreadyHere()) {
        return false;
    }
    const QString script = folder() + QLatin1Char('/') + QLatin1String(kScript);
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
#ifdef Q_OS_MACOS
    file.write(macScript().toUtf8());
#else
    file.write(windowsScript(QCoreApplication::applicationPid(), fileFor(m_version),
            QCoreApplication::applicationFilePath())
                       .toUtf8());
#endif
    file.close();
#if defined(Q_OS_WIN)
    QProcess process;
    process.setProgram(QStringLiteral("cmd.exe"));
    process.setArguments({QStringLiteral("/d"), QStringLiteral("/c"), QDir::toNativeSeparators(script)});
    process.setWorkingDirectory(folder());
    // No black window: the installer shows its own progress.
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* pArgs) {
        pArgs->flags |= CREATE_NO_WINDOW;
    });
    if (!process.startDetached()) {
        qWarning() << "B2B: couldn't start the updater";
        return false;
    }
    qInfo() << "B2B: installing PitchRide" << m_version << "once the app has closed";
    return true;
#elif defined(Q_OS_MACOS)
    const QString app = macAppBundle();
    if (app.isEmpty()) {
        return false;
    }
    if (!QProcess::startDetached(QStringLiteral("/bin/bash"),
                {script,
                        QString::number(QCoreApplication::applicationPid()),
                        folder() + QLatin1Char('/') + fileFor(m_version),
                        app},
                folder())) {
        qWarning() << "B2B: couldn't start the updater";
        return false;
    }
    qInfo() << "B2B: installing PitchRide" << m_version << "once the app has closed";
    return true;
#else
    // (Testing: the script is written, nothing runs.)
    qInfo() << "B2B: would install PitchRide" << m_version << "with" << script;
    return true;
#endif
}

} // namespace mixxx::b2b
