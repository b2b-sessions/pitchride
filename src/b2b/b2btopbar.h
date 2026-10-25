#pragma once

#include <QPointer>
#include <QWidget>

class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QMenu;
class QPushButton;
class QToolButton;

namespace mixxx::b2b {

/// PitchRide's bar along the top of the window, in place of the menus:
///
///   [logo] PitchRide   ROOMS   BOOTH      (session)   code   ⚙  ?  (L)
///
/// The tabs switch pages; the right holds settings, help and the profile.
class TopBar : public QWidget {
    Q_OBJECT
  public:
    enum Tab { Rooms = 0, Booth = 1 };

    explicit TopBar(QWidget* pParent = nullptr);

    void setTab(int tab);
    int tab() const {
        return m_tab;
    }
    /// Shown while the Booth tab is (the 2 / 4 decks switch).
    void setBoothTools(QWidget* pTools);
    /// The B2B session's strip (shown while in a session).
    void setSession(QWidget* pStrip);
    /// Hosting a room: its code, shown to copy (empty: none).
    void setRoomCode(const QString& code);
    /// Something to look at on Rooms (someone wants to play); empty: none.
    /// answerable: with "Let them in" / "Not now" right there, so the
    /// host can answer from the booth.
    void setNotice(const QString& text, bool answerable = false);
    /// The signed-in person (empty: signed out).
    void setProfile(const QString& name, bool signingIn);
    /// Signed out: nothing but signing in (no tabs, no settings).
    void setSignedOut(bool signedOut);
    /// A newer PitchRide ("" : none). ready: downloaded and checked, so
    /// "Update" installs it; otherwise "Download" (the website).
    void setUpdate(const QString& version, bool ready = false);
    /// The update notice (for a menu under it).
    QWidget* updateNotice() const;

    QMenu* helpMenu() const {
        return m_pHelpMenu;
    }
    QMenu* profileMenu() const {
        return m_pProfileMenu;
    }

  signals:
    void tabChanged(int tab);
    void settingsClicked();
    void noticeClicked();
    /// "Let them in" (true) or "Not now" (false), from the notice.
    void noticeAnswered(bool yes);
    /// The update notice was clicked ("Update" or "Download").
    void updateClicked();

  protected:
    void paintEvent(QPaintEvent* pEvent) override;

  private:
    int m_tab = Rooms;
    QButtonGroup* m_pTabs;
    QHBoxLayout* m_pRow;
    QHBoxLayout* m_pMiddle;
    QPointer<QWidget> m_pBoothTools;
    QPointer<QWidget> m_pSession;
    QPushButton* m_pRoomCode;
    QPushButton* m_pUpdate;
    QString m_roomCode;
    QPushButton* m_pNotice;
    QPushButton* m_pNoticeYes;
    QPushButton* m_pNoticeNo;
    QToolButton* m_pSettings;
    QToolButton* m_pHelp;
    QToolButton* m_pProfile;
    QMenu* m_pHelpMenu;
    QMenu* m_pProfileMenu;
    QString m_name;
};

} // namespace mixxx::b2b
