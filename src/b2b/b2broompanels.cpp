#include "b2b/b2broompanels.h"

#include "b2b/b2bflyerwall.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QHideEvent>
#include <QJsonObject>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QOpenGLWindow>
#include <QPalette>
#include <QPaintEvent>
#include <QPainter>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cstdlib>

#include "b2b/b2broomchat.h"
#include "b2b/b2bsetlist.h"
#include "b2b/b2btheme.h"
#include "skin/legacy/legacyskinparser.h"
#include "moc_b2broompanels.cpp"
#include "widget/wglwidget.h"

namespace mixxx::b2b {

namespace {

// The columns, as the mockup (520 | the rest | 330 of 1600).
constexpr int kSetListStretch = 33;
constexpr int kChatStretch = 46;
constexpr int kPeopleStretch = 21;
constexpr int kPadX = 20;
constexpr int kPadY = 14;
constexpr int kSlimBelow = 170;     // px: panels shorter than this are compact
const QColor kColumnLine(232, 72, 60, 64);     // the keyline, faint
const QColor kRowLine(170, 183, 242, 38);      // a faint divider

QLabel* title(const QString& text, QWidget* pParent) {
    auto* pLabel = new QLabel(text, pParent);
    pLabel->setObjectName(QStringLiteral("Title"));
    theme::caps(pLabel);
    return pLabel;
}

QString panelsStyle() {
    using namespace theme;
    return QStringLiteral(
                   "QLabel#Title { color: %1; %2 font-size: 11px; letter-spacing: 1px; background: transparent; }"
                   "QLabel#Role { color: #DFE6FF; %3 font-size: 14px; background: transparent; }")
            .arg(QLatin1String(kTypeMuted), css(Face::Label), css(Face::Body));
}

} // namespace

/// The setlist's lines: time, title (and artist), DJ; newest at the
/// bottom. The wheel goes back through it.
class SetListView : public QWidget {
  public:
    SetListView(SetList* pSetList, QWidget* pParent)
            : QWidget(pParent),
              m_pSetList(pSetList) {
        setMinimumHeight(30);
        connect(pSetList, &SetList::changed, this, [this] {
            m_back = 0; // the newest in sight
            update();
        });
    }

  protected:
    void wheelEvent(QWheelEvent* pEvent) override {
        const int steps = pEvent->angleDelta().y() / 120;
        m_back = std::clamp(m_back + steps, 0, std::max(0, static_cast<int>(m_pSetList->entries().size()) - rows()));
        update();
        pEvent->accept();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        const auto& entries = m_pSetList->entries();
        if (entries.isEmpty()) {
            p.setFont(theme::font(theme::Face::Body, 14));
            p.setPen(QColor(theme::kTypeMuted));
            p.drawText(QRect(0, 0, width(), m_row), Qt::AlignLeft | Qt::AlignVCenter, tr("nothing played yet."));
            return;
        }
        const int shown = std::min(rows(), static_cast<int>(entries.size()));
        const int last = static_cast<int>(entries.size()) - 1 - m_back;
        const int first = std::max(0, last - shown + 1);
        const QFont mono = theme::font(theme::Face::Label, 11);
        QFont plain = mono;
        plain.setCapitalization(QFont::MixedCase);
        plain.setLetterSpacing(QFont::AbsoluteSpacing, 0);
        const QFont body = theme::font(theme::Face::Body, 14);
        const QFont smallFont = theme::font(theme::Face::Body, 12);
        int y = 0;
        for (int i = first; i <= last; ++i) {
            const SetList::Entry& e = entries[i];
            const QRect row(0, y, width(), m_row);
            p.setFont(plain);
            p.setPen(QColor(theme::kFaint));
            p.drawText(row, Qt::AlignLeft | Qt::AlignVCenter, e.at.toString(QStringLiteral("HH:mm")));
            // The DJ at the right; the title, then the artist, in what's left.
            const QString dj = e.dj.toLower();
            const int djWidth = QFontMetrics(plain).horizontalAdvance(dj);
            p.setPen(QColor(theme::kTypeMuted));
            p.drawText(row, Qt::AlignRight | Qt::AlignVCenter, dj);
            const int x0 = 50;
            const int room = std::max(0, width() - x0 - djWidth - 14);
            const QFontMetrics bm(body);
            const QString titleText = bm.elidedText(e.title.toLower(), Qt::ElideRight, room);
            p.setFont(body);
            p.setPen(QColor(theme::kType));
            p.drawText(QRect(x0, y, room, m_row), Qt::AlignLeft | Qt::AlignVCenter, titleText);
            const int used = bm.horizontalAdvance(titleText);
            if (!e.artist.isEmpty() && used + 8 < room) {
                p.setFont(smallFont);
                p.setPen(QColor(theme::kTypeMuted));
                const QString artist = QFontMetrics(smallFont).elidedText(e.artist.toLower(), Qt::ElideRight, room - used - 8);
                p.drawText(QRect(x0 + used + 8, y, room - used - 8, m_row), Qt::AlignLeft | Qt::AlignVCenter, artist);
            }
            p.setPen(kRowLine);
            p.drawLine(0, y + m_row - 1, width(), y + m_row - 1);
            y += m_row;
        }
    }

