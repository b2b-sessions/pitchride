#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <array>

class PlayerManager;
class QWidget;

namespace mixxx::b2b {

class Manager;
class NowLine;

/// The copper line above each deck (DESIGN.md 7, room layout 3): who has it
/// and what is on it, "DECK 1 · MIA · NOW: BLUE HOUR — THE LANES · 124 BPM ·
/// 8A" (CUED: while it waits, EMPTY with no track). In the skin's
/// title row above each player (DeckHeadN, in place of its PLAYER N title),
/// so it turns with the player when it shows deck 3 or 4, and it is a plain
/// widget there, over the booth's picture, never over the decks' OpenGL
/// views. It follows the decks as they change, a few times a second.
class NowPlayingUi : public QObject {
    Q_OBJECT
  public:
    explicit NowPlayingUi(PlayerManager* pPlayerManager, QObject* pParent = nullptr);

    void setWindow(QWidget* pWindow);
    /// The session (nullptr: none): who plays which deck.
    void setManager(Manager* pManager);
    /// In a room: your side ("A" / "B", empty: none) and your name; the
    /// other side's name (who plays the decks that aren't yours).
    void setSides(const QString& mySide, const QString& myName, const QString& otherName);

    /// Deck n's line, as shown (for tests and the room's stream).
    QString line(int deck) const;
    /// Who has deck n now ("A" / "B", empty: no sides here), and their name.
    QString sideOf(int deck) const;
    QString whoOf(int deck) const;

  private:
    void refresh();
    QString text(int deck) const;

    PlayerManager* m_pPlayerManager;
    QPointer<QWidget> m_pWindow;
    QPointer<Manager> m_pManager;
    QString m_mySide;
    QString m_myName;
    QString m_otherName;
    std::array<QPointer<NowLine>, 4> m_lines;
    QTimer m_timer;
};

} // namespace mixxx::b2b
