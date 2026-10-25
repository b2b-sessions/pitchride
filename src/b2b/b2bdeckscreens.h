#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <array>

class QWidget;

namespace mixxx::b2b {

class StandbyScreen;

/// The players' screens before a track is loaded: a standby page ("PRESS
/// SOURCE", click it to browse) in the skins' StandbyN slots, and a short
/// power-up when a track loads. See BoothControls for deckN_standby /
/// deckN_powering.
class DeckScreensUi : public QObject {
    Q_OBJECT
  public:
    explicit DeckScreensUi(QObject* pParent = nullptr);
    ~DeckScreensUi() override;
    void setWindow(QWidget* pWindow);

  private:
    void refresh();

    struct Deck {
        QPointer<QWidget> slot;
        QPointer<StandbyScreen> screen;
    };
    QPointer<QWidget> m_pWindow;
    std::array<Deck, 4> m_decks;
    QTimer m_timer;
};

} // namespace mixxx::b2b