  public:
    void setCompact(bool compact) {
        m_row = compact ? 22 : 29;
        update();
    }

  private:
    int rows() const {
        return std::max(1, height() / m_row);
    }
    int m_row = 29;
    SetList* m_pSetList;
    int m_back = 0; // rows back from the newest
};

/// Everyone in the room as a square initial (DESIGN.md 8: people are
/// squares, not circles); their name on hover.
class PeopleView : public QWidget {
  public:
    explicit PeopleView(QWidget* pParent)
            : QWidget(pParent) {
        setMinimumHeight(kSquare);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    void setPeople(const QJsonArray& people) {
        m_names.clear();
        for (const QJsonValue& v : people) {
            const QString name = v.toObject().value(QStringLiteral("name")).toString().trimmed();
            if (!name.isEmpty()) {
                m_names.append(name);
            }
        }
        updateGeometry();
        update();
    }
    bool hasHeightForWidth() const override {
        return true;
    }
    int heightForWidth(int w) const override {
        const int perRow = std::max(1, (w + kGap) / (kSquare + kGap));
        const int rowsWanted = std::max(1, static_cast<int>((m_names.size() + perRow - 1) / perRow));
        return std::min(rowsWanted, 3) * (kSquare + kGap);
    }
    QSize sizeHint() const override {
        return QSize(200, heightForWidth(200));
    }

  protected:
    bool event(QEvent* pEvent) override {
        if (pEvent->type() == QEvent::ToolTip) {
            auto* pHelp = static_cast<QHelpEvent*>(pEvent);
            const int i = indexAt(pHelp->pos());
            if (i >= 0) {
                QToolTip::showText(pHelp->globalPos(), m_names[i], this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(pEvent);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont f = theme::font(theme::Face::Label, 12);
        f.setCapitalization(QFont::MixedCase);
        p.setFont(f);
        const int perRow = std::max(1, (width() + kGap) / (kSquare + kGap));
        const int rowsShown = std::max(1, (height() + kGap) / (kSquare + kGap));
        const int fit = perRow * rowsShown;
        const int n = static_cast<int>(m_names.size());
        for (int i = 0; i < std::min(n, fit); ++i) {
            const QRect r = square(i, perRow);
            p.setPen(QColor(247, 214, 220, 128));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5));
            p.setPen(QColor(theme::kType));
            // The last square that fits says how many more there are.
            const QString text = (i == fit - 1 && n > fit) ? QStringLiteral("+%1").arg(n - fit + 1)
                                                            : m_names[i].left(1).toLower();
            p.drawText(r, Qt::AlignCenter, text);
        }
    }

  private:
    static constexpr int kSquare = 30;
    static constexpr int kGap = 6;
    QRect square(int i, int perRow) const {
        return QRect((i % perRow) * (kSquare + kGap), (i / perRow) * (kSquare + kGap), kSquare, kSquare);
    }
    int indexAt(const QPoint& at) const {
        const int perRow = std::max(1, (width() + kGap) / (kSquare + kGap));
        for (int i = 0; i < m_names.size(); ++i) {
            if (square(i, perRow).contains(at)) {
                return i;
            }
        }
        return -1;
    }
    QStringList m_names;
};

RoomPanels::RoomPanels(RoomChat* pChat, SetList* pSetList, QWidget* pParent)
        : QWidget(pParent),
          m_pChat(pChat) {
    setObjectName(QStringLiteral("RoomPanels"));
    setStyleSheet(panelsStyle());
    auto* pRow = new QHBoxLayout(this);
    pRow->setContentsMargins(0, 0, 0, 0);
    pRow->setSpacing(0);

    // The setlist: the back of the sleeve.
    auto* pSetColumn = new QWidget(this);
    {
        auto* pCol = new QVBoxLayout(pSetColumn);
        m_columns.append(pCol);
        pCol->setContentsMargins(kPadX, kPadY, kPadX, kPadY);
        pCol->setSpacing(6);
        m_pSetTitle = title(tr("the setlist"), pSetColumn);
        pCol->addWidget(m_pSetTitle);
        m_pSetList = new SetListView(pSetList, pSetColumn);
        pCol->addWidget(m_pSetList, 1);
    }
    pRow->addWidget(pSetColumn, kSetListStretch);

    // The chat.
    auto* pChatColumn = new QWidget(this);
    {
        auto* pCol = new QVBoxLayout(pChatColumn);
        m_columns.append(pCol);
        pCol->setContentsMargins(kPadX, kPadY, kPadX, kPadY);
        pChat->setParent(pChatColumn);
        pChat->show();
        pCol->addWidget(pChat, 1);
    }
    pRow->addWidget(pChatColumn, kChatStretch);

    // Who's in the room, and what you are there.
    auto* pPeopleColumn = new QWidget(this);
    {
        auto* pCol = new QVBoxLayout(pPeopleColumn);
        m_columns.append(pCol);
        pCol->setContentsMargins(kPadX, kPadY, kPadX, kPadY);
        pCol->setSpacing(10);
        m_pPeopleTitle = title(tr("in the room"), pPeopleColumn);
        pCol->addWidget(m_pPeopleTitle);
        m_pPeople = new PeopleView(pPeopleColumn);
        pCol->addWidget(m_pPeople);
        pCol->addSpacing(6);
        m_pRoleTitle = title(QString(), pPeopleColumn);
        pCol->addWidget(m_pRoleTitle);
        m_pRole = new QLabel(pPeopleColumn);
        m_pRole->setObjectName(QStringLiteral("Role"));
        m_pRole->setWordWrap(true);
        pCol->addWidget(m_pRole);
        pCol->addStretch(1);
    }
    pRow->addWidget(pPeopleColumn, kPeopleStretch);

    setMouseTracking(true); // the edge's cursor
    connect(pChat, &RoomChat::peopleChanged, this, [this](const QJsonArray& people) {
        m_pPeople->setPeople(people);
    });
    hide();
}

void RoomPanels::setRole(const QString& label, const QString& note) {
    m_pRoleTitle->setText(label);
    m_pRoleTitle->setVisible(!m_slim && !label.isEmpty());
    if (m_pRole->text() != note) {
        m_pRole->setText(note);
    }
    m_pRole->setVisible(!m_slim && !note.isEmpty());
}

void RoomPanels::setSlim(bool slim) {
    if (slim == m_slim) {
        return;
    }
    m_slim = slim;
    const int padY = slim ? 8 : kPadY;
    for (QBoxLayout* pCol : std::as_const(m_columns)) {
        pCol->setContentsMargins(kPadX, padY, kPadX, padY);
    }
    m_pSetTitle->setVisible(!slim);
    m_pSetList->setCompact(slim);
    m_pChat->setSlim(slim);
    m_pPeopleTitle->setVisible(!slim);
    m_pPeople->setVisible(!slim);
    m_pRoleTitle->setVisible(!slim && !m_pRoleTitle->text().isEmpty());
    m_pRole->setVisible(!slim && !m_pRole->text().isEmpty());
    setCursor(Qt::ArrowCursor);
    setToolTip(m_resizable ? tr("drag the bar up or down to make the panels taller or shorter") : QString());
}

namespace {
constexpr int kEdge = 8;    // px at the top that drag the panels' height
constexpr int kSnapPx = 40; // dragged to within this of their bar: down to just the bar
} // namespace

int& RoomPanels::overPx() {
    return m_overPx[LegacySkinParser::fourDecks() ? 1 : 0];
}

bool RoomPanels::onEdge(const QPoint& at) const {
    // The header bar (or, a skin without one, the top few px).
    return m_resizable && at.y() >= 0 && at.y() < std::max(kEdge, m_barPx);
}

QRect RoomPanels::placeFor(int overPx) const {
    QWidget* pWindow = window();
    if (!m_pPanel || !pWindow) {
        return geometry();
    }
    // The skin's panel area and its header bar: where they are at their
    // own size.
    QRect own(m_pPanel->mapTo(pWindow, QPoint(0, 0)), m_pPanel->size());
    if (m_pBar && m_pBar->isVisible()) {
        own = own.united(QRect(m_pBar->mapTo(pWindow, QPoint(0, 0)), m_pBar->size()));
    }
    if (!m_resizable || overPx <= 0 || overPx >= own.height()) {
        // Never taller than their own place: the decks stay clear.
        return own;
    }
    // Shorter (dragged down, the flyer wall above them), as far as just
    // their bar: hidden, the bar left to drag them back up.
    const int px = std::max(overPx, std::min(own.height(), std::max(m_barPx, kEdge)));
    return QRect(own.left(), own.bottom() + 1 - px, own.width(), px);
}

int RoomPanels::chosenHeight(int windowY) const {
    // From the dragged edge to the panels' bottom: no more than their own,
    // no less than their bar (close to it: snapped down to it).
    const QRect own = placeFor(0);
    const int least = std::min(own.height(), std::max(m_barPx, kEdge));
    const int px = std::clamp(own.bottom() + 1 - windowY, least, own.height());
    return px < least + kSnapPx ? least : px;
}

void RoomPanels::mousePressEvent(QMouseEvent* pEvent) {
    if (pEvent->button() == Qt::LeftButton && onEdge(pEvent->pos())) {
        // The edge: drag it, and the panels follow (over the decks).
        m_dragging = true;
        m_pressY = pEvent->pos().y();
        pEvent->accept();
        return;
    }
    QWidget::mousePressEvent(pEvent);
}

void RoomPanels::mouseMoveEvent(QMouseEvent* pEvent) {
    if (m_dragging) {
        // The edge stays under the pointer (as far as it can go).
        const int y = mapTo(window(), pEvent->pos()).y() - m_pressY;
        overPx() = chosenHeight(y);
        const QRect area = placeFor(overPx());
        if (geometry() != area) {
            setGeometry(area);
            setSlim(area.height() - m_barPx < kSlimBelow);
            placeWall();
            clipGlViews();
        }
        return;
    }
    setCursor(onEdge(pEvent->pos()) ? Qt::SizeVerCursor : Qt::ArrowCursor);
    QWidget::mouseMoveEvent(pEvent);
}

void RoomPanels::mouseReleaseEvent(QMouseEvent* pEvent) {
    if (m_dragging) {
        m_dragging = false;
        // Kept as it is now shown (no taller than there was room for).
        const int px = height() != placeFor(0).height() ? height() : 0;
        overPx() = px;
        const bool four = LegacySkinParser::fourDecks();
        QMetaObject::invokeMethod(this, [this, px, four] { emit heightChosen(px, four); }, Qt::QueuedConnection);
        return;
    }
    QWidget::mouseReleaseEvent(pEvent);
}

void RoomPanels::mouseDoubleClickEvent(QMouseEvent* pEvent) {
    // A double-click on the edge: their own size again.
    if (onEdge(pEvent->pos())) {
        overPx() = 0;
        setGeometry(placeFor(0));
        clipGlViews();
        const bool four = LegacySkinParser::fourDecks();
        QMetaObject::invokeMethod(this, [this, four] { emit heightChosen(0, four); }, Qt::QueuedConnection);
        return;
    }
    QWidget::mouseDoubleClickEvent(pEvent);
}

namespace {

#ifdef MIXXX_USE_QOPENGL
// An OpenGL view's own window, in its container: both are clipped (the
// container's mask alone leaves the view drawing on).
void setViewMask(WGLWidget* pGl, QWidget* pContainer, const QRegion& keep) {
    if (pContainer && pContainer->mask() != keep) {
        if (keep.isEmpty()) {
            pContainer->clearMask();
        } else {
            pContainer->setMask(keep);
        }
    }
    if (QWindow* pWindow = pGl->getOpenGLWindow()) {
        pWindow->setMask(keep);
    }
}

QWidget* containerOf(WGLWidget* pGl) {
    for (QWidget* pChild : pGl->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (QLatin1String(pChild->metaObject()->className()) == QLatin1String("QWindowContainer")) {
            return pChild;
        }
    }
    return nullptr;
}
#endif

} // namespace

void RoomPanels::clipGlViews() {
#ifdef MIXXX_USE_QOPENGL
    QWidget* pWindow = window();
    const bool over = pWindow && isVisible() && m_pPanel && height() > placeFor(0).height();
    QList<QPointer<QWidget>> clipped; // (the GL widgets)
    QList<QPointer<QWidget>> hidden;  // (their containers)
    if (over) {
        const QRect mine = geometry(); // (on the window)
        for (WGLWidget* pGl : WGLWidget::allWidgets()) {
            QWidget* pContainer = containerOf(pGl);
            const bool wasHidden = pContainer && m_hidden.contains(pContainer);
            if (!pGl->isVisibleTo(pWindow) || (pContainer && !pContainer->isVisible() && !wasHidden)) {
                continue; // not on show, or hidden by something else (a deck turning round)
            }
            const QRect at(pGl->mapTo(pWindow, QPoint(0, 0)), pGl->size());
            const QRect covered = at.intersected(mine);
            if (covered.isEmpty()) {
                continue;
            }
            if (covered == at && pContainer) {
                // All under the panels (an empty mask would be no mask).
                pContainer->hide();
                hidden.append(pContainer);
                continue;
            }
            if (wasHidden) {
                pContainer->show();
            }
            setViewMask(pGl, pContainer, QRegion(pGl->rect()) - QRegion(covered.translated(-at.topLeft())));
            clipped.append(pGl);
        }
    }
    // Out from under them: whole again.
    for (const QPointer<QWidget>& pView : std::as_const(m_clipped)) {
        if (pView && !clipped.contains(pView)) {
            auto* pGl = static_cast<WGLWidget*>(pView.data());
            setViewMask(pGl, containerOf(pGl), QRegion());
        }
    }
    for (const QPointer<QWidget>& pContainer : std::as_const(m_hidden)) {
        if (pContainer && !hidden.contains(pContainer)) {
            pContainer->show();
        }
    }
    m_clipped = clipped;
    m_hidden = hidden;
#endif
}

void RoomPanels::hideEvent(QHideEvent* pEvent) {
    QWidget::hideEvent(pEvent);
    clipGlViews(); // (not over anything now: all given back)
    placeWall();
}

void RoomPanels::showEvent(QShowEvent* pEvent) {
    QWidget::showEvent(pEvent);
    placeWall();
}

void RoomPanels::moveEvent(QMoveEvent* pEvent) {
    QWidget::moveEvent(pEvent);
    placeWall();
}

void RoomPanels::resizeEvent(QResizeEvent* pEvent) {
    QWidget::resizeEvent(pEvent);
    placeWall();
}

void RoomPanels::placeWall() {
    // The panels shorter than their place (two decks or four): the wall
    // fills what they leave above them (the posters as tall as the most
    // they leave).
    QWidget* pWindow = window();
    const QRect own = placeFor(0);
    const bool shown = isVisible() && pWindow && m_pPanel && height() < own.height() - 1;
    if (!shown) {
        if (m_pWall) {
            m_pWall->hide();
        }
        return;
    }
    // In the skin's own panel area (the booth is a native window for its
    // OpenGL views: a widget on the window itself would be under it).
    QWidget* pHome = m_pPanel->parentWidget();
    if (!pHome) {
        return;
    }
    if (!m_pWall) {
        m_pWall = new FlyerWall(pHome);
    } else if (m_pWall->parentWidget() != pHome) {
        m_pWall->setParent(pHome);
    }
    const int most = own.height() - std::min(own.height(), std::max(m_barPx, kEdge));
    m_pWall->setPosterHeight(std::max(0, most - 36));
    const QPoint at = pHome->mapFrom(pWindow, own.topLeft());
    m_pWall->setGeometry(QRect(at, QSize(own.width(), own.height() - height())));
    m_pWall->show();
    m_pWall->raise();
}

void RoomPanels::leaveEvent(QEvent* pEvent) {
    if (!m_dragging) {
        unsetCursor();
    }
    QWidget::leaveEvent(pEvent);
}

void RoomPanels::attachTo(QWidget* pBoothRoot, QWidget* pWindow) {
    // The skin's panel area (an older skin's four-deck slim strip if that
    // is what is on show).
    QWidget* pPanel = pBoothRoot ? pBoothRoot->findChild<QWidget*>(QStringLiteral("ChatPanel")) : nullptr;
    if ((!pPanel || !pPanel->isVisible()) && pBoothRoot) {
        pPanel = pBoothRoot->findChild<QWidget*>(QStringLiteral("ChatSlim"));
    }
    if (!pPanel || !pWindow || !pPanel->isVisible()) {
        hide();
        return;
    }
    m_pBooth = pBoothRoot->findChild<QWidget*>(QStringLiteral("BoothRow"));
    m_pBar = pBoothRoot->findChild<QWidget*>(QStringLiteral("ChatBar"));
    const int barPx = m_pBar && m_pBar->isVisible() ? m_pBar->height() : 0;
    if (barPx != m_barPx) {
        m_barPx = barPx;
        layout()->setContentsMargins(0, m_barPx, 0, 0); // the columns under the bar
        update();
    }
    // Compact (one line each) where there's little room for them (four
    // decks on a short screen, or dragged down).
    const int shown = m_dragging ? height() : placeFor(overPx()).height();
    setSlim(shown - m_barPx < kSlimBelow);
    if (parentWidget() != pWindow) {
        setParent(pWindow);
    }
    if (pPanel != m_pPanel) {
        m_pPanel = pPanel;
        // The skin's placeholder makes way.
        for (QWidget* pChild : pPanel->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
            pChild->hide();
        }
    }
    if (!m_dragging) {
        const QRect area = placeFor(overPx());
        if (geometry() != area) {
            setGeometry(area);
        }
    }
    if (!isVisible()) {
        show();
    }
    raise();
    clipGlViews();
}

void RoomPanels::paintEvent(QPaintEvent* pEvent) {
    QPainter p(this);
    // Under the booth, the blue coming back up from its deepest.
    QLinearGradient g(0, m_barPx, 0, height());
    g.setColorAt(0.0, QColor(theme::kBoothDeep));
    g.setColorAt(1.0, QColor(QStringLiteral("#0B1452")));
    p.fillRect(pEvent->rect(), g);
    // The header bar: their name, a line under it; dragged to move them
    // up over the decks.
    if (m_barPx > 0) {
        p.fillRect(QRect(0, 0, width(), m_barPx), QColor(theme::kBoothDeep));
        p.fillRect(QRect(0, m_barPx - 1, width(), 1), QColor(theme::kLineStrong));
        QFont label = theme::font(theme::Face::Label, 11);
        label.setLetterSpacing(QFont::AbsoluteSpacing, 1.3);
        p.setFont(label);
        p.setPen(QColor(theme::kTypeMuted));
        p.drawText(QRect(0, 0, width(), m_barPx - 1), Qt::AlignCenter,
                LegacySkinParser::fourDecks() ? tr("THE ROOM") : tr("CHAT"));
    }
    // Faint keylines between the columns.
    p.setPen(kColumnLine);
    const auto* pRow = layout();
    for (int i = 0; pRow && i < pRow->count() - 1; ++i) {
        const int x = pRow->itemAt(i)->geometry().right() + 1;
        p.drawLine(x, m_barPx, x, height());
    }
}

} // namespace mixxx::b2b
