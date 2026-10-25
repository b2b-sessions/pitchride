#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpServer;

namespace mixxx::b2b {

/// The PitchRide website, from the app: signing in (the browser signs you
/// in and hands the app a token on a loopback address) and the rooms.
///
/// The site is https://pitchride.co.uk, or B2B_WEB_URL (testing). The token
/// is kept in b2b.ini.
class Site : public QObject {
    Q_OBJECT
  public:
    explicit Site(const QString& settingsPath, QObject* pParent = nullptr);
    ~Site() override;

    /// A page or API path on the site.
    QUrl url(const QString& path = QString()) const;

    bool signedIn() const {
        return !m_token.isEmpty();
    }
    /// The signed-in person ({id, name, avatar}), empty until known.
    QJsonObject me() const {
        return m_me;
    }
    /// For the site's sockets (Authorization: Bearer).
    QByteArray token() const {
        return m_token.toUtf8();
    }
    bool isAdmin() const {
        return m_admin;
    }
    /// Signed in, and the account may play (invite-only beta: it redeemed
    /// an invite code). Until the site has said, assumed so.
    bool canPlay() const {
        return signedIn() && m_access;
    }
    /// An invite code (K7PM-2QXR): the account may play once it's taken.
    void redeemInvite(const QString& code, std::function<void(const QString& error)> done);
    /// Your codes to give a friend: [{code: "K7PM-2QXR", used, used_by}].
    void myInvites(std::function<void(const QJsonArray& codes, const QString& error)> done);
    /// The installer for a computer ("windows", "mac-intel",
    /// "mac-apple-silicon"): a private link that works for a few minutes
    /// ({url, version, expires}). Only for accounts that are in.
    void downloadLink(const QString& platform, std::function<void(const QJsonObject& reply, const QString& error)> done);

    /// Opens the browser to sign in; signedInChanged() when it's done.
    void signIn();
    void cancelSignIn();
    bool signingIn() const {
        return m_pLoopback != nullptr;
    }
    void signOut();

    /// The open public rooms and the signed-in person's own rooms.
    QJsonArray rooms() const {
        return m_rooms;
    }
    QJsonArray myRooms() const {
        return m_mine;
    }
    /// Rooms the signed-in person asked to play in (pending or approved).
    QJsonArray askedRooms() const {
        return m_asked;
    }
    bool roomsLoaded() const {
        return m_roomsLoaded;
    }
    QString roomsProblem() const {
        return m_roomsProblem;
    }
    void refreshRooms();
    /// Keeps the rooms up to date while something shows them.
    void watchRooms(bool on);

    using Done = std::function<void(const QJsonObject& reply, const QString& error)>;
    void createRoom(const QString& title, const QString& visibility, Done done);
    void getRoom(const QString& id, Done done, const QString& invite = QString());
    /// Ask the host to play in their room (invite: for a private room).
    void askToPlay(const QString& id, const QString& invite, Done done);
    void stopPlaying(const QString& id, Done done);
    /// The host: "approve", "decline" or "remove" a request to play.
    void decide(const QString& id, int requestId, const QString& action, Done done);
    /// The host: the B2B session's join code, for the DJs the room lets in.
    // The room's join code and the session secret that goes with it (the
    // guest's app needs both; the join-code server never gets the secret).
    void setSessionCode(const QString& id, const QString& code, const QString& secret = QString());
    void findRoomByCode(const QString& code, Done done);
    void closeRoom(const QString& id, Done done);

  signals:
    void signedInChanged();
    void meChanged();
    void roomsChanged();
    void signInProblem(const QString& message);
    /// The browser finished signing in: bring the app to the front.
    void wantsFront();

  private:
    void refreshMe();
    void setToken(const QString& token);
    QNetworkReply* request(const QByteArray& verb, const QString& path, const QJsonObject& body = {});
    void finish(QNetworkReply* pReply, Done done);
    void loopbackConnection();

    QString m_settingsFile;
    QString m_token;
    QJsonObject m_me;
    bool m_admin = false;
    bool m_access = true;
    QJsonArray m_rooms;
    QJsonArray m_mine;
    QJsonArray m_asked;
    bool m_roomsLoaded = false;
    QString m_roomsProblem;
    QNetworkAccessManager* m_pNet;
    QTcpServer* m_pLoopback = nullptr;
    QString m_signInState; // random, in our loopback address: only a token sent back with it is taken
    QTimer m_roomsTimer;
    QTimer m_signInTimeout;
    QTimer m_helloTimer; // signed in: "the app is open" to the site, once a minute
    std::function<QJsonObject()> m_linkInfo; // the room we play in, and how we reach the friend
    void sayHello();

  public:
    /// Added to the once-a-minute "the app is open": {"room": id, "link":
    /// "direct" | "relay" | ""}.
    void setLinkInfo(std::function<QJsonObject()> fn) {
        m_linkInfo = std::move(fn);
    }
};

} // namespace mixxx::b2b
