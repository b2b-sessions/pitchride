#pragma once

#include <QAbstractButton>
#include <QFrame>
#include <QObject>
#include <QPushButton>
#include <QPointer>
#include <QTimer>
#include <array>

#include "b2b/b2babouttext.h"

namespace mixxx::b2b {

class AboutPanel;

/// A small round "i" in the booth's copper, at the top corner of a deck
/// (the end of its title row) or of the mixer (after MIXER). Click it,
/// or Enter / Space on it: the panel about that piece of gear, next to it.
class AboutButton : public QAbstractButton {
    Q_OBJECT
  public:
    AboutButton(const about::Text& text, QWidget* pParent);
    ~AboutButton() override;

    QSize sizeHint() const override;
    /// Open or closed (the button lit while it's open).
    void togglePanel(bool byKeyboard = false);

  protected:
    bool event(QEvent* pEvent) override;
    void paintEvent(QPaintEvent* pEvent) override;
    void keyPressEvent(QKeyEvent* pEvent) override;
    void enterEvent(QEnterEvent* pEvent) override;
    void leaveEvent(QEvent* pEvent) override;

  private:
    const about::Text& m_text;
    QPointer<AboutPanel> m_pPanel;
    bool m_hovered = false;
    bool m_byKeyboard = false;
};

/// The panel: its title, a few lines and the link to the real thing (in
/// the browser). A pop-up: a click anywhere else, Esc, or the button again
/// closes it.
class AboutPanel : public QFrame {
    Q_OBJECT
  public:
    AboutPanel(const about::Text& text, AboutButton* pButton);

    /// Next to the button, inside its window.
    void showBy(QWidget* pButton, bool byKeyboard);

  protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override;
    void mousePressEvent(QMouseEvent* pEvent) override;
    void keyPressEvent(QKeyEvent* pEvent) override;
    void hideEvent(QHideEvent* pEvent) override;

  private:
    QPointer<AboutButton> m_pButton;
    QPointer<QPushButton> m_pLink;
};

/// Puts the "i" buttons on the skin's deck title rows (DeckHead1-4) and
/// next to its MIXER title, again whenever the skin is made again.
class AboutGearUi : public QObject {
    Q_OBJECT
  public:
    explicit AboutGearUi(QObject* pParent = nullptr);
    void setWindow(QWidget* pWindow);

  private:
    void refresh();

    QPointer<QWidget> m_pWindow;
    std::array<QPointer<AboutButton>, 4> m_decks;
    QPointer<AboutButton> m_pMixer;
    QTimer m_timer;
};

} // namespace mixxx::b2b
