#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QWidget>
#include <functional>

class QMainWindow;

namespace mixxx::b2b {

/// The two-deck B2B skins: a player turns round to deck 3 (or 4) and back
/// (click its title). The skin has the player twice, Side1 and Side3 (Side2,
/// Side4), and shows one; this turns it over like a card instead of
/// switching at once: a picture of the side going (OpenGL views read back
/// too), the switch, a picture of the side coming, and the turn drawn over
/// them.
class DeckFlip : public QWidget {
    Q_OBJECT
  public:
    explicit DeckFlip(QMainWindow* pWindow);

    /// Side 0 (left) or 1 (right) switches: apply() does it. Animated if
    /// this skin has the sides, else at once.
    void flip(int side, std::function<void()> apply);

  protected:
    void paintEvent(QPaintEvent* pEvent) override;
    /// A click on a player's title (DeckTitleN) turns it round.
    bool eventFilter(QObject* pObject, QEvent* pEvent) override;

  private:
    /// The side shown now (Side1..4 visible), or null.
    QWidget* shownSide(int side) const;
    /// rect (window coordinates) as it looks, OpenGL views included.
    QImage picture(const QRect& rect) const;
    void hideGlViews(const QRect& rect, bool hide);
    void finish();
    /// The players' titles (the skin makes them again on a reload).
    void watchTitles();

    QPointer<QMainWindow> m_pWindow;
    QImage m_from;
    QImage m_to;
    QRect m_rect;
    QElapsedTimer m_clock;
    QTimer m_frame;
    QTimer m_titles;
    bool m_busy = false;
    bool m_hasSides = false; // this skin's players turn round (Side1..4)
    QList<QPointer<QWidget>> m_hidden;
};

} // namespace mixxx::b2b
