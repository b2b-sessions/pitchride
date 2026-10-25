#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

class QWidget;

namespace mixxx::b2b {

class Manager;
class PeerScreen;

/// Each DJ sees the other's music browser: while the friend browses on a
/// player's screen, that player's screen here shows what they see (where
/// they are, the list, their cursor), to look at, not to use.
///
/// Sent as text, a row per session message (the friend's playlists aren't
/// on this computer, and pictures would compete with track sharing):
///   kind 1  "<deck>\t<tracks 0/1>\t<cursor row>\t<tracks in the list>\t<rows>"
///   kind 2  the heading      kind 3  where it is (the crumb)
///   kind 4  "<row>\t<row text>"   kind 5  done: show it
///   kind 6  "<deck>": the browser is closed
///
/// Controls, group [B2BBooth]: deckN_peer_browsing (1 while the friend's
/// browser is on player N's screen here).
class PeerBrowser : public QObject {
    Q_OBJECT
  public:
    explicit PeerBrowser(QObject* pParent = nullptr);
    ~PeerBrowser() override;

    void setWindow(QWidget* pWindow);
    void setManager(Manager* pManager);

  private:
    struct View {
        int deck = 0;
        bool tracks = false;
        int current = -1;
        int total = 0;
        QString heading;
        QString crumb;
        QStringList rows;
    };
    void tick();
    void received(double kind, const QString& text);
    void show(const View& view);
    void hide();

    QPointer<QWidget> m_pWindow;
    QPointer<Manager> m_pManager;
    QTimer m_timer;

    // Sending.
    QStringList m_lastSent;
    QElapsedTimer m_sinceSent;
    bool m_sentOpen = false;
    int m_sentDeck = 0;

    // Receiving.
    View m_incoming;
    QElapsedTimer m_sinceReceived;
    QPointer<PeerScreen> m_pScreen;
    int m_shownDeck = 0;
    QList<QPointer<QWidget>> m_hiddenWaveforms;
};

} // namespace mixxx::b2b
