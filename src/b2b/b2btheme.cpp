#include "b2b/b2btheme.h"

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QScreen>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtMath>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QPointer>
#include <QTimer>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>

#include "moc_b2btheme.cpp"

namespace mixxx::b2b {

namespace theme {

QFont font(Face face, int pixelSize) {
    QFont f;
    switch (face) {
    case Face::Display:
        // The 800 instance is a family of its own (Windows finds it by that
        // name); "Hanken Grotesk" at 800 where the system lists it so.
        f.setFamilies({QStringLiteral("Hanken Grotesk ExtraBold"), QStringLiteral("Hanken Grotesk")});
        f.setLetterSpacing(QFont::PercentageSpacing, 95); // tight: -0.05em
        break;
    case Face::Label:
        f.setFamilies({QStringLiteral("DM Mono"), QStringLiteral("monospace")});
        f.setCapitalization(QFont::AllUppercase);
        f.setLetterSpacing(QFont::AbsoluteSpacing, pixelSize * 0.12);
        break;
    case Face::Voice:
        f.setFamilies({QStringLiteral("Courier Prime"), QStringLiteral("monospace")});
        break;
    case Face::Readout:
        f.setFamilies({QStringLiteral("DotGothic16"), QStringLiteral("monospace")});
        f.setLetterSpacing(QFont::AbsoluteSpacing, pixelSize * 0.06);
        break;
    case Face::Body:
        f.setFamilies({QStringLiteral("Hanken Grotesk"), QStringLiteral("sans-serif")});
        f.setWeight(QFont::Normal);
        break;
    }
    f.setPixelSize(pixelSize);
    return f;
}

QString css(Face face) {
    switch (face) {
    case Face::Display:
        return QStringLiteral("font-family: 'Hanken Grotesk ExtraBold', 'Hanken Grotesk'; font-weight: 400;");
    case Face::Label:
        return QStringLiteral("font-family: 'DM Mono', monospace; font-weight: 400;");
    case Face::Voice:
        return QStringLiteral("font-family: 'Courier Prime', monospace; font-weight: 400;");
    case Face::Readout:
        return QStringLiteral("font-family: 'DotGothic16', monospace; font-weight: 400;");
    case Face::Body:
        return QStringLiteral("font-family: 'Hanken Grotesk', sans-serif; font-weight: 400;");
    }
    return QString();
}

QPixmap icon(Icon which, int size, const QColor& colour, qreal dpr) {
    QPixmap pixmap(qCeil(size * dpr), qCeil(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 16.0, size / 16.0); // drawn on a 16 grid
    p.setPen(QPen(colour, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    switch (which) {
    case Icon::Record:
        p.drawEllipse(QPointF(7, 9), 5.5, 5.5);
        p.drawLine(QPointF(14, 1.5), QPointF(11.5, 7));
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        p.drawEllipse(QPointF(7, 9), 1.2, 1.2);
        break;
    case Icon::Flip: {
        // Two half turns round the middle, each with its arrowhead: over
        // the top to the right, under the bottom back to the left.
        const QRectF round(2.5, 2.5, 11, 11);
        p.drawArc(round, 30 * 16, 150 * 16);  // from the right, over the top
        p.drawArc(round, 210 * 16, 150 * 16); // from the left, under the bottom
        p.drawLine(QPointF(2.5, 8), QPointF(1, 5.8));
        p.drawLine(QPointF(2.5, 8), QPointF(4.8, 6.6));
        p.drawLine(QPointF(13.5, 8), QPointF(15, 10.2));
        p.drawLine(QPointF(13.5, 8), QPointF(11.2, 9.4));
        break;
    }
    }
    p.end();
    return pixmap;
}

QString buttons(bool compact) {
    // Flat and square (DESIGN.md 8); pressed: one pixel down.
    const int padV = compact ? 5 : 9;
    const int padH = compact ? 12 : 16;
    return QStringLiteral(
            "QPushButton { %1 font-size: %2px; letter-spacing: 1px; padding: %3px %4px; border-radius: 0;"
            " color: %5; background: transparent; border: 1px solid %5; }"
            "QPushButton:hover { background: rgba(247, 214, 220, 0.10); }"
            "QPushButton:pressed { padding-top: %6px; padding-bottom: %7px; }"
            "QPushButton:disabled { color: %8; border-color: %8; }"
            "QPushButton#Primary, QPushButton:default { background: %9; border-color: %9; color: %10; }"
            "QPushButton#Primary:hover, QPushButton:default:hover { background: %11; border-color: %11; }"
            "QPushButton#Primary:disabled { background: transparent; color: %8; border-color: %8; }"
            "QPushButton#Quiet { border-color: transparent; color: %12; }"
            "QPushButton#Quiet:hover { color: %5; background: transparent; }"
            "QPushButton#Danger { border-color: %13; }"
            "QPushButton#Danger:hover { background: rgba(232, 72, 60, 0.16); }"
            "QPushButton::menu-indicator { image: none; width: 0; }")
            .arg(css(Face::Label),
                    QString::number(compact ? 11 : 12),
                    QString::number(padV),
                    QString::number(padH),
                    QLatin1String(kType),
                    QString::number(padV + 1),
                    QString::number(padV - 1),
                    QLatin1String(kFaint),
                    QLatin1String(kAction))
            .arg(QLatin1String(kOnAction),
                    QLatin1String(kActionHover),
                    QLatin1String(kTypeMuted),
                    QLatin1String(kKeyline));
}

QString dialog() {
    // A deep blue panel with the red keyline round it: the title in the
    // display face, the sentence typed, the fields outlined and square.
    return QStringLiteral(
                   "QDialog, QMessageBox, QInputDialog, QProgressDialog { background: %1; border: 1px solid %2; }"
                   "QLabel { color: %3; %4 font-size: 15px; }"
                   "QMessageBox QLabel#qt_msgbox_informativelabel { color: %5; padding: 0 6px 6px 6px; }"
                   "QLabel#Muted { color: %5; font-size: 14px; }"
                   "QLabel#Heading { %6 font-size: 26px; }"
                   "QLabel#Section { %7 font-size: 11px; letter-spacing: 1px; color: %5; }"
                   "QLineEdit, QSpinBox, QComboBox { background: transparent; color: %3; border: 1px solid %8;"
                   " border-radius: 0; padding: 7px 10px; %4 font-size: 15px; selection-background-color: %9;"
                   " selection-color: %10; }"
                   "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: %3; }"
                   "QLineEdit#CodeEdit { %11 font-size: 20px; letter-spacing: 2px; }"
                   "QRadioButton, QCheckBox { color: %3; %4 font-size: 15px; spacing: 9px; }"
                   "QRadioButton:disabled, QCheckBox:disabled { color: %12; }"
                   "QTextEdit, QPlainTextEdit { background: transparent; color: %3; border: 1px solid %8;"
                   " border-radius: 0; }"
                   "QProgressBar { background: transparent; border: 1px solid %8; border-radius: 0; height: 6px;"
                   " color: transparent; }"
                   "QProgressBar::chunk { background: %9; border-radius: 0; }")
                   .arg(QLatin1String(kDeep),
                           QLatin1String(kKeyline),
                           QLatin1String(kType),
                           css(Face::Voice),
                           QLatin1String(kTypeMuted),
                           css(Face::Display),
                           css(Face::Label),
                           QLatin1String(kLineStrong),
                           QLatin1String(kAction))
                   .arg(QLatin1String(kOnAction), css(Face::Readout), QLatin1String(kFaint)) +
            buttons(false);
}

QString bar() {
    return QStringLiteral(
                   "QLabel { color: %1; %2 font-size: 12px; letter-spacing: 1px; background: transparent; }"
                   "QLabel#Muted { color: %3; }"
                   "QLabel#Problem { color: %4; }"
                   "QPushButton:checked { background: rgba(247, 214, 220, 0.14); }"
                   "QSlider::groove:horizontal { height: 2px; background: %3; }"
                   "QSlider::sub-page:horizontal { background: %5; }"
                   "QSlider::handle:horizontal { background: %1; width: 10px; margin: -4px 0; border-radius: 0; }")
                   .arg(QLatin1String(kType),
                           css(Face::Label),
                           QLatin1String(kTypeMuted),
                           QLatin1String(kLcd),
                           QLatin1String(kAction)) +
            buttons(true) + menu();
}

QString menu() {
    return QStringLiteral(
            "QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: 0; padding: 4px;"
            " %4 font-size: 13px; }"
            "QMenu::item { padding: 6px 18px; border-radius: 0; }"
            "QMenu::item:selected { background: rgba(247, 214, 220, 0.12); color: %2; }"
            "QMenu::item:disabled { color: %5; }"
            "QMenu::separator { height: 1px; background: %6; margin: 4px 8px; }")
            .arg(QLatin1String(kDeep),
                    QLatin1String(kType),
                    QLatin1String(kKeyline),
                    css(Face::Label),
                    QLatin1String(kFaint),
                    QLatin1String(kLine));
}

void caps(QWidget* pWidget) {
    QFont f = pWidget->font();
    f.setCapitalization(QFont::AllUppercase);
    pWidget->setFont(f);
}

void paintPage(QPainter& p, const QWidget* pWidget, const QRect& rect) {
    // radial-gradient(ellipse at 30% 0%, lit 0, ground 50%, deep 100%), as
    // the mockups (design/reference/scripts/rooms.py), over the window.
    const QWidget* pWindow = pWidget->window();
    const QPoint at = pWidget->mapTo(pWindow, QPoint(0, 0));
    const QSizeF size(qMax(1, pWindow->width()), qMax(1, pWindow->height()));
    const QPointF centre(size.width() * 0.3 - at.x(), -at.y());
    // CSS "farthest-corner" for an ellipse: the same shape as the corner
    // box, reaching the farthest corner.
    const double dx = size.width() * 0.7;
    const double dy = size.height();
    const double rx = dx * M_SQRT2;
    const double ry = dy * M_SQRT2;
    QRadialGradient light(QPointF(0, 0), 1.0);
    light.setColorAt(0.0, QColor(kGroundLit));
    light.setColorAt(0.5, QColor(kGround));
    light.setColorAt(1.0, QColor(kDeep));
    QBrush brush(light);
    QTransform t;
    t.translate(centre.x(), centre.y());
    t.scale(rx, ry);
    brush.setTransform(t);
    p.fillRect(rect, brush);
}

} // namespace theme

namespace {

/// The toast: paints itself at its own opacity (a graphics effect does not
/// show over the window's OpenGL views).
class Toast : public QWidget {
  public:
    Toast(const QString& text, QWidget* pParent)
            : QWidget(pParent),
              m_text(text) {
        setObjectName(QStringLiteral("B2BToast"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        // A typed note (DESIGN.md 5): the voice face, square, the keyline
        // round it.
        setFont(theme::font(theme::Face::Voice, 15));
        const QFontMetrics fm(font());
        resize(fm.horizontalAdvance(m_text) + 40, fm.height() + 22);
        m_clock.start();
        m_tick.setInterval(16);
        connect(&m_tick, &QTimer::timeout, this, [this] {
            const qint64 t = m_clock.elapsed();
            if (t >= kIn + kStay + kOut) {
                deleteLater();
                return;
            }
            update();
        });
        m_tick.start();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const qint64 t = m_clock.elapsed();
        double a = 1.0;
        if (t < kIn) {
            a = double(t) / kIn;
        } else if (t > kIn + kStay) {
            a = 1.0 - double(t - kIn - kStay) / kOut;
        }
        a = qBound(0.0, a, 1.0);
        QPainter p(this);
        p.setOpacity(a);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QColor(theme::kKeyline));
        p.setBrush(QColor(theme::kDeep));
        p.drawRect(r);
        p.setPen(QColor(theme::kType));
        p.drawText(rect(), Qt::AlignCenter, m_text);
    }

  private:
    static constexpr qint64 kIn = 160;
    static constexpr qint64 kStay = 2200;
    static constexpr qint64 kOut = 320;
    QString m_text;
    QElapsedTimer m_clock;
    QTimer m_tick;
};

} // namespace

void toast(QWidget* pWindow, const QString& text) {
    if (!pWindow) {
        return;
    }
    pWindow = pWindow->window();
    // The bottom of what can be seen of the window (it may run off the screen).
    int bottom = pWindow->height();
    if (QScreen* pScreen = pWindow->screen()) {
        const int screenBottom = pWindow->mapFromGlobal(
                pScreen->availableGeometry().bottomLeft()).y();
        bottom = qMin(bottom, screenBottom);
    }
    // On the page that is showing there (the page, the skin: windows of
    // their own, which a note on the window itself would be under).
    const QPoint spot(pWindow->width() / 2, bottom - 40);
    QWidget* pHost = pWindow->childAt(spot);
    while (pHost && pHost->parentWidget() != pWindow) {
        pHost = pHost->parentWidget();
    }
    if (!pHost) {
        pHost = pWindow;
    }
    for (QWidget* pOld : pHost->findChildren<QWidget*>(
                 QStringLiteral("B2BToast"), Qt::FindDirectChildrenOnly)) {
        pOld->deleteLater();
    }
    auto* pToast = new Toast(text, pHost);
    const QPoint at = pHost->mapFrom(pWindow,
            QPoint((pWindow->width() - pToast->width()) / 2, bottom - pToast->height() - 36));
    pToast->move(at);
    pToast->show();
    pToast->raise();
}

void fadeIn(QWidget* pWidget, int ms) {
    if (!pWidget) {
        return;
    }
    auto* pFade = new QGraphicsOpacityEffect(pWidget);
    pFade->setOpacity(0);
    pWidget->setGraphicsEffect(pFade);
    auto* pIn = new QPropertyAnimation(pFade, "opacity", pWidget);
    pIn->setDuration(ms);
    pIn->setEndValue(1.0);
    pIn->setEasingCurve(QEasingCurve::OutCubic);
    // Done: no effect left (it would slow the page down).
    QObject::connect(pIn, &QAbstractAnimation::finished, pWidget, [pWidget] {
        pWidget->setGraphicsEffect(nullptr);
    });
    pIn->start(QAbstractAnimation::DeleteWhenStopped);
}

DialogStyler::DialogStyler(QObject* pParent)
        : QObject(pParent) {
}

bool DialogStyler::eventFilter(QObject* pObject, QEvent* pEvent) {
    // Before it's laid out (a message box sizes itself for its text then),
    // and again when shown in case it was made before we were installed.
    if (pEvent->type() == QEvent::Polish || pEvent->type() == QEvent::Show) {
        // Message boxes and the small dialogs (input, progress), not the
        // big ones that have a look of their own (Preferences, track info).
        if (qobject_cast<QMessageBox*>(pObject) || qobject_cast<QInputDialog*>(pObject) ||
                qobject_cast<QProgressDialog*>(pObject)) {
            style(static_cast<QWidget*>(pObject));
        }
    }
    return false;
}

void DialogStyler::style(QWidget* pDialog) {
    if (pDialog->property("b2bStyled").toBool()) {
        return;
    }
    pDialog->setProperty("b2bStyled", true);
    if (pDialog->styleSheet().isEmpty()) {
        pDialog->setStyleSheet(theme::dialog());
    }
    // PitchRide to its users (Mixxx's own boxes say "Mixxx").
    auto rename = [](QString text) {
        return text.replace(QLatin1String("Mixxx"), QLatin1String("PitchRide"));
    };
    pDialog->setWindowTitle(rename(pDialog->windowTitle()));
    if (auto* pBox = qobject_cast<QMessageBox*>(pDialog)) {
        // The words say it: no warning triangle or question bubble.
        pBox->setIconPixmap(QPixmap());
        pBox->setIcon(QMessageBox::NoIcon);
        if (pBox->text().contains(QLatin1String("Mixxx"))) {
            pBox->setText(rename(pBox->text()));
        }
        if (pBox->informativeText().contains(QLatin1String("Mixxx"))) {
            pBox->setInformativeText(rename(pBox->informativeText()));
        }
    }
    // No coloured balls and ticks on the buttons; a hand over them, and
    // their labels in capitals (DM Mono, DESIGN.md 8).
    for (QAbstractButton* pButton : pDialog->findChildren<QAbstractButton*>()) {
        pButton->setIcon(QIcon());
        pButton->setCursor(Qt::PointingHandCursor);
        pButton->setText(rename(pButton->text()));
        if (qobject_cast<QPushButton*>(pButton)) {
            theme::caps(pButton);
        }
    }
    // Laid out again once shown, the new text and look in it (kept centred).
    QPointer<QWidget> pSafe(pDialog);
    QTimer::singleShot(0, pDialog, [pSafe] {
        if (!pSafe) {
            return;
        }
        const QPoint centre = pSafe->geometry().center();
        if (auto* pBox = qobject_cast<QMessageBox*>(pSafe.data())) {
            // A message box sizes itself for its text (fixed): again, in
            // this font.
            pBox->setText(pBox->text());
        } else {
            if (pSafe->layout()) {
                pSafe->layout()->activate();
            }
            pSafe->adjustSize();
        }
        QRect r = pSafe->geometry();
        r.moveCenter(centre);
        pSafe->setGeometry(r);
    });
}

} // namespace mixxx::b2b
