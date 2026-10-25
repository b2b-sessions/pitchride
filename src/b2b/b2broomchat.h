#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QBoxLayout;
class QLabel;
class QLineEdit;
class QTextBrowser;

namespace mixxx::b2b {

class Site;
class WebSocketClient;

/// A room's chat: the middle of the booth's panels (RoomPanels), the room
/// you're in, your own room while you host it, or the room you're playing
/// in. One chat, wherever you are. Its connection also says how the room
/// is (roomChanged) and who's in it (peopleChanged).
class RoomChat : public QWidget {
    Q_OBJECT
  public:
    explicit RoomChat(Site* pSite, QWidget* pParent = nullptr);
    ~RoomChat() override;

    /// The room to chat in (invite: a private room's); empty: none.
    void setRoom(const QString& roomId, const QString& invite);
    QString roomId() const {
        return m_roomId;
    }
    /// The panels are open (unread messages count while they aren't).
    void setOpen(bool open);
    /// One line of the chat only, and the input (the slim strip under
    /// four decks), or all of it.
    void setSlim(bool slim);

  signals:
    /// The room as the chat's connection describes it (title, host, the
    /// DJs, your request...).
    void roomChanged(const QJsonObject& room);
    /// Who is in the room ([{id, name}], the signed-in ones).
    void peopleChanged(const QJsonArray& people);
    void unreadChanged(int unread);

  private:
    void connectNow();
    void received(const QString& text);
    void render();
    void send();

    Site* m_pSite;
    WebSocketClient* m_pSocket;
    QString m_roomId;
    QString m_invite;
    QJsonObject m_room;
    QTimer m_retry;
    int m_wait = 1000;
    QVector<QJsonObject> m_messages;
    bool m_open = false;
    bool m_slim = false;
    int m_unread = 0;
    QBoxLayout* m_pLayout;
    QLabel* m_pTitle;
    QTextBrowser* m_pMessages;
    QLineEdit* m_pInput;
    QLabel* m_pNote;
};

} // namespace mixxx::b2b
