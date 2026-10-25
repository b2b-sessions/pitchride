#include "b2b/b2bsite.h"

#include <QDesktopServices>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

#include "b2b/version.hpp"
#include "moc_b2bsite.cpp"

namespace mixxx::b2b {

namespace {
const QString kSite = QStringLiteral("https://pitchride.co.uk");
constexpr int kRoomsEveryMs = 15000;
constexpr int kSignInWaitMs = 10 * 60 * 1000;
constexpr int kHelloEveryMs = 60 * 1000;
constexpr qint64 kMaxRequestLine = 8192; // the browser's "GET /done?... HTTP/1.1"

// Compared in constant time (a local program could otherwise time the
// answers to learn the state a character at a time).
bool sameText(const QString& a, const QString& b) {
    const QByteArray x = a.toUtf8();
    const QByteArray y = b.toUtf8();
    if (x.size() != y.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (int i = 0; i < x.size(); ++i) {
        diff |= static_cast<unsigned char>(x[i] ^ y[i]);
    }
    return diff == 0;
}
constexpr quint16 kLoopbackPort = 47823; // tried first (then the next few)

QString errorText(QNetworkReply* pReply, const QByteArray& body) {
    const int status = pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        return QObject::tr("can't reach pitchride. check your internet connection.");
    }
    QString text = QString::fromUtf8(body).trimmed();
    // Shown in message boxes, which would render HTML: none of that.
    text.remove(QLatin1Char('<')).remove(QLatin1Char('>'));
    if (!text.isEmpty() && text.size() < 200 && !QString::fromUtf8(body).trimmed().startsWith(QLatin1Char('<'))) {
        return text.left(1).toUpper() + text.mid(1);
    }
    return QObject::tr("pitchride said no (%1).").arg(status);
}

const char* const kDonePage =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
        "<!doctype html><html><head><meta charset=utf-8><title>PitchRide</title>"
        "<style>body{background:#0b0b0d;color:#f5f4f0;font:16px system-ui,sans-serif;display:grid;"
        "place-items:center;height:100vh;margin:0}h1{font-size:22px;margin:0 0 8px}"
        "p{color:#8f949e;margin:0}</style></head><body><div><h1>You're signed in to the PitchRide app</h1>"
        "<p>You can close this tab and go back to the app.</p></div></body></html>";
} // namespace

Site::Site(const QString& settingsPath, QObject* pParent)
        : QObject(pParent),
          m_settingsFile(settingsPath + QStringLiteral("/b2b.ini")),
          m_pNet(new QNetworkAccessManager(this)) {
    m_token = QSettings(m_settingsFile, QSettings::IniFormat).value(QStringLiteral("site_token")).toString();
    m_roomsTimer.setInterval(kRoomsEveryMs);
    connect(&m_roomsTimer, &QTimer::timeout, this, &Site::refreshRooms);
    m_signInTimeout.setSingleShot(true);
    connect(&m_signInTimeout, &QTimer::timeout, this, &Site::cancelSignIn);
    m_helloTimer.setInterval(kHelloEveryMs);
    connect(&m_helloTimer, &QTimer::timeout, this, &Site::sayHello);
    m_helloTimer.start();
    if (signedIn()) {
        QTimer::singleShot(0, this, &Site::refreshMe);
    }
}

Site::~Site() = default;

QUrl Site::url(const QString& path) const {
    QString base = qEnvironmentVariable("B2B_WEB_URL", kSite).trimmed();
    while (base.endsWith(QLatin1Char('/'))) {
        base.chop(1);
    }
    return QUrl(base + path);
}

void Site::setToken(const QString& token) {
    m_token = token;
    QSettings settings(m_settingsFile, QSettings::IniFormat);
    if (token.isEmpty()) {
        settings.remove(QStringLiteral("site_token"));
    } else {
        settings.setValue(QStringLiteral("site_token"), token);
    }
    settings.sync();
    // The token signs this app in for months: the file is for this user only
    // (QSettings writes it with the default permissions, usually readable by all).
    QFile::setPermissions(m_settingsFile, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QNetworkReply* Site::request(const QByteArray& verb, const QString& path, const QJsonObject& body) {
    QNetworkRequest req(url(path));
    req.setRawHeader("Accept", "application/json");
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("PitchRide app"));
    if (!m_token.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    }
    req.setTransferTimeout(15000);
    if (verb == "GET") {
        return m_pNet->get(req);
    }
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // The site refuses cross-site writes: say where we come from.
    req.setRawHeader("Origin", url().toString().toUtf8());
    return m_pNet->sendCustomRequest(req, verb, QJsonDocument(body).toJson(QJsonDocument::Compact));
}

void Site::finish(QNetworkReply* pReply, Done done) {
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, done = std::move(done)] {
        pReply->deleteLater();
        const QByteArray body = pReply->readAll();
        const int status = pReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401 && signedIn()) {
            // The site forgot us (signed out elsewhere, or a long time ago).
            setToken(QString());
            m_me = QJsonObject();
            emit signedInChanged();
        }
        if (status >= 200 && status < 300) {
            done(QJsonDocument::fromJson(body).object(), QString());
        } else {
            done(QJsonObject(), errorText(pReply, body));
        }
    });
}

