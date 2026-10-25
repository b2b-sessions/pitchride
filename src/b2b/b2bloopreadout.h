#pragma once

#include <QObject>
#include <QPair>
#include <QPointer>
#include <QTimer>
#include <array>

class QWidget;

namespace mixxx::b2b {

class LoopBadge;

/// While a deck loops, its length on the deck's screen ("LOOP 4", "LOOP
/// 1/2"): a box like KEY and BPM, left of them in the screen's top row
/// (the title and artist cut shorter meanwhile). Found by the skin's
/// names (KeyN, TitleN, ArtistN), again when it is made again. From
/// [B2BBooth],deckN_loop_beats.
class LoopReadoutUi : public QObject {
    Q_OBJECT
  public:
    explicit LoopReadoutUi(QObject* pParent = nullptr);
    void setWindow(QWidget* pWindow);

    /// 4 -> "4", 0.5 -> "1/2", 3.25 -> "3.3"; below 0 (no beat grid): "".
    static QString lengthText(double beats);

  private:
    void refresh();

    QPointer<QWidget> m_pWindow;
    std::array<QPointer<LoopBadge>, 4> m_badges;
    std::array<QPair<int, int>, 4> m_widths; // the title's and artist's own
    QTimer m_timer;
};

} // namespace mixxx::b2b
