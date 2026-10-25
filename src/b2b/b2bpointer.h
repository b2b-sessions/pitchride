#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

class QMainWindow;
class QWidget;

namespace mixxx::b2b {

class Manager;
class PointerMark;

/// In a session each DJ sees the other's mouse pointer on the decks (an
/// orange arrow marked FRIEND; a ring while they hold the button down).
/// Positions are sent relative to the part of the skin under the pointer
/// (the booth, player 3 or 4, a channel strip...), so they land on the same
/// control whether each DJ shows 2 or 4 decks. On a deck the other one doesn't
/// show, it waits at the edge where that deck would be, saying which deck.
/// With different skins a note says so instead.
class PointerShare : public QObject {
    Q_OBJECT
  public:
    explicit PointerShare(QObject* pParent = nullptr);
    ~PointerShare() override;

    void setWindow(QMainWindow* pWindow);
    /// The session (nullptr: none).
    void setManager(Manager* pManager);
    bool enabled() const {
        return m_enabled;
    }
    void setEnabled(bool on);
    /// Something covers the booth (the Rooms page): the friend's pointer
    /// isn't shown over it.
    void setCovered(bool covered) {
        m_covered = covered;
        if (covered) {
            hideMark();
        }
    }

  private:
    void tick();
    void received(double packed, const QString& where);
    QString mySkin() const;
    QWidget* area() const;
    void hideMark();

    QPointer<QMainWindow> m_pWindow;
    QPointer<Manager> m_pManager;
    QTimer m_timer;
    // A child of the skin or the window: goes when they go (a skin reload
    // deletes the old skin).
    QPointer<PointerMark> m_pMark;
    bool m_enabled = true;
    bool m_covered = false;
    double m_lastSent = -2;
    QElapsedTimer m_sinceSent;
    QElapsedTimer m_sinceHeard;
    // The friend's pointer: fraction of the area, button down, their view.
    double m_x = 0;
    double m_y = 0;
    bool m_pressed = false;
    bool m_visible = false;
    QString m_peerSkin;
    QString m_peerAnchor; // the part of the skin it is in ("" = all of it)
    QString m_lastWhere;
};

} // namespace mixxx::b2b