void Site::refreshMe() {
    if (!signedIn()) {
        return;
    }
    finish(request("GET", QStringLiteral("/api/me")), [this](const QJsonObject& reply, const QString& error) {
        if (!error.isEmpty()) {
            return;
        }
        const QJsonObject user = reply.value(QStringLiteral("user")).toObject();
        if (user.isEmpty()) {
            setToken(QString());
            m_me = QJsonObject();
            emit signedInChanged();
            return;
        }
        const bool first = m_me.isEmpty();
        m_me = user;
        m_admin = reply.value(QStringLiteral("admin")).toBool();
        if (first) {
            sayHello(); // (then once a minute)
        }
        // (A site from before invites doesn't say: then everyone may.)
        m_access = reply.value(QStringLiteral("access")).toBool(true);
        emit meChanged();
    });
}

void Site::signIn() {
    // The browser signs in, then goes to our loopback address with the
    // token. One address for as long as we wait, and the same one each time
    // where we can: a sign-in page left open from an earlier try (or the
    // app opened twice) still finds us.
    if (!m_pLoopback) {
        m_pLoopback = new QTcpServer(this);
        bool listening = false;
        for (quint16 port = kLoopbackPort; port < kLoopbackPort + 10 && !listening; ++port) {
            listening = m_pLoopback->listen(QHostAddress::LocalHost, port);
        }
        if (!listening && !m_pLoopback->listen(QHostAddress::LocalHost, 0)) {
            delete m_pLoopback;
            m_pLoopback = nullptr;
            emit signInProblem(tr("couldn't start signing in. try again."));
            return;
        }
        connect(m_pLoopback, &QTcpServer::newConnection, this, &Site::loopbackConnection);
        // A new random state for this listener: the site sends it back with
        // the token, and nothing without it is taken (another web page or
        // program can't slip us their own account's token).
        quint32 random[4];
        QRandomGenerator::system()->fillRange(random);
        m_signInState.clear();
        for (quint32 r : random) {
            m_signInState += QStringLiteral("%1").arg(r, 8, 16, QLatin1Char('0'));
        }
    }
    QUrl page = url(QStringLiteral("/auth/app"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("redirect"),
            QStringLiteral("http://127.0.0.1:%1/done?state=%2").arg(m_pLoopback->serverPort()).arg(m_signInState));
    page.setQuery(q);
    m_signInTimeout.start(kSignInWaitMs);
    if (!QDesktopServices::openUrl(page)) {
        cancelSignIn();
        emit signInProblem(tr("couldn't open your browser. go to %1 to sign in.").arg(url().host()));
        return;
    }
    emit signedInChanged(); // "signing in…"
}

void Site::cancelSignIn() {
    m_signInTimeout.stop();
    m_signInState.clear(); // nothing more is taken once we stop listening
    if (m_pLoopback) {
        m_pLoopback->close();
        m_pLoopback->deleteLater();
        m_pLoopback = nullptr;
        emit signedInChanged();
    }
}

void Site::loopbackConnection() {
    if (!m_pLoopback) {
        return;
    }
    while (QTcpSocket* pSocket = m_pLoopback->nextPendingConnection()) {
        // Ours, not the listener's: it goes once we're signed in, and the
        // browser must still get the page.
        pSocket->setParent(this);
        connect(pSocket, &QTcpSocket::disconnected, pSocket, &QObject::deleteLater);
        connect(pSocket, &QTcpSocket::readyRead, this, [this, pSocket] {
            if (!pSocket->canReadLine()) {
                if (pSocket->bytesAvailable() > kMaxRequestLine) {
                    pSocket->abort(); // no browser sends a line this long
                }
                return;
            }
            // "GET /done?state=...&token=... HTTP/1.1"
            const QList<QByteArray> parts = pSocket->readLine(kMaxRequestLine).split(' ');
            pSocket->readAll();
            const QUrl got(QStringLiteral("http://127.0.0.1") +
                    QString::fromUtf8(parts.value(1)));
            const QUrlQuery query(got);
            const QString token = query.queryItemValue(QStringLiteral("token"));
            const QString state = query.queryItemValue(QStringLiteral("state"));
            if (!m_pLoopback || got.path() != QLatin1String("/done") || token.isEmpty() ||
                    !sameText(state, m_signInState)) {
                pSocket->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n");
                pSocket->disconnectFromHost();
                return;
            }
            pSocket->write(kDonePage);
            pSocket->flush();
            pSocket->disconnectFromHost();
            setToken(token);
            cancelSignIn();
            emit signedInChanged();
            emit wantsFront();
            refreshMe();
            refreshRooms();
        });
    }
}

void Site::sayHello() {
    // For the site's admins: who has the app open now, which version, on
    // which computer. Nothing about what's played. Best effort: an older
    // site without it, or no network, and nothing happens.
    if (!signedIn()) {
        return;
    }
#if defined(Q_OS_WIN)
    const QString platform = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    const QString platform = QStringLiteral("mac");
#else
    const QString platform = QStringLiteral("linux");
#endif
    QJsonObject hello{{QStringLiteral("version"), QString::fromLatin1(::b2b::kVersion)},
            {QStringLiteral("platform"), platform}};
    // The room and whether we reach the friend directly or through the
    // relay (so the admins can see how the connection server is used).
    if (m_linkInfo) {
        const QJsonObject link = m_linkInfo();
        for (auto it = link.begin(); it != link.end(); ++it) {
            hello.insert(it.key(), it.value());
        }
    }
    QNetworkReply* pReply = request("POST", QStringLiteral("/api/app/hello"), hello);
    connect(pReply, &QNetworkReply::finished, pReply, &QObject::deleteLater);
}

void Site::signOut() {
    if (signedIn()) {
        // Ends the app's session on the site too (best effort).
        QNetworkReply* pReply = request("POST", QStringLiteral("/auth/logout"));
        connect(pReply, &QNetworkReply::finished, pReply, &QObject::deleteLater);
    }
    setToken(QString());
    m_me = QJsonObject();
    m_admin = false;
    m_mine = QJsonArray();
    emit signedInChanged();
    emit meChanged();
    refreshRooms();
}

void Site::watchRooms(bool on) {
    if (on) {
        refreshRooms();
        m_roomsTimer.start();
    } else {
        m_roomsTimer.stop();
    }
}

void Site::refreshRooms() {
    finish(request("GET", QStringLiteral("/api/rooms")), [this](const QJsonObject& reply, const QString& error) {
        m_roomsLoaded = true;
        m_roomsProblem = error;
        if (error.isEmpty()) {
            m_rooms = reply.value(QStringLiteral("rooms")).toArray();
            m_mine = reply.value(QStringLiteral("mine")).toArray();
            m_asked = reply.value(QStringLiteral("asked")).toArray();
        }
        emit roomsChanged();
    });
}

void Site::createRoom(const QString& title, const QString& visibility, Done done) {
    finish(request("POST",
                   QStringLiteral("/api/rooms"),
                   QJsonObject{{QStringLiteral("title"), title}, {QStringLiteral("visibility"), visibility}}),
            [this, done](const QJsonObject& reply, const QString& error) {
                done(reply.value(QStringLiteral("room")).toObject(), error);
                refreshRooms();
            });
}

namespace {
QString roomPath(const QString& id, const QString& rest = QString(), const QString& invite = QString()) {
    QString path = QStringLiteral("/api/rooms/") + QString::fromLatin1(QUrl::toPercentEncoding(id)) + rest;
    if (!invite.isEmpty()) {
        path += QStringLiteral("?invite=") + QString::fromLatin1(QUrl::toPercentEncoding(invite));
    }
    return path;
}
} // namespace

void Site::askToPlay(const QString& id, const QString& invite, Done done) {
    finish(request("POST", roomPath(id, QStringLiteral("/requests"), invite)),
            [done](const QJsonObject& reply, const QString& error) {
                done(reply.value(QStringLiteral("room")).toObject(), error);
            });
}

void Site::stopPlaying(const QString& id, Done done) {
    finish(request("DELETE", roomPath(id, QStringLiteral("/requests/mine"))),
            [done](const QJsonObject& reply, const QString& error) {
                done(reply.value(QStringLiteral("room")).toObject(), error);
            });
}

void Site::decide(const QString& id, int requestId, const QString& action, Done done) {
    finish(request("POST",
                   roomPath(id, QStringLiteral("/requests/%1").arg(requestId)),
                   QJsonObject{{QStringLiteral("action"), action}}),
            [done](const QJsonObject& reply, const QString& error) {
                done(reply.value(QStringLiteral("room")).toObject(), error);
            });
}

void Site::setSessionCode(const QString& id, const QString& code, const QString& secret) {
    finish(request("PATCH", roomPath(id),
                   QJsonObject{{QStringLiteral("join_code"), code}, {QStringLiteral("join_secret"), secret}}),
            [](const QJsonObject&, const QString&) {});
}

void Site::getRoom(const QString& id, Done done, const QString& invite) {
    finish(request("GET", roomPath(id, QString(), invite)),
            [done](const QJsonObject& reply, const QString& error) {
                done(reply.value(QStringLiteral("room")).toObject(), error);
            });
}

void Site::findRoomByCode(const QString& code, Done done) {
    finish(request("GET", QStringLiteral("/api/rooms/code/") + QUrl::toPercentEncoding(code.trimmed())),
            [done](const QJsonObject& reply, const QString& error) {
                done(reply.value(QStringLiteral("room")).toObject(), error);
            });
}

void Site::redeemInvite(const QString& code, std::function<void(const QString& error)> done) {
    finish(request("POST", QStringLiteral("/api/invites/redeem"), QJsonObject{{QStringLiteral("code"), code.trimmed()}}),
            [this, done](const QJsonObject& reply, const QString& error) {
                if (error.isEmpty() && reply.value(QStringLiteral("access")).toBool()) {
                    m_access = true;
                    refreshMe();
                    refreshRooms();
                }
                done(error);
            });
}

void Site::myInvites(std::function<void(const QJsonArray& codes, const QString& error)> done) {
    finish(request("GET", QStringLiteral("/api/invites/mine")), [done](const QJsonObject& reply, const QString& error) {
        done(reply.value(QStringLiteral("codes")).toArray(), error);
    });
}

void Site::downloadLink(const QString& platform, std::function<void(const QJsonObject& reply, const QString& error)> done) {
    finish(request("GET", QStringLiteral("/api/download/") + QUrl::toPercentEncoding(platform)), std::move(done));
}

void Site::closeRoom(const QString& id, Done done) {
    finish(request("PATCH", QStringLiteral("/api/rooms/") + QUrl::toPercentEncoding(id), QJsonObject{{QStringLiteral("closed"), true}}),
            [this, done](const QJsonObject& reply, const QString& error) {
                done(reply, error);
                refreshRooms();
            });
}

} // namespace mixxx::b2b
