#include "b2b/b2bexplorer.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QMimeData>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

#include "analyzer/analyzerscheduledtrack.h"
#include "control/controlobject.h"
#include "control/controlproxy.h"
#include "library/dao/playlistdao.h"
#include "library/library.h"
#include "library/playlisttablemodel.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "sources/soundsourceproxy.h"
#include "library/librarytablemodel.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "library/sidebarmodel.h"
#include "library/tabledelegates/coverartdelegate.h"
#include "library/trackmodel.h"
#include "library/trackset/tracksettablemodel.h"
#include "moc_b2bexplorer.cpp"
#include "widget/wlibrary.h"
#include "widget/wlibrarysidebar.h"
#include "widget/wsearchlineedit.h"
#include "widget/wtracktableview.h"

namespace mixxx::b2b {

namespace {

QPointer<LibraryExplorer> s_instance;

// ------------------------------------------------------------------ look --

// Right-click menus, in the screens' colours.
const QString kMenuStyle = QStringLiteral(
        "QMenu { background: #101623; color: #f5f7fb; border: 1px solid #26324a; padding: 4px; font-weight: 600; }"
        "QMenu::item { padding: 5px 18px; border-radius: 3px; }"
        "QMenu::item:selected { background: #2f6bff; color: #ffffff; }"
        "QMenu::item:disabled { color: #54607c; }"
        "QMenu::separator { height: 1px; background: #1a2336; margin: 4px 6px; }");
const QColor kBg(10, 14, 23);        // near black, a little navy
const QColor kPanel(16, 22, 35);     // cards, placeholders
const QColor kRail(7, 10, 17);       // the icon bar
const QColor kLine(26, 35, 54);      // dividers, borders
const QColor kAccent(47, 107, 255);  // the one blue
const QColor kAccentHi(84, 136, 255);
const QColor kHover(17, 27, 48);
const QColor kText(245, 247, 251);
const QColor kGrey(139, 149, 171);
const QColor kDim(84, 96, 124);

const char* const kStyle =
        "#B2BExplorer { background: #0a0e17; }"
        // The displays' type (bundled, res/fonts).
        "#B2BExplorer, #B2BExplorer * { font-family: \"Barlow Semi Condensed Medium\"; }"
        "#ExplorerHeading { font-family: \"Barlow Semi Condensed SemiBold\"; letter-spacing: 0.5px; }"
        "#ExplorerHead { background: #0a0e17; border-bottom: 1px solid #1a2336; }"
        "#ExplorerHeading { color: #f5f7fb; font-weight: 800; background: transparent; }"
        "#ExplorerCrumb { color: #8b95ab; font-weight: 600; background: transparent; }"
        "QToolButton#ExplorerBack, QToolButton#ExplorerTextSize { color: #f5f7fb; background: #111827;"
        " border: 1px solid #1f2a40; border-radius: 4px; padding: 0px 7px; font-weight: 700; }"
        "QToolButton#ExplorerBack:hover, QToolButton#ExplorerTextSize:hover { border-color: #2f6bff;"
        " background: #13203d; }"
        "QToolButton#ExplorerBack:pressed, QToolButton#ExplorerTextSize:pressed { background: #2f6bff; }"
        "#ExplorerList, #ExplorerCategories { background: #0a0e17; border: 0; outline: 0; }"
        "#ExplorerRail { background: #070a11; border-right: 1px solid #1a2336; }"
        "#ExplorerSource, #ExplorerEmpty, #ExplorerBrowse { background: #0a0e17; }"
        "#ExplorerHead QComboBox { background: #0f1522; color: #f5f7fb; border: 1px solid #1f2a40;"
        " border-radius: 4px; min-width: 80px; max-width: 120px; padding: 0 4px; }"
        "#ExplorerHead QComboBox:focus { border-color: #2f6bff; }"
        "#B2BExplorer QScrollBar:vertical { background: #0a0e17; width: 5px; margin: 0; border: 0; }"
        "#B2BExplorer QScrollBar::handle:vertical { background: #23314f; border-radius: 2px; min-height: 20px; }"
        "#B2BExplorer QScrollBar::add-line, #B2BExplorer QScrollBar::sub-line { height: 0; }"
        "#B2BExplorer QScrollBar::add-page, #B2BExplorer QScrollBar::sub-page { background: transparent; }";

// Icons, drawn (so they stay sharp at any size and take any colour).
enum Icon {
    IconNone,
    IconFolder,
    IconNote,
    IconArtist,
    IconAlbum,
    IconTrack,
    IconBpm,
    IconKey,
    IconRating,
    IconHistory,
    IconUsb,
    IconComputer,
    IconRecord,
    IconSearch,
    IconTagList,
    IconGenre,
    IconPlaylist,
};

void drawIcon(QPainter& p, int icon, QRectF r, const QColor& c) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double s = std::min(r.width(), r.height());
    r = QRectF(r.center().x() - s / 2, r.center().y() - s / 2, s, s);
    const double w = std::max(1.2, s / 11);
    QPen pen(c, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    auto P = [&r, s](double x, double y) {
        return QPointF(r.left() + x * s, r.top() + y * s);
    };
    switch (icon) {
    case IconFolder:
    case IconPlaylist: {
        QPainterPath path;
        path.moveTo(P(0.08, 0.28));
        path.lineTo(P(0.38, 0.28));
        path.lineTo(P(0.47, 0.38));
        path.lineTo(P(0.92, 0.38));
        path.lineTo(P(0.92, 0.80));
        path.lineTo(P(0.08, 0.80));
        path.closeSubpath();
        p.setBrush(QColor(c.red(), c.green(), c.blue(), 60));
        p.drawPath(path);
        if (icon == IconPlaylist) {
            p.drawLine(P(0.3, 0.55), P(0.7, 0.55));
            p.drawLine(P(0.3, 0.67), P(0.6, 0.67));
        }
        break;
    }
    case IconNote: {
        p.setBrush(c);
        p.drawEllipse(QRectF(P(0.22, 0.62), P(0.48, 0.84)));
        p.drawLine(P(0.46, 0.72), P(0.46, 0.16));
        p.drawLine(P(0.46, 0.16), P(0.78, 0.26));
        break;
    }
    case IconArtist: {
        p.drawEllipse(QRectF(P(0.34, 0.12), P(0.66, 0.44)));
        QPainterPath path;
        path.moveTo(P(0.16, 0.88));
        path.cubicTo(P(0.18, 0.56), P(0.82, 0.56), P(0.84, 0.88));
        p.drawPath(path);
        break;
    }
    case IconAlbum:
    case IconRecord: {
        p.drawEllipse(QRectF(P(0.1, 0.1), P(0.9, 0.9)));
        p.drawEllipse(QRectF(P(0.38, 0.38), P(0.62, 0.62)));
        if (icon == IconRecord) {
            p.drawArc(QRectF(P(0.22, 0.22), P(0.78, 0.78)), 30 * 16, 60 * 16);
        }
        break;
    }
    case IconTrack:
    case IconTagList: {
        for (int i = 0; i < 3; ++i) {
            const double y = 0.26 + i * 0.24;
            p.drawLine(P(0.34, y), P(0.9, y));
            if (icon == IconTagList) {
                p.drawLine(P(0.1, y), P(0.2, y));
            } else {
                p.drawPoint(P(0.14, y));
            }
        }
        break;
    }
    case IconBpm: {
        QPainterPath path; // a metronome
        path.moveTo(P(0.36, 0.12));
        path.lineTo(P(0.64, 0.12));
        path.lineTo(P(0.82, 0.88));
        path.lineTo(P(0.18, 0.88));
        path.closeSubpath();
        p.drawPath(path);
        p.drawLine(P(0.5, 0.72), P(0.74, 0.28));
        break;
    }
    case IconKey: {
        QFont f = p.font();
        f.setPixelSize(static_cast<int>(s * 0.78));
        f.setBold(true);
        p.setFont(f);
        p.drawText(r, Qt::AlignCenter, QStringLiteral("♯"));
        break;
    }
    case IconRating: {
        QPainterPath star;
        for (int i = 0; i < 10; ++i) {
            const double a = -M_PI / 2 + i * M_PI / 5;
            const double rad = (i % 2 == 0) ? 0.42 : 0.18;
            const QPointF pt = P(0.5 + rad * std::cos(a), 0.54 + rad * std::sin(a));
            if (i == 0) {
                star.moveTo(pt);
            } else {
                star.lineTo(pt);
            }
        }
        star.closeSubpath();
        p.drawPath(star);
        break;
    }
    case IconHistory: {
        p.drawEllipse(QRectF(P(0.12, 0.12), P(0.88, 0.88)));
        p.drawLine(P(0.5, 0.5), P(0.5, 0.26));
        p.drawLine(P(0.5, 0.5), P(0.68, 0.6));
        break;
    }
    case IconUsb: {
        p.drawRoundedRect(QRectF(P(0.3, 0.34), P(0.7, 0.9)), s * 0.06, s * 0.06);
        p.drawRect(QRectF(P(0.38, 0.1), P(0.62, 0.34)));
        p.drawLine(P(0.45, 0.2), P(0.45, 0.24));
        p.drawLine(P(0.55, 0.2), P(0.55, 0.24));
        break;
    }
    case IconComputer: {
        p.drawRoundedRect(QRectF(P(0.1, 0.16), P(0.9, 0.68)), s * 0.05, s * 0.05);
        p.drawLine(P(0.5, 0.68), P(0.5, 0.82));
        p.drawLine(P(0.3, 0.84), P(0.7, 0.84));
        break;
    }
    case IconSearch: {
        p.drawEllipse(QRectF(P(0.14, 0.14), P(0.62, 0.62)));
        p.drawLine(P(0.56, 0.56), P(0.86, 0.86));
        break;
    }
    case IconGenre: {
        // A tag.
        QPainterPath path;
        path.moveTo(P(0.12, 0.2));
        path.lineTo(P(0.52, 0.2));
        path.lineTo(P(0.88, 0.56));
        path.lineTo(P(0.56, 0.88));
        path.lineTo(P(0.12, 0.46));
        path.closeSubpath();
        p.drawPath(path);
        p.drawPoint(P(0.3, 0.36));
        break;
    }
    default:
        break;
    }
    p.restore();
}

// ------------------------------------------------------------- widgets --

/// A source card: icon tile, title, a line about it, and a tag.
class SourceCard : public QAbstractButton {
  public:
    SourceCard(int icon, const QString& name, const QString& about, bool recommended, QWidget* pParent)
            : QAbstractButton(pParent),
              m_icon(icon),
              m_name(name),
              m_about(about),
              m_recommended(recommended) {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setStatus(const QString& status) {
        m_status = status;
        update();
    }
    void setSelected(bool selected) {
        m_selected = selected;
        update();
    }
    bool selected() const {
        return m_selected;
    }
    QString title() const {
        return m_name;
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool hot = underMouse() || m_selected || hasFocus();
        p.setPen(QPen(hot ? kAccent : kLine, hot ? 1.5 : 1));
        p.setBrush(isDown() ? kHover : kPanel);
        p.drawRoundedRect(r, 6, 6);
        const double h = r.height();
        const double tile = std::min(h - 12, 44.0);
        const QRectF tileRect(r.left() + 8, r.center().y() - tile / 2, tile, tile);
        p.setPen(Qt::NoPen);
        p.setBrush(m_recommended ? kAccent : QColor(28, 40, 66));
        p.drawRoundedRect(tileRect, 5, 5);
        drawIcon(p, m_icon, tileRect.adjusted(tile * 0.2, tile * 0.2, -tile * 0.2, -tile * 0.2), kText);

        const double x = tileRect.right() + 10;
        QFont title = font();
        title.setBold(true);
        title.setPixelSize(std::clamp(static_cast<int>(h * 0.26), 10, 16));
        p.setFont(title);
        const QFontMetrics tm(title);
        const QRectF titleRect(x, r.top() + h * 0.16, r.right() - x - 8, tm.height());
        p.setPen(kText);
        p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, m_name);
        if (m_recommended) {
            QFont tagFont = font();
            tagFont.setBold(true);
            tagFont.setPixelSize(std::max(7, title.pixelSize() - 5));
            const QFontMetrics fm(tagFont);
            const QString tag = QObject::tr("RECOMMENDED");
            const double tw = fm.horizontalAdvance(tag) + 10;
            const QRectF tagRect(x + tm.horizontalAdvance(m_name) + 8,
                    titleRect.center().y() - fm.height() / 2 - 1, tw, fm.height() + 2);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(47, 107, 255, 50));
            p.drawRoundedRect(tagRect, 3, 3);
            p.setPen(kAccentHi);
            p.setFont(tagFont);
            p.drawText(tagRect, Qt::AlignCenter, tag);
        }
        QFont body = font();
        body.setPixelSize(std::max(8, title.pixelSize() - 3));
        p.setFont(body);
        p.setPen(m_status.isEmpty() ? kGrey : kAccentHi);
        const QRectF aboutRect(x, titleRect.bottom() + 2, r.right() - x - 22, r.bottom() - titleRect.bottom() - 6);
        p.drawText(aboutRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                m_status.isEmpty() ? m_about : m_status);
        p.setPen(kDim);
        QFont chevron = font();
        chevron.setPixelSize(title.pixelSize() + 4);
        p.setFont(chevron);
        p.drawText(r.adjusted(0, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("›"));
    }

  private:
    int m_icon;
    QString m_name;
    QString m_about;
    QString m_status;
    bool m_recommended;
    bool m_selected = false;
};

/// A shortcut in the slim icon bar: icon over a tiny label.
class RailButton : public QAbstractButton {
  public:
    RailButton(int icon, const QString& label, QWidget* pParent)
            : QAbstractButton(pParent),
              m_icon(icon) {
        setText(label);
        setToolTip(label);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    }
    void setActive(bool active) {
        m_active = active;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = rect();
        if (m_active || underMouse()) {
            p.fillRect(r, m_active ? QColor(47, 107, 255, 40) : kHover);
        }
        if (m_active) {
            p.fillRect(QRectF(r.left(), r.top(), 2, r.height()), kAccent);
        }
        const QColor c = m_active ? kAccentHi : (underMouse() ? kText : kGrey);
        if (r.height() < 26) {
            // No room for a label: the icon alone (the name is its tooltip).
            const double iconSize = std::min(r.width() * 0.45, r.height() * 0.72);
            drawIcon(p, m_icon, QRectF(r.center().x() - iconSize / 2, r.center().y() - iconSize / 2, iconSize, iconSize), c);
            return;
        }
        const double iconSize = std::min(r.width() * 0.42, r.height() * 0.48);
        const QRectF iconRect(r.center().x() - iconSize / 2, r.top() + r.height() * 0.12, iconSize, iconSize);
        drawIcon(p, m_icon, iconRect, c);
        QFont f = font();
        f.setPixelSize(std::clamp(static_cast<int>(r.height() * 0.2), 6, 9));
        f.setBold(true);
        p.setFont(f);
        p.setPen(c);
        p.drawText(QRectF(r.left(), iconRect.bottom(), r.width(), r.bottom() - iconRect.bottom()),
                Qt::AlignHCenter | Qt::AlignVCenter, text().toUpper());
    }

  private:
    int m_icon;
    bool m_active = false;
};

constexpr int kIconRole = Qt::UserRole + 10;
constexpr int kDetailRole = Qt::UserRole + 11;
constexpr int kFolderRole = Qt::UserRole + 12;
constexpr int kAccentRole = Qt::UserRole + 13;

/// A row in a list or the categories: folder icon, name, count, chevron.
class RowDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        return QSize(option.rect.width(), option.fontMetrics.height() * 2);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = option.rect;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        p->fillRect(r, selected ? kAccent : hover ? kHover : kBg);
        p->fillRect(QRect(r.left() + 6, r.bottom(), r.width() - 12, 1), kLine);
        const int h = r.height();
        const QRectF iconRect(r.left() + 10, r.top() + h * 0.22, h * 0.56, h * 0.56);
        const int icon = index.data(kIconRole).toInt();
        const bool accent = index.data(kAccentRole).toBool();
        if (icon != IconNone) {
            drawIcon(*p, icon, iconRect, selected ? kText : (accent ? kAccentHi : kAccentHi));
        }
        QFont f = option.font;
        f.setBold(true);
        p->setFont(f);
        const QString detail = index.data(kDetailRole).toString();
        const bool folder = index.data(kFolderRole).toBool();
        const int right = r.right() - (folder ? 22 : 10);
        QFont smallFont = option.font;
        smallFont.setPixelSize(std::max(7, option.font.pixelSize() - 1));
        const int detailW = detail.isEmpty() ? 0 : QFontMetrics(smallFont).horizontalAdvance(detail) + 10;
        const QRect textRect(static_cast<int>(iconRect.right()) + 10, r.top(),
                right - detailW - static_cast<int>(iconRect.right()) - 10, h);
        const bool idle = !(index.flags() & Qt::ItemIsSelectable); // a note, not a choice
        p->setPen(idle ? kGrey : accent && !selected ? kAccentHi : kText);
        p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(index.data().toString(), Qt::ElideRight, textRect.width()));
        if (!detail.isEmpty()) {
            p->setFont(smallFont);
            p->setPen(selected ? kText : kGrey);
            p->drawText(QRect(right - detailW, r.top(), detailW, h), Qt::AlignRight | Qt::AlignVCenter, detail);
        }
        if (folder) {
            p->setFont(f);
            p->setPen(selected ? kText : kDim);
            p->drawText(QRect(r.right() - 20, r.top(), 12, h), Qt::AlignCenter, QStringLiteral("›"));
        }
        p->restore();
    }
};

// ---------------------------------------------------------- track list --

/// Columns of the track list, as the table shows them (model columns).
struct TrackColumns {
    int title = -1;
    int artist = -1;
    int date = -1;
    int bpm = -1;
    int key = -1;
    int location = -1;
    int rating = -1;
    int cover = -1;
};

TrackColumns columnsOf(TrackModel* pModel) {
    TrackColumns c;
    if (!pModel) {
        return c;
    }
    using Id = TrackModel::SortColumnId;
    c.title = pModel->columnIndexFromSortColumnId(Id::Title);
    c.artist = pModel->columnIndexFromSortColumnId(Id::Artist);
    c.date = pModel->columnIndexFromSortColumnId(Id::DateTimeAdded);
    c.bpm = pModel->columnIndexFromSortColumnId(Id::Bpm);
    c.key = pModel->columnIndexFromSortColumnId(Id::Key);
    c.location = pModel->columnIndexFromSortColumnId(Id::Location);
    if (c.location < 0) {
        c.location = pModel->columnIndexFromSortColumnId(Id::NativeLocation);
    }
    c.rating = pModel->columnIndexFromSortColumnId(Id::Rating);
    c.cover = pModel->columnIndexFromSortColumnId(Id::CoverArt);
    return c;
}

/// "01 - my_track (extended mix).mp3" -> "my track (extended mix)".
QString tidyFileName(const QString& location) {
    QString name = QFileInfo(location).completeBaseName();
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    static const QRegularExpression kLeadingNumber(QStringLiteral("^\\s*\\d{1,3}\\s*[-.)_]\\s*"));
    name.remove(kLeadingNumber);
    name = name.simplified();
    return name;
}

/// Shared look of a track row's cells: blue when selected, dark otherwise,
/// a thin divider, no stripes or grid.
class CellDelegate : public QStyledItemDelegate {
  public:
    CellDelegate(QObject* pParent, int textPx)
            : QStyledItemDelegate(pParent),
              m_textPx(textPx) {
    }
    QWidget* createEditor(QWidget*, const QStyleOptionViewItem&, const QModelIndex&) const override {
        return nullptr; // nothing is edited here
    }

  protected:
    bool background(QPainter* p, const QStyleOptionViewItem& option) const {
        const bool selected = option.state & QStyle::State_Selected;
        const bool hover = option.state & QStyle::State_MouseOver;
        p->fillRect(option.rect, selected ? kAccent : hover ? kHover : kBg);
        p->fillRect(QRect(option.rect.left(), option.rect.bottom(), option.rect.width(), 1), kLine);
        return selected;
    }
    QFont font(const QStyleOptionViewItem& option, int delta, bool bold) const {
        QFont f = option.font;
        f.setPixelSize(std::max(7, m_textPx + delta));
        f.setBold(bold);
        return f;
    }
    int m_textPx;
};

/// The track column: number, artwork, a note, "Artist - Title".
class TitleDelegate : public CellDelegate {
  public:
    TitleDelegate(QTableView* pView, TrackModel* pModel, const TrackColumns& columns, int textPx)
            : CellDelegate(pView, textPx),
              m_columns(columns),
              m_pTrackModel(pModel) {
        if (columns.cover >= 0 && pModel) {
            m_pCover = new CoverArtDelegate(pView);
            connect(m_pCover, &CoverArtDelegate::rowsChanged, pView, [pView](const QList<int>&) {
                pView->viewport()->update();
            });
        }
    }
    /// Where the parts of the cell start, from its left (for the header).
    static int numberWidth(int textPx) {
        return textPx * 3;
    }
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool selected = background(p, option);
        const QRect r = option.rect;
        const int h = r.height();
        // Number.
        const int nw = numberWidth(m_textPx);
        p->setFont(font(option, -1, false));
        p->setPen(selected ? kText : kDim);
        p->drawText(QRect(r.left(), r.top(), nw, h), Qt::AlignCenter,
                QStringLiteral("%1").arg(index.row() + 1, 3, 10, QLatin1Char('0')));
        // Artwork, or a neat placeholder.
        const int art = h - 4;
        const QRect artRect(r.left() + nw, r.top() + 2, art, art);
        bool drew = false;
        if (m_pCover && m_pTrackModel &&
                m_pTrackModel->getCoverInfo(index.sibling(index.row(), m_columns.cover)).hasImage()) {
            QStyleOptionViewItem coverOption(option);
            coverOption.rect = artRect;
            QPainterPath clip;
            clip.addRoundedRect(artRect, 2, 2);
            p->save();
            p->setClipPath(clip);
            p->fillRect(artRect, kPanel);
            m_pCover->paintItem(p, coverOption, index.sibling(index.row(), m_columns.cover));
            p->restore();
            drew = true;
        }
        if (!drew) {
            p->setPen(Qt::NoPen);
            p->setBrush(selected ? QColor(255, 255, 255, 40) : kPanel);
            p->drawRoundedRect(artRect, 2, 2);
            drawIcon(*p, IconRecord, QRectF(artRect).adjusted(art * 0.22, art * 0.22, -art * 0.22, -art * 0.22),
                    selected ? kText : kDim);
        }
        // Note.
        const int noteSize = std::max(8, h / 2 - 1);
        const QRectF noteRect(artRect.right() + 6, r.top() + (h - noteSize) / 2.0, noteSize, noteSize);
        drawIcon(*p, IconNote, noteRect, selected ? kText : kAccentHi);
        // Artist - Title (or a tidied file name).
        QString title = index.data().toString().trimmed();
        if (title.isEmpty() && m_columns.location >= 0) {
            title = tidyFileName(index.sibling(index.row(), m_columns.location).data().toString());
        }
        const QString artist = m_columns.artist >= 0
                ? index.sibling(index.row(), m_columns.artist).data().toString().trimmed()
                : QString();
        const QString text = artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title;
        p->setFont(font(option, 0, true));
        p->setPen(kText);
        const QRect textRect(static_cast<int>(noteRect.right()) + 6, r.top(),
                r.right() - static_cast<int>(noteRect.right()) - 10, h);
        p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                p->fontMetrics().elidedText(text, Qt::ElideRight, textRect.width()));
        p->restore();
    }

  private:
    TrackColumns m_columns;
    TrackModel* m_pTrackModel;
    CoverArtDelegate* m_pCover = nullptr;
};

/// Date added, YYYY-MM-DD, small and grey.
class DateDelegate : public CellDelegate {
  public:
    using CellDelegate::CellDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        p->save();
        const bool selected = background(p, option);
        const QVariant value = index.data(Qt::EditRole);
        QDateTime when = value.toDateTime();
        QString text = when.isValid() ? when.toLocalTime().toString(QStringLiteral("yyyy-MM-dd"))
                                      : index.data().toString().left(10);
        p->setFont(font(option, -2, false));
        p->setPen(selected ? kText : kGrey);
        p->drawText(option.rect.adjusted(4, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter, text);
        p->restore();
    }
};

/// BPM with one decimal (no lock box).
class BpmDelegate : public CellDelegate {
  public:
    using CellDelegate::CellDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        p->save();
        const bool selected = background(p, option);
        bool ok = false;
        double bpm = index.data(Qt::EditRole).toDouble(&ok);
        if (!ok) {
            bpm = index.data().toString().toDouble(&ok);
        }
        p->setFont(font(option, -1, true));
        p->setPen(selected ? kText : (ok && bpm > 0 ? kText : kDim));
        p->drawText(option.rect.adjusted(4, 0, -6, 0), Qt::AlignRight | Qt::AlignVCenter,
                ok && bpm > 0 ? QString::number(bpm, 'f', 1) : QStringLiteral("--.-"));
        p->restore();
    }
};

/// The key, as the library writes it (Em, Ebm...).
class KeyDelegate : public CellDelegate {
  public:
    using CellDelegate::CellDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        p->save();
        const bool selected = background(p, option);
        const QString key = index.data().toString();
        p->setFont(font(option, -1, true));
        p->setPen(selected ? kText : (key.isEmpty() ? kDim : QColor(126, 224, 161)));
        p->drawText(option.rect.adjusted(6, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                key.isEmpty() ? QStringLiteral("--") : key);
        p->restore();
    }
};

} // namespace

/// The column header over the track list: #, artwork, TRACK (sort arrow and
/// count), DATE ADDED, BPM, KEY; the sorted column in blue. Clicking sorts.
class ColumnBar : public QWidget {
  public:
    explicit ColumnBar(QWidget* pParent)
            : QWidget(pParent) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setCursor(Qt::PointingHandCursor);
    }
    void setView(WTrackTableView* pView, const TrackColumns& columns, int textPx, int rows) {
        m_pView = pView;
        m_columns = columns;
        m_textPx = textPx;
        m_rows = rows;
        update();
    }

  protected:
    struct Cell {
        QString label;
        int column;
        int x;
        int w;
        Qt::Alignment align;
    };
    QList<Cell> cells() const {
        QList<Cell> out;
        if (!m_pView || !m_pView->model()) {
            return out;
        }
        const int nw = TitleDelegate::numberWidth(m_textPx);
        auto pos = [this](int column) {
            return m_pView->columnViewportPosition(column);
        };
        auto width = [this](int column) {
            return m_pView->columnWidth(column);
        };
        out.append(Cell{QStringLiteral("#"), -1, 0, nw, Qt::AlignCenter});
        if (m_columns.title >= 0) {
            const int rowH = m_pView->verticalHeader()->defaultSectionSize();
            const int start = nw + rowH - 4 + 6 + std::max(8, rowH / 2 - 1) + 6;
            out.append(Cell{QObject::tr("TRACK"), m_columns.title, pos(m_columns.title) + start,
                    width(m_columns.title) - start, Qt::AlignLeft});
        }
        if (m_columns.date >= 0 && !m_pView->isColumnHidden(m_columns.date)) {
            out.append(Cell{QObject::tr("DATE ADDED"), m_columns.date, pos(m_columns.date) + 4,
                    width(m_columns.date) - 4, Qt::AlignLeft});
        }
        if (m_columns.bpm >= 0) {
            out.append(Cell{QObject::tr("BPM"), m_columns.bpm, pos(m_columns.bpm), width(m_columns.bpm) - 6,
                    Qt::AlignRight});
        }
        if (m_columns.key >= 0) {
            out.append(Cell{QObject::tr("KEY"), m_columns.key, pos(m_columns.key) + 6, width(m_columns.key) - 6,
                    Qt::AlignLeft});
        }
        return out;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), kBg);
        p.fillRect(QRect(0, height() - 1, width(), 1), kLine);
        if (!m_pView) {
            return;
        }
        const int sorted = m_pView->horizontalHeader()->sortIndicatorSection();
        const bool ascending = m_pView->horizontalHeader()->sortIndicatorOrder() == Qt::AscendingOrder;
        const int offset = mapFromGlobal(m_pView->viewport()->mapToGlobal(QPoint(0, 0))).x();
        QFont f = font();
        f.setPixelSize(std::max(7, m_textPx - 2));
        f.setBold(true);
        p.setFont(f);
        for (const Cell& cell : cells()) {
            const bool active = cell.column >= 0 && cell.column == sorted;
            QString label = cell.label;
            if (cell.column == m_columns.title) {
                if (active) {
                    label += ascending ? QStringLiteral(" ▲") : QStringLiteral(" ▼");
                }
                label += QStringLiteral("  [%1]").arg(std::max(0, m_rows), 4, 10, QLatin1Char('0'));
            } else if (active) {
                label += ascending ? QStringLiteral(" ▲") : QStringLiteral(" ▼");
            }
            const QRect r(offset + cell.x, 0, std::max(0, cell.w), height() - 1);
            if (active) {
                p.fillRect(QRect(r.left() - 2, height() - 2, r.width() + 4, 2), kAccent);
            }
            p.setPen(active ? kAccentHi : kGrey);
            p.drawText(r, cell.align | Qt::AlignVCenter, p.fontMetrics().elidedText(label, Qt::ElideRight, r.width()));
        }
    }
    void mousePressEvent(QMouseEvent* pEvent) override {
        if (!m_pView) {
            return;
        }
        const int offset = mapFromGlobal(m_pView->viewport()->mapToGlobal(QPoint(0, 0))).x();
        for (const Cell& cell : cells()) {
            if (cell.column < 0) {
                continue;
            }
            if (pEvent->pos().x() >= offset + cell.x - 4 && pEvent->pos().x() < offset + cell.x + cell.w) {
                QHeaderView* pHeader = m_pView->horizontalHeader();
                Qt::SortOrder order = Qt::AscendingOrder;
                if (pHeader->sortIndicatorSection() == cell.column) {
                    order = pHeader->sortIndicatorOrder() == Qt::AscendingOrder ? Qt::DescendingOrder
                                                                                : Qt::AscendingOrder;
                }
                m_pView->sortByColumn(cell.column, order);
                update();
                return;
            }
        }
    }

  private:
    QPointer<WTrackTableView> m_pView;
    TrackColumns m_columns;
    int m_textPx = 12;
    int m_rows = 0;
};

// ---------------------------------------------------------------- setup --

LibraryExplorer::LibraryExplorer(QObject* pParent)
        : QObject(pParent) {
    s_instance = this;
    m_timer.setInterval(800);
    connect(&m_timer, &QTimer::timeout, this, &LibraryExplorer::refresh);
    m_timer.start();
    m_tick.setInterval(250);
    connect(&m_tick, &QTimer::timeout, this, &LibraryExplorer::tick);
    m_tick.start();
}

LibraryExplorer::~LibraryExplorer() = default;

void LibraryExplorer::setLibrary(Library* pLibrary) {
    m_pLibraryObject = pLibrary;
}

void LibraryExplorer::setSettingsPath(const QString& path) {
    m_settingsPath = path;
    QSettings ini(QDir(path).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
    m_textSize = std::clamp(ini.value(QStringLiteral("browser_text"), 1).toInt(), 0, 2);
}

void LibraryExplorer::setWindow(QWidget* pWindow) {
    m_pWindow = pWindow;
    // Screen keys like TRACK ▲ ▼ press these: on a list they move through it.
    if (!m_pMoveUp) {
        m_pMoveUp = new ControlProxy(QStringLiteral("[Library]"), QStringLiteral("MoveUp"), this,
                ControlFlag::NoWarnIfMissing);
        m_pMoveUp->connectValueChanged(this, [](double v) {
            if (v > 0) {
                moveSelection(-1);
            }
        });
        m_pMoveDown = new ControlProxy(QStringLiteral("[Library]"), QStringLiteral("MoveDown"), this,
                ControlFlag::NoWarnIfMissing);
        m_pMoveDown->connectValueChanged(this, [](double v) {
            if (v > 0) {
                moveSelection(1);
            }
        });
    }
    refresh();
}

void LibraryExplorer::refresh() {
    if (!m_pWindow) {
        return;
    }
    QWidget* pHost = m_pWindow->findChild<QWidget*>(QStringLiteral("B2BExplorer"));
    if (pHost && pHost != m_pHost) {
        build(pHost);
    }
    buildLoadBars();
}

int LibraryExplorer::textPx() const {
    // From the screen's height (less with four decks), and the Aa choice.
    const int screen = m_pHost && m_pHost->parentWidget() ? m_pHost->parentWidget()->height() : 260;
    const int base = std::clamp(screen / 19, 8, 14);
    return base + (m_textSize - 1) * 2;
}

int LibraryExplorer::rowHeight() const {
    return textPx() * 2 + 2;
}

void LibraryExplorer::build(QWidget* pHost) {
    QWidget* pBrowser = pHost->parentWidget();
    auto* pSidebar = pBrowser ? pBrowser->findChild<WLibrarySidebar*>() : nullptr;
    auto* pLibrary = pBrowser ? pBrowser->findChild<WLibrary*>() : nullptr;
    auto* pModel = pSidebar ? qobject_cast<SidebarModel*>(pSidebar->model()) : nullptr;
    if (!pSidebar || !pLibrary || !pModel) {
        return; // not bound yet: next time
    }
    m_pHost = pHost;
    m_pSidebar = pSidebar;
    m_pLibrary = pLibrary;
    m_pModel = pModel;
    m_configuredModel = nullptr;
    m_cards.clear();

    pHost->setStyleSheet(QString::fromLatin1(kStyle));
    pHost->setAttribute(Qt::WA_StyledBackground);
    auto* pLayout = qobject_cast<QBoxLayout*>(pHost->layout());
    if (!pLayout) {
        pLayout = new QVBoxLayout(pHost);
    }
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->setSpacing(0);

    // Header: back, the heading with where you are under it, text size, search.
    m_pHead = new QWidget(pHost);
    m_pHead->setObjectName(QStringLiteral("ExplorerHead"));
    m_pHead->setAttribute(Qt::WA_StyledBackground);
    auto* pRow = new QHBoxLayout(m_pHead);
    pRow->setContentsMargins(6, 3, 6, 3);
    pRow->setSpacing(6);
    auto* pTitles = new QVBoxLayout();
    pTitles->setSpacing(0);
    pTitles->setContentsMargins(0, 0, 0, 0);
    m_pHeading = new QLabel(m_pHead);
    m_pHeading->setObjectName(QStringLiteral("ExplorerHeading"));
    m_pHeading->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_pCrumb = new QLabel(m_pHead);
    m_pCrumb->setObjectName(QStringLiteral("ExplorerCrumb"));
    m_pCrumb->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pTitles->addWidget(m_pHeading);
    pTitles->addWidget(m_pCrumb);
    pRow->addLayout(pTitles, 1);
    if (auto* pSearch = pBrowser->findChild<WSearchLineEdit*>()) {
        m_pSearch = pSearch;
        pSearch->setParent(m_pHead);
        pRow->addWidget(pSearch);
    }
    m_pTextSize = new QToolButton(m_pHead);
    m_pTextSize->setObjectName(QStringLiteral("ExplorerTextSize"));
    m_pTextSize->setText(QStringLiteral("Aa"));
    m_pTextSize->setToolTip(tr("Text size"));
    m_pTextSize->setCursor(Qt::PointingHandCursor);
    connect(m_pTextSize, &QToolButton::clicked, this, &LibraryExplorer::cycleTextSize);
    pRow->addWidget(m_pTextSize);
    m_pBack = new QToolButton(m_pHead);
    m_pBack->setObjectName(QStringLiteral("ExplorerBack"));
    m_pBack->setText(tr("◀ BACK"));
    m_pBack->setCursor(Qt::PointingHandCursor);
    connect(m_pBack, &QToolButton::clicked, this, &LibraryExplorer::back);
    pRow->addWidget(m_pBack);
    pLayout->addWidget(m_pHead);

    m_pStack = new QStackedWidget(pHost);
    pLayout->addWidget(m_pStack, 1);

    // SOURCE: three cards.
    m_pSourcePage = new QWidget(m_pStack);
    m_pSourcePage->setObjectName(QStringLiteral("ExplorerSource"));
    auto* pCards = new QVBoxLayout(m_pSourcePage);
    pCards->setContentsMargins(8, 8, 8, 8);
    pCards->setSpacing(6);
    struct CardSpec {
        int icon;
        QString name;
        QString about;
        bool recommended;
        std::function<void()> open;
    };
    const QList<CardSpec> specs = {
            {IconRecord, tr("rekordbox"), tr("Your rekordbox library: tracks, playlists, cues"), true,
                    [this] {
                        connectRekordbox(false, true);
                    }},
            {IconUsb, tr("USB drive"), tr("Music on a plugged-in USB, or its rekordbox export"), false,
                    [this] {
                        connectRekordbox(true, true);
                    }},
            {IconComputer, tr("This computer"), tr("Choose a folder of music files"), false,
                    [this] {
                        connectComputer();
                    }},
    };
    for (const CardSpec& spec : specs) {
        auto* pCard = new SourceCard(spec.icon, spec.name, spec.about, spec.recommended, m_pSourcePage);
        connect(pCard, &QAbstractButton::clicked, this, spec.open);
        pCards->addWidget(pCard);
        m_cards.append(pCard);
    }
    m_pStack->addWidget(m_pSourcePage);

    // BROWSE: the icon bar and the categories.
    auto* pBrowse = new QWidget(m_pStack);
    pBrowse->setObjectName(QStringLiteral("ExplorerBrowse"));
    auto* pBrowseRow = new QHBoxLayout(pBrowse);
    pBrowseRow->setContentsMargins(0, 0, 0, 0);
    pBrowseRow->setSpacing(0);
    auto* pRail = new QWidget(pBrowse);
    pRail->setObjectName(QStringLiteral("ExplorerRail"));
    pRail->setAttribute(Qt::WA_StyledBackground);
    auto* pRailColumn = new QVBoxLayout(pRail);
    pRailColumn->setContentsMargins(0, 2, 0, 2);
    pRailColumn->setSpacing(0);
    const QList<QPair<int, QString>> rail = {{IconArtist, tr("Artist")}, {IconAlbum, tr("Album")},
            {IconTrack, tr("Track")}, {IconBpm, tr("BPM")}, {IconKey, tr("Key")}, {IconRating, tr("Rating")},
            {IconHistory, tr("History")}, {IconFolder, tr("Folder")}};
    for (int i = 0; i < rail.size(); ++i) {
        auto* pButton = new RailButton(rail[i].first, rail[i].second, pRail);
        pRailColumn->addWidget(pButton);
        connect(pButton, &QAbstractButton::clicked, this, [this, i] {
            pushBack([this] {
                showBrowse();
            });
            switch (i) {
            case 0:
                showValues(1);
                break;
            case 1:
                showValues(2);
                break;
            case 2:
                showTracks(m_libraryNode, tr("ALL TRACKS"), sourceName() + QStringLiteral(" › TRACK"));
                break;
            case 3:
                showValues(5);
                break;
            case 4:
                showValues(4);
                break;
            case 5: {
                showTracks(m_libraryNode, tr("BY RATING"), sourceName() + QStringLiteral(" › RATING"));
                QTimer::singleShot(300, this, [this] {
                    const TrackColumns c = columnsOf(trackModel());
                    if (trackView() && c.rating >= 0) {
                        trackView()->sortByColumn(c.rating, Qt::DescendingOrder);
                    }
                });
                break;
            }
            case 6:
                showTree(topNode(QStringLiteral("History")), tr("HISTORY"));
                break;
            default:
                showTree(topNode(QStringLiteral("Computer")), tr("FOLDER"));
                break;
            }
        });
    }
    pRail->setFixedWidth(40);
    pBrowseRow->addWidget(pRail);
    m_pCategories = new QListWidget(pBrowse);
    m_pCategories->setObjectName(QStringLiteral("ExplorerCategories"));
    m_pCategories->setItemDelegate(new RowDelegate(m_pCategories));
    m_pCategories->setMouseTracking(true);
    m_pCategories->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_pCategories, &QListWidget::itemClicked, this, [this](QListWidgetItem* pItem) {
        const int row = m_pCategories->row(pItem);
        if (row >= 0 && row < m_browseEntries.size() && m_browseEntries[row].open) {
            m_browseEntries[row].open();
        }
    });
    pBrowseRow->addWidget(m_pCategories, 1);
    m_pStack->addWidget(pBrowse);

    // A list (playlists, artists...).
    m_pList = new QListWidget(m_pStack);
    m_pList->setObjectName(QStringLiteral("ExplorerList"));
    m_pList->setItemDelegate(new RowDelegate(m_pList));
    m_pList->setMouseTracking(true);
    m_pList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_pList->setContextMenuPolicy(Qt::CustomContextMenu);
    acceptDrops(m_pList->viewport());
    connect(m_pList, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int row = m_pList->row(m_pList->itemAt(pos));
        if (row >= 0 && row < m_listEntries.size() && m_listEntries[row].playlistId >= 0) {
            playlistMenu(m_listEntries[row], m_pList->viewport()->mapToGlobal(pos));
        }
    });
    connect(m_pList, &QListWidget::itemClicked, this, [this](QListWidgetItem* pItem) {
        const int row = m_pList->row(pItem);
        if (row >= 0 && row < m_listEntries.size() && m_listEntries[row].open) {
            const auto open = m_listEntries[row].open; // it may replace the list
            open();
        }
    });
    m_pStack->addWidget(m_pList);

    // Nothing in a track list: say how to get music in.
    m_pEmpty = new QWidget(m_pStack);
    m_pEmpty->setObjectName(QStringLiteral("ExplorerEmpty"));
    auto* pEmptyColumn = new QVBoxLayout(m_pEmpty);
    pEmptyColumn->addStretch(1);
    auto* pEmptyCard = new SourceCard(IconNote, tr("No tracks here yet"), tr("Click here to load tracks"), false,
            m_pEmpty);
    m_pEmptyCard = pEmptyCard;
    pEmptyCard->setMinimumHeight(56); // (a button with no size of its own: the stretches would squash it)
    pEmptyCard->setMaximumHeight(64);
    connect(pEmptyCard, &QAbstractButton::clicked, this, [this] {
        pushBack([this] {
            showBrowse();
        });
        showSource();
    });
    pEmptyColumn->addWidget(pEmptyCard);
    pEmptyColumn->addStretch(1);
    pEmptyColumn->setContentsMargins(14, 8, 14, 8);
    m_pStack->addWidget(m_pEmpty);
    // Music dropped on an empty playlist goes into it.
    acceptDrops(m_pEmpty);
    acceptDrops(pEmptyCard);

    m_pColumns = new ColumnBar(pHost);
    pLayout->addWidget(m_pColumns);

    // TAG LIST / PLAYLIST keys pick an item in the (hidden) sidebar: show it.
    connect(pSidebar, &WLibrarySidebar::pressed, this, [this](const QModelIndex& index) {
        if (m_opening || !index.isValid()) {
            return;
        }
        const QPersistentModelIndex node(index);
        m_backStack.clear();
        pushBack([this] {
            openSource();
        });
        if (m_pModel->hasTrackTable(node) || m_pModel->rowCount(node) == 0) {
            showTracks(node, title(node), title(node).toUpper());
        } else {
            showTree(node, title(node).toUpper());
        }
    });

    // The library's own sidebar stays out of sight.
    if (QWidget* pParked = pBrowser->findChild<QWidget*>(QStringLiteral("BrowserParked"))) {
        pParked->hide();
    }
    applyTextSize();
    openSource();
}

void LibraryExplorer::buildLoadBars() {
    if (!m_pWindow) {
        return;
    }
    // Four players on screen (the 4-deck view), or two that turn round to
    // decks 3 and 4 (Side3): all four. Else two.
    const bool four = m_pWindow->findChild<QWidget*>(QStringLiteral("Booth3")) != nullptr ||
            m_pWindow->findChild<QWidget*>(QStringLiteral("Side3")) != nullptr;
    const auto bars = m_pWindow->findChildren<QWidget*>(QRegularExpression(QStringLiteral("^LoadBar\\d$")));
    for (QWidget* pBar : bars) {
        if (pBar->property("b2bBuilt").toBool()) {
            continue;
        }
        pBar->setProperty("b2bBuilt", true);
        const int own = pBar->objectName().right(1).toInt();
        auto* pRow = qobject_cast<QBoxLayout*>(pBar->layout());
        if (!pRow) {
            pRow = new QHBoxLayout(pBar);
        }
        pRow->setContentsMargins(0, 0, 0, 0);
        pRow->setSpacing(2);
        const int decks = four ? 4 : 2;
        for (int deck = 1; deck <= decks; ++deck) {
            auto* pButton = new QToolButton(pBar);
            const bool mine = deck == own;
            pButton->setText(mine ? tr("LOAD %1").arg(deck) : QString::number(deck));
            pButton->setToolTip(tr("Load the selected track to player %1").arg(deck));
            pButton->setCursor(Qt::PointingHandCursor);
            pButton->setSizePolicy(mine ? QSizePolicy::Expanding : QSizePolicy::Preferred, QSizePolicy::Expanding);
            pButton->setStyleSheet(mine
                            ? QStringLiteral("QToolButton { background: #2f6bff; color: #ffffff; border: 0;"
                                             " border-radius: 4px; font-weight: 800; padding: 0 10px; }"
                                             "QToolButton:hover { background: #5488ff; }")
                            : QStringLiteral("QToolButton { background: #0f1522; color: #f5f7fb; border: 1px solid"
                                             " #1f2a40; border-radius: 4px; font-weight: 800; min-width: 22px; }"
                                             "QToolButton:hover { border-color: #2f6bff; }"));
            connect(pButton, &QToolButton::clicked, this, [deck, own] {
                const ConfigKey load(QStringLiteral("[Channel%1]").arg(deck), QStringLiteral("LoadSelectedTrack"));
                ControlObject::set(load, 1.0);
                ControlObject::set(load, 0.0);
                if (deck == own) {
                    // Loaded here: back to the deck.
                    const ConfigKey view(QStringLiteral("[B2BBooth]"), QStringLiteral("deck%1_show_view").arg(own));
                    ControlObject::set(view, 1.0);
                    ControlObject::set(view, 0.0);
                }
            });
            pRow->insertWidget(pRow->count(), pButton, mine ? 3 : 1);
        }
    }
}

// --------------------------------------------------------------- finding --

QString LibraryExplorer::title(const QModelIndex& index) const {
    return index.data(Qt::DisplayRole).toString();
}

QModelIndex LibraryExplorer::topNode(const QString& name) const {
    if (!m_pModel) {
        return QModelIndex();
    }
    for (int r = 0; r < m_pModel->rowCount(); ++r) {
        const QModelIndex index = m_pModel->index(r, 0);
        QString label = title(index);
        if (label.startsWith(QLatin1Char('('))) {
            label = label.section(QLatin1Char(')'), 1).trimmed(); // "(loading) Rekordbox"
        }
        if (label.compare(name, Qt::CaseInsensitive) == 0) {
            return index;
        }
    }
    return QModelIndex();
}

WTrackTableView* LibraryExplorer::trackView() const {
    return m_pLibrary ? m_pLibrary->getCurrentTrackTableView() : nullptr;
}

TrackModel* LibraryExplorer::trackModel() const {
    WTrackTableView* pView = trackView();
    return pView ? dynamic_cast<TrackModel*>(pView->model()) : nullptr;
}

// ----------------------------------------------------------------- pages --

void LibraryExplorer::setHead(const QString& heading, const QString& crumb, bool back, bool upper) {
    if (!m_pHeading) {
        return;
    }
    // Lists keep their own names ("Am", "Friday warm-up"); pages shout.
    m_pHeading->setText(upper ? heading.toUpper() : heading);
    m_pCrumb->setText(crumb.toUpper());
    m_pCrumb->setVisible(!crumb.isEmpty());
    m_pBack->setVisible(back);
}

void LibraryExplorer::setPage(Page page) {
    m_page = page;
    const bool tracks = page == Page::Tracks;
    m_pStack->setVisible(!tracks);
    m_pColumns->setVisible(tracks);
    if (m_pSearch) {
        m_pSearch->setVisible(tracks);
    }
    if (m_pLibrary) {
        m_pLibrary->setVisible(tracks);
    }
    // LOAD buttons only where there are tracks to load.
    if (m_pWindow) {
        const auto bars = m_pWindow->findChildren<QWidget*>(QRegularExpression(QStringLiteral("^LoadBar\\d$")));
        for (QWidget* pBar : bars) {
            pBar->setVisible(tracks);
        }
    }
    // The track list takes the space below the header when it shows.
    m_pHost->setSizePolicy(QSizePolicy::Expanding, tracks ? QSizePolicy::Maximum : QSizePolicy::Expanding);
    m_pHost->setMaximumHeight(tracks ? m_pHead->sizeHint().height() + m_pColumns->height() : QWIDGETSIZE_MAX);
    switch (page) {
    case Page::Source:
        m_pStack->setCurrentWidget(m_pSourcePage);
        break;
    case Page::Browse:
        m_pStack->setCurrentWidget(m_pCategories->parentWidget());
        break;
    case Page::List:
        m_pStack->setCurrentWidget(m_pList);
        break;
    case Page::Tracks:
        break;
    }
}

void LibraryExplorer::pushBack(std::function<void()> fn) {
    m_backStack.append(std::move(fn));
}

void LibraryExplorer::back() {
    if (m_backStack.isEmpty()) {
        if (m_page != Page::Source) {
            showSource();
        }
        return;
    }
    const auto fn = m_backStack.takeLast();
    fn();
}

void LibraryExplorer::showSource() {
    if (!m_pHost) {
        return;
    }
    setHead(tr("Source"), tr("Where is your music?"), m_libraryNode.isValid() || m_source == SourceKind::Folders);
    m_backStack.clear();
    if (m_libraryNode.isValid() || m_source == SourceKind::Folders) {
        pushBack([this] {
            openSource();
        });
    }
    for (QWidget* pCard : std::as_const(m_cards)) {
        static_cast<SourceCard*>(pCard)->setStatus(QString());
    }
    setPage(Page::Source);
}

QString LibraryExplorer::sourceName() const {
    switch (m_source) {
    case SourceKind::Rekordbox:
        return QStringLiteral("MASTER.DB");
    case SourceKind::Usb:
        return m_libraryNode.isValid() ? title(m_libraryNode).toUpper() : tr("USB");
    case SourceKind::Computer:
        return tr("THIS COMPUTER");
    case SourceKind::Folders:
        return tr("USB");
    default:
        return QString();
    }
}

void LibraryExplorer::showBrowse() {
    if (!m_libraryNode.isValid()) {
        showSource();
        return;
    }
    m_backStack.clear();
    pushBack([this] {
        showSource();
    });
    const QString heading = m_source == SourceKind::Rekordbox
            ? tr("rekordbox")
            : m_source == SourceKind::Computer ? tr("This computer") : sourceName();
    setHead(heading, sourceName(), true);
    // Load the source's track list (for the counts) out of sight.
    m_opening = true;
    m_pModel->doubleClicked(m_libraryNode);
    m_pModel->clicked(m_libraryNode);
    m_opening = false;
    if (TrackModel* pModel = trackModel()) {
        pModel->search(QString());
    }
    computeCounts();

    const QString crumb = sourceName();
    auto count = [](int n) {
        return n < 0 ? QString() : QString::number(n);
    };
    m_browseEntries.clear();
    m_browseEntries.append(Entry{tr("Playlist"), count(m_counts.playlists), IconPlaylist, true, false, [this, crumb] {
                                     pushBack([this] {
                                         showBrowse();
                                     });
                                     if (m_source == SourceKind::Computer) {
                                         // Your playlists, straight away (no crates: a
                                         // player has playlists).
                                         const QPersistentModelIndex node(topNode(QStringLiteral("Playlists")));
                                         if (node.isValid()) {
                                             showTree(node, crumb + QStringLiteral(" › PLAYLIST"), tr("Playlist"));
                                         } else {
                                             showList(tr("Playlist"), crumb, {});
                                         }
                                     } else {
                                         showTree(m_libraryNode, crumb + QStringLiteral(" › PLAYLIST"), tr("Playlist"));
                                     }
                                 }});
    m_browseEntries.append(Entry{tr("Artist"), count(m_counts.artists), IconFolder, true, false, [this] {
                                     pushBack([this] {
                                         showBrowse();
                                     });
                                     showValues(1);
                                 }});
    m_browseEntries.append(Entry{tr("Album"), count(m_counts.albums), IconFolder, true, false, [this] {
                                     pushBack([this] {
                                         showBrowse();
                                     });
                                     showValues(2);
                                 }});
    m_browseEntries.append(Entry{tr("Track"), count(m_counts.tracks), IconFolder, true, false, [this, crumb] {
                                     pushBack([this] {
                                         showBrowse();
                                     });
                                     showTracks(m_libraryNode, tr("All tracks"), crumb + QStringLiteral(" › TRACK"));
                                 }});
    m_browseEntries.append(Entry{tr("Genre"), count(m_counts.genres), IconFolder, true, false, [this] {
                                     pushBack([this] {
                                         showBrowse();
                                     });
                                     showValues(3);
                                 }});
    m_browseEntries.append(Entry{tr("Key"), count(m_counts.keys), IconFolder, true, false, [this] {
                                     pushBack([this] {
                                         showBrowse();
                                     });
                                     showValues(4);
                                 }});
    m_browseEntries.append(Entry{tr("BPM"), count(m_counts.bpms), IconFolder, true, false, [this] {
                                     pushBack([this] {
                                         showBrowse();
                                     });
                                     showValues(5);
                                 }});
    m_browseEntries.append(Entry{tr("Search"), QString(), IconFolder, true, false, [this, crumb] {
                                     pushBack([this] {
                                         showBrowse();
                                     });
                                     showTracks(m_libraryNode, tr("Search"), crumb + QStringLiteral(" › SEARCH"));
                                     if (m_pSearch) {
                                         m_pSearch->setFocus();
                                     }
                                 }});
    const QModelIndex autoDj = topNode(QStringLiteral("Auto DJ"));
    if (autoDj.isValid()) {
        const QPersistentModelIndex node(autoDj);
        m_browseEntries.append(Entry{tr("Tag List"), QString(), IconFolder, true, false, [this, node, crumb] {
                                         pushBack([this] {
                                             showBrowse();
                                         });
                                         showTracks(node, tr("Tag list"), crumb + QStringLiteral(" › TAG LIST"));
                                     }});
    }
    const int keep = std::max(0, m_pCategories->currentRow());
    m_pCategories->clear();
    for (const Entry& entry : std::as_const(m_browseEntries)) {
        auto* pItem = new QListWidgetItem(entry.label, m_pCategories);
        pItem->setData(kIconRole, entry.icon);
        pItem->setData(kDetailRole, entry.detail);
        pItem->setData(kFolderRole, entry.folder);
    }
    m_pCategories->setCurrentRow(std::min(keep, m_pCategories->count() - 1));
    setPage(Page::Browse);
}

void LibraryExplorer::showList(const QString& heading, const QString& crumb, const QList<Entry>& entries) {
    setHead(heading, crumb, true);
    m_listEntries = entries;
    m_pList->clear();
    for (const Entry& entry : entries) {
        auto* pItem = new QListWidgetItem(entry.label, m_pList);
        pItem->setData(kIconRole, entry.icon);
        pItem->setData(kDetailRole, entry.detail);
        pItem->setData(kFolderRole, entry.folder);
        pItem->setData(kAccentRole, entry.accent);
        if (!entry.open) {
            pItem->setFlags(Qt::ItemIsEnabled); // "Nothing here yet" is not a choice
        }
    }
    if (m_pList->count() > 0 && (m_pList->item(0)->flags() & Qt::ItemIsSelectable)) {
        m_pList->setCurrentRow(0);
    }
    setPage(Page::List);
}

void LibraryExplorer::showTree(const QModelIndex& parentIndex, const QString& crumb, const QString& name) {
    if (!parentIndex.isValid() || !m_pModel) {
        return;
    }
    const QPersistentModelIndex parent(parentIndex);
    m_opening = true;
    m_pModel->doubleClicked(parent); // some lists fill in only when opened
    m_opening = false;
    QList<Entry> entries;
    const int rows = m_pModel->rowCount(parent);
    // A library or folder that lists tracks itself: all of them first.
    const bool library = parent.parent().isValid() && !parent.parent().parent().isValid();
    if ((m_pModel->hasTrackTable(parent) || library) && parent != m_libraryNode) {
        entries.append(Entry{tr("All tracks"), QString(), IconTrack, false, true, [this, parent, crumb, name] {
                                 pushBack([this, parent, crumb, name] {
                                     showTree(parent, crumb, name);
                                 });
                                 showTracks(parent, title(parent), crumb);
                             }});
    }
    const QPersistentModelIndex playlistsRoot(topNode(QStringLiteral("Playlists")));
    const bool yourPlaylists = playlistsRoot.isValid() && parent == playlistsRoot;
    m_refreshList = [this, parent, crumb, name] {
        showTree(parent, crumb, name);
    };
    if (yourPlaylists) {
        entries.append(Entry{tr("New playlist"), QString(), IconPlaylist, false, true, [this] {
                                 newPlaylist(m_refreshList);
                             }});
        entries.append(Entry{tr("New playlist from a folder…"), QString(), IconFolder, false, true, [this] {
                                 newPlaylistFromFolder(m_refreshList);
                             }});
    }
    // rekordbox / a USB: its playlists are read-only; yours are here.
    if (parent == m_libraryNode && playlistsRoot.isValid() &&
            (m_source == SourceKind::Rekordbox || m_source == SourceKind::Usb)) {
        entries.append(Entry{tr("My playlists"), QString::number(m_pModel->rowCount(playlistsRoot)),
                IconFolder, true, false, [this, playlistsRoot, parent, crumb, name] {
                    pushBack([this, parent, crumb, name] {
                        showTree(parent, crumb, name);
                    });
                    showTree(playlistsRoot, crumb + QStringLiteral(" › MY PLAYLISTS"), tr("My playlists"));
                }});
    }
    if (!parent.parent().isValid() && title(parent).contains(QStringLiteral("Rekordbox"), Qt::CaseInsensitive)) {
        entries.append(Entry{tr("Choose a rekordbox folder or USB export…"), QString(), IconFolder, false, true, [] {
                                 RekordboxFeature::chooseFolder();
                             }});
    }
    for (int r = 0; r < rows; ++r) {
        const QPersistentModelIndex child(m_pModel->index(r, 0, parent));
        const bool folder = m_pModel->hasChildren(child);
        const int count = folder ? m_pModel->rowCount(child) : -1;
        entries.append(Entry{title(child), count > 0 ? QString::number(count) : QString(),
                folder ? IconFolder : IconPlaylist, folder, false, [this, parent, child, crumb, folder, name] {
                    pushBack([this, parent, crumb, name] {
                        showTree(parent, crumb, name);
                    });
                    m_opening = true;
                    m_pModel->doubleClicked(child);
                    m_opening = false;
                    // A folder lists what's in it; anything else is a list
                    // of tracks (a playlist has no rows of its own in the
                    // sidebar, and "has a track table" is only ever true of
                    // a whole feature, not of a playlist in it).
                    if (folder || m_pModel->rowCount(child) > 0 || title(child).startsWith(QLatin1Char('('))) {
                        showTree(child, crumb + QStringLiteral(" › ") + title(child).toUpper());
                        return;
                    }
                    showTracks(child, title(child), crumb);
                }});
        if (yourPlaylists) {
            entries.last().playlistId = m_pModel->data(child, SidebarModel::DataRole).toInt();
        }
    }
    if (entries.isEmpty()) {
        entries.append(Entry{title(parent).startsWith(QLatin1Char('(')) ? tr("looking") : tr("nothing here yet"),
                QString(), IconNone, false, false, nullptr});
    }
    QString heading = name.isEmpty() ? title(parent) : name;
    if (heading.startsWith(QLatin1Char('('))) {
        heading = heading.section(QLatin1Char(')'), 1).trimmed();
    }
    showList(heading, crumb, entries);
    // It may still be filling in (a device being read): list again soon.
    if (rows == 0) {
        QTimer::singleShot(700, this, [this, parent, crumb, name] {
            if (m_page == Page::List && parent.isValid() && m_pModel->rowCount(parent) > 0 &&
                    m_pHeading->text().compare(name.isEmpty() ? title(parent).section(QLatin1Char(')'), -1).trimmed() : name,
                            Qt::CaseInsensitive) == 0) {
                showTree(parent, crumb, name);
            }
        });
    }
}

void LibraryExplorer::showTracks(
        const QModelIndex& nodeIndex, const QString& heading, const QString& crumb, const QString& filter) {
    if (!nodeIndex.isValid() || !m_pModel) {
        return;
    }
    const QPersistentModelIndex node(nodeIndex);
    // A playlist's sidebar label has its count and length: "Warm-up (12) 48:10".
    static const QRegularExpression kCounts(QStringLiteral("\\s*\\(\\d+\\)(\\s+[\\d:]+)?$"));
    const QString cleanHeading = QString(heading).remove(kCounts);
    m_trackHeading = cleanHeading;
    m_trackCrumb = crumb;
    m_pendingFilter = filter;
    m_opening = true;
    m_pModel->doubleClicked(node);
    m_pModel->clicked(node);
    m_opening = false;
    if (TrackModel* pModel = trackModel()) {
        pModel->search(filter);
    }
    m_configuredModel = nullptr; // set the table up for this list
    setHead(cleanHeading, crumb, true, false);
    setPage(Page::Tracks);
    tick();
}

void LibraryExplorer::newPlaylist(const std::function<void()>& refresh) {
    if (!m_pLibraryObject) {
        return;
    }
    PlaylistDAO& playlists = m_pLibraryObject->trackCollectionManager()->internalCollection()->getPlaylistDAO();
    bool ok = false;
    const QString name = QInputDialog::getText(m_pWindow, tr("New playlist"), tr("Name of the playlist:"),
            QLineEdit::Normal, QString(), &ok)
                                 .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    if (playlists.getPlaylistIdFromName(name) >= 0) {
        QMessageBox::information(m_pWindow, tr("New playlist"), tr("There's already a playlist called \"%1\".").arg(name));
        return;
    }
    if (playlists.createPlaylist(name) < 0) {
        QMessageBox::warning(m_pWindow, tr("New playlist"), tr("The playlist couldn't be made."));
        return;
    }
    // The sidebar lists it a moment later.
    QTimer::singleShot(250, this, [refresh] {
        if (refresh) {
            refresh();
        }
    });
}

void LibraryExplorer::playlistMenu(const Entry& entry, const QPoint& globalPos) {
    if (!m_pLibraryObject) {
        return;
    }
    PlaylistDAO& playlists = m_pLibraryObject->trackCollectionManager()->internalCollection()->getPlaylistDAO();
    const int id = entry.playlistId;
    const QString name = playlists.getPlaylistName(id); // (the list shows it with its count)
    QMenu menu(m_pWindow);
    menu.setStyleSheet(kMenuStyle);
    QAction* pRename = menu.addAction(tr("Rename…"));
    QAction* pDelete = menu.addAction(tr("Delete"));
    const bool locked = playlists.isPlaylistLocked(id);
    pRename->setEnabled(!locked);
    pDelete->setEnabled(!locked);
    QAction* pChosen = menu.exec(globalPos);
    const auto refresh = m_refreshList;
    if (pChosen == pRename) {
        bool ok = false;
        const QString newName = QInputDialog::getText(m_pWindow, tr("Rename playlist"), tr("New name:"),
                QLineEdit::Normal, name, &ok)
                                        .trimmed();
        if (!ok || newName.isEmpty() || newName == name) {
            return;
        }
        if (playlists.getPlaylistIdFromName(newName) >= 0) {
            QMessageBox::information(m_pWindow, tr("Rename playlist"),
                    tr("There's already a playlist called \"%1\".").arg(newName));
            return;
        }
        playlists.renamePlaylist(id, newName);
    } else if (pChosen == pDelete) {
        if (QMessageBox::question(m_pWindow, tr("Delete playlist"),
                    tr("Delete the playlist \"%1\"? The tracks stay in your library.").arg(name)) !=
                QMessageBox::Yes) {
            return;
        }
        playlists.deletePlaylist(id);
    } else {
        return;
    }
    QTimer::singleShot(250, this, [refresh] {
        if (refresh) {
            refresh();
        }
    });
}

void LibraryExplorer::trackMenu(const QPoint& globalPos) {
    WTrackTableView* pView = trackView();
    TrackModel* pTrackModel = trackModel();
    if (!pView || !pTrackModel || !m_pLibraryObject) {
        return;
    }
    QModelIndexList rows = pView->selectionModel() ? pView->selectionModel()->selectedRows() : QModelIndexList();
    if (rows.isEmpty() && pView->currentIndex().isValid()) {
        rows.append(pView->currentIndex());
    }
    if (rows.isEmpty()) {
        return;
    }
    PlaylistDAO& playlists = m_pLibraryObject->trackCollectionManager()->internalCollection()->getPlaylistDAO();
    // (Asked only when needed: a track from rekordbox joins the library then.)
    auto trackIds = [pTrackModel, rows] {
        QList<TrackId> ids;
        for (const QModelIndex& index : rows) {
            const TrackId id = pTrackModel->getTrackId(index);
            if (id.isValid()) {
                ids.append(id);
            }
        }
        return ids;
    };
    QMenu menu(m_pWindow);
    menu.setStyleSheet(kMenuStyle);
    QMenu* pAdd = menu.addMenu(rows.size() == 1 ? tr("Add to playlist") : tr("Add %1 tracks to playlist").arg(rows.size()));
    auto lists = playlists.getPlaylists(PlaylistDAO::PLHT_NOT_HIDDEN);
    std::sort(lists.begin(), lists.end(), [](const auto& a, const auto& b) {
        return a.second.compare(b.second, Qt::CaseInsensitive) < 0;
    });
    for (const auto& [id, name] : std::as_const(lists)) {
        if (playlists.isPlaylistLocked(id)) {
            continue;
        }
        const int playlistId = id;
        connect(pAdd->addAction(name), &QAction::triggered, this, [&playlists, playlistId, trackIds] {
            playlists.appendTracksToPlaylist(trackIds(), playlistId);
        });
    }
    if (!pAdd->isEmpty()) {
        pAdd->addSeparator();
    }
    connect(pAdd->addAction(tr("New playlist…")), &QAction::triggered, this, [this, &playlists, trackIds] {
        bool ok = false;
        const QString name = QInputDialog::getText(m_pWindow, tr("New playlist"), tr("Name of the playlist:"),
                QLineEdit::Normal, QString(), &ok)
                                     .trimmed();
        if (!ok || name.isEmpty()) {
            return;
        }
        int id = playlists.getPlaylistIdFromName(name);
        if (id < 0) {
            id = playlists.createPlaylist(name);
        }
        if (id >= 0) {
            playlists.appendTracksToPlaylist(trackIds(), id);
        }
    });
    connect(menu.addAction(tr("Add to the tag list")), &QAction::triggered, this, [&playlists, trackIds] {
        playlists.addTracksToAutoDJQueue(trackIds(), PlaylistDAO::AutoDJSendLoc::BOTTOM);
    });
    // In one of your playlists (or the tag list): take tracks out of it.
    if (qobject_cast<PlaylistTableModel*>(pView->model()) &&
            (pTrackModel->hasCapabilities(TrackModel::Capability::Remove) ||
                    pTrackModel->hasCapabilities(TrackModel::Capability::RemovePlaylist))) {
        menu.addSeparator();
        connect(menu.addAction(tr("Remove from this list")), &QAction::triggered, this, [pTrackModel, rows] {
            pTrackModel->removeTracks(rows);
        });
    }
    menu.exec(globalPos);
}

namespace {

/// The music files among dropped/chosen paths, folders opened (all the way
/// down), in name order.
QStringList musicFiles(const QStringList& paths) {
    QStringList files;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        if (info.isDir()) {
            QStringList inFolder;
            QDirIterator it(path, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString file = it.next();
                if (SoundSourceProxy::isFileNameSupported(file)) {
                    inFolder.append(file);
                }
            }
            std::sort(inFolder.begin(), inFolder.end(), [](const QString& a, const QString& b) {
                return a.compare(b, Qt::CaseInsensitive) < 0;
            });
            files += inFolder;
        } else if (info.isFile() && SoundSourceProxy::isFileNameSupported(path)) {
            files.append(path);
        }
    }
    return files;
}

QStringList droppedPaths(const QMimeData* pMime) {
    QStringList paths;
    if (pMime && pMime->hasUrls()) {
        const auto urls = pMime->urls();
        for (const QUrl& url : urls) {
            if (url.isLocalFile()) {
                paths.append(url.toLocalFile());
            }
        }
    }
    return paths;
}

} // namespace

void LibraryExplorer::acceptDrops(QWidget* pWidget) {
    if (pWidget) {
        pWidget->setAcceptDrops(true);
        pWidget->installEventFilter(this);
    }
}

int LibraryExplorer::currentPlaylistId() const {
    WTrackTableView* pView = trackView();
    auto* pModel = pView ? qobject_cast<PlaylistTableModel*>(pView->model()) : nullptr;
    return m_page == Page::Tracks && pModel ? pModel->getPlaylist() : -1;
}

int LibraryExplorer::createUniquePlaylist(const QString& name) {
    PlaylistDAO& playlists = m_pLibraryObject->trackCollectionManager()->internalCollection()->getPlaylistDAO();
    QString unique = name.trimmed().isEmpty() ? tr("New playlist") : name.trimmed();
    const QString base = unique;
    for (int n = 2; playlists.getPlaylistIdFromName(unique) >= 0; ++n) {
        unique = QStringLiteral("%1 %2").arg(base).arg(n);
    }
    return playlists.createPlaylist(unique);
}

void LibraryExplorer::addToPlaylist(int playlistId, const QStringList& files) {
    if (!m_pLibraryObject || playlistId < 0 || files.isEmpty()) {
        return;
    }
    PlaylistDAO& playlists = m_pLibraryObject->trackCollectionManager()->internalCollection()->getPlaylistDAO();
    if (playlists.isPlaylistLocked(playlistId)) {
        QMessageBox::information(m_pWindow, tr("Add to playlist"), tr("That playlist is locked."));
        return;
    }
    // New files join the library on the way (read in one go).
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QList<TrackId> resolved = m_pLibraryObject->trackCollectionManager()->resolveTrackIdsFromLocations(files);
    // Not twice (the same folder dropped again).
    const QList<TrackId> already = playlists.getTrackIds(playlistId);
    const QSet<TrackId> have(already.begin(), already.end());
    QList<TrackId> ids;
    for (const TrackId& id : resolved) {
        if (!have.contains(id) && !ids.contains(id)) {
            ids.append(id);
        }
    }
    playlists.appendTracksToPlaylist(ids, playlistId);
    QApplication::restoreOverrideCursor();
    qInfo() << "B2B: added" << ids.size() << "tracks to playlist" << playlistId << "("
             << resolved.size() - ids.size() << "already in it)";
}

void LibraryExplorer::newPlaylistFromFolder(const std::function<void()>& refresh) {
    if (!m_pLibraryObject) {
        return;
    }
    const QString folder = QFileDialog::getExistingDirectory(m_pWindow, tr("Choose a folder of music for the playlist"));
    if (folder.isEmpty()) {
        return;
    }
    const QStringList files = musicFiles({folder});
    if (files.isEmpty()) {
        QMessageBox::information(m_pWindow, tr("New playlist"), tr("There's no music in that folder."));
        return;
    }
    const int id = createUniquePlaylist(QFileInfo(folder).fileName());
    if (id < 0) {
        QMessageBox::warning(m_pWindow, tr("New playlist"), tr("The playlist couldn't be made."));
        return;
    }
    addToPlaylist(id, files);
    QTimer::singleShot(250, this, [refresh] {
        if (refresh) {
            refresh();
        }
    });
}

bool LibraryExplorer::eventFilter(QObject* pObject, QEvent* pEvent) {
    const QEvent::Type type = pEvent->type();
    if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::Drop) {
        return QObject::eventFilter(pObject, pEvent);
    }
    auto* pDrop = static_cast<QDropEvent*>(pEvent);
    if (droppedPaths(pDrop->mimeData()).isEmpty()) {
        return false; // not files (e.g. a track dragged within the list): as usual
    }
    // Where it goes: a playlist row in the list, or the playlist on screen.
    int playlistId = -1;
    bool newPlaylist = false;
    if (m_pList && pObject == m_pList->viewport()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint pos = pDrop->position().toPoint();
#else
        const QPoint pos = pDrop->pos();
#endif
        const int row = m_pList->row(m_pList->itemAt(pos));
        if (row >= 0 && row < m_listEntries.size()) {
            playlistId = m_listEntries[row].playlistId;
            newPlaylist = playlistId < 0 && m_listEntries[row].label == tr("New playlist");
        }
    } else {
        playlistId = currentPlaylistId();
    }
    if (playlistId < 0 && !newPlaylist) {
        return false; // not a playlist here: the list's own handling (or none)
    }
    if (type != QEvent::Drop) {
        pDrop->setDropAction(Qt::CopyAction);
        pDrop->accept();
        return true;
    }
    pDrop->setDropAction(Qt::CopyAction);
    pDrop->accept();
    const QStringList paths = droppedPaths(pDrop->mimeData());
    // Handle it once the drag is over (a folder can take a moment).
    QTimer::singleShot(0, this, [this, paths, playlistId, newPlaylist] {
        const QStringList files = musicFiles(paths);
        if (files.isEmpty()) {
            QMessageBox::information(m_pWindow, tr("Add to playlist"), tr("No music files there."));
            return;
        }
        int id = playlistId;
        if (newPlaylist) {
            const QFileInfo first(paths.first());
            id = createUniquePlaylist(first.isDir() ? first.fileName() : tr("New playlist"));
        }
        addToPlaylist(id, files);
        if (newPlaylist && m_refreshList) {
            const auto refresh = m_refreshList;
            QTimer::singleShot(250, this, [refresh] { refresh(); });
        }
    });
    return true;
}

void LibraryExplorer::showValues(int category) {
    // 1 artist, 2 album, 3 genre, 4 key, 5 BPM: the values in the source's
    // tracks, each with how many tracks have it.
    if (!m_libraryNode.isValid()) {
        return;
    }
    m_opening = true;
    m_pModel->clicked(m_libraryNode);
    m_opening = false;
    TrackModel* pTrackModel = trackModel();
    auto* pModel = trackView() ? trackView()->model() : nullptr;
    if (!pTrackModel || !pModel) {
        return;
    }
    pTrackModel->search(QString());
    using Id = TrackModel::SortColumnId;
    const Id ids[] = {Id::Artist, Id::Artist, Id::Album, Id::Genre, Id::Key, Id::Bpm};
    const QString names[] = {QString(), tr("Artist"), tr("Album"), tr("Genre"), tr("Key"), tr("BPM")};
    const QString fields[] = {QString(), QStringLiteral("artist"), QStringLiteral("album"),
            QStringLiteral("genre"), QStringLiteral("key"), QStringLiteral("bpm")};
    const int column = pTrackModel->columnIndexFromSortColumnId(ids[category]);
    if (column < 0) {
        return;
    }
    QMap<QString, int> counts;
    QMap<QString, QString> sortKey;
    for (int row = 0; row < pModel->rowCount(); ++row) {
        const QModelIndex index = pModel->index(row, column);
        QString value;
        if (category == 5) {
            const double bpm = index.data(Qt::EditRole).toDouble();
            if (bpm <= 0) {
                continue;
            }
            value = QString::number(static_cast<int>(std::floor(bpm)));
            sortKey[value] = QStringLiteral("%1").arg(static_cast<int>(std::floor(bpm)), 4, 10, QLatin1Char('0'));
        } else {
            value = index.data().toString().trimmed();
            if (value.isEmpty()) {
                continue;
            }
            sortKey[value] = value.toLower();
        }
        counts[value] += 1;
    }
    QList<QString> values = counts.keys();
    std::sort(values.begin(), values.end(), [&sortKey](const QString& a, const QString& b) {
        return sortKey.value(a) < sortKey.value(b);
    });
    const QString crumb = sourceName() + QStringLiteral(" › ") + names[category].toUpper();
    QList<Entry> entries;
    for (const QString& value : values) {
        QString filter;
        if (category == 5) {
            filter = QStringLiteral("bpm:%1-%2.99").arg(value, value);
        } else {
            QString clean = value;
            clean.remove(QLatin1Char('"'));
            filter = QStringLiteral("%1:\"%2\"").arg(fields[category], clean);
        }
        const QString label = category == 5 ? value + QStringLiteral(" BPM") : value;
        entries.append(Entry{label, QString::number(counts.value(value)), category == 4 ? IconKey : IconFolder,
                true, false, [this, label, crumb, filter, category] {
                    pushBack([this, category] {
                        showValues(category);
                    });
                    showTracks(m_libraryNode, label, crumb, filter);
                }});
    }
    if (entries.isEmpty()) {
        entries.append(Entry{tr("Nothing here yet"), QString(), IconNone, false, false, nullptr});
    }
    showList(names[category], crumb, entries);
}

// --------------------------------------------------------------- sources --

void LibraryExplorer::saveSource() {
    if (m_settingsPath.isEmpty()) {
        return;
    }
    QSettings ini(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
    ini.setValue(QStringLiteral("browser_source"), static_cast<int>(m_source));
}

void LibraryExplorer::openSource() {
    // Where the browser opens: the last source, if it's still there.
    QSettings ini(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
    const auto saved = static_cast<SourceKind>(ini.value(QStringLiteral("browser_source"), 0).toInt());
    if (m_libraryNode.isValid() && m_source != SourceKind::None) {
        showBrowse();
        return;
    }
    switch (saved) {
    case SourceKind::Computer: {
        const QModelIndex tracks = topNode(QStringLiteral("Tracks"));
        if (tracks.isValid()) {
            m_source = SourceKind::Computer;
            m_libraryNode = tracks;
            showBrowse();
            return;
        }
        break;
    }
    case SourceKind::Rekordbox:
    case SourceKind::Usb:
        showSource();
        connectRekordbox(saved == SourceKind::Usb, false);
        return;
    default:
        break;
    }
    showSource();
}

void LibraryExplorer::connectRekordbox(bool usb, bool interactive) {
    const QModelIndex root = topNode(QStringLiteral("Rekordbox"));
    auto* pCard = static_cast<SourceCard*>(m_cards.value(usb ? 1 : 0));
    if (!root.isValid()) {
        if (usb) {
            connectFolders();
        } else if (pCard) {
            pCard->setStatus(tr("rekordbox support isn't in this build"));
        }
        return;
    }
    if (pCard) {
        pCard->setStatus(usb ? tr("looking for usb drives.") : tr("looking for your rekordbox library."));
    }
    // Rekordbox's own feature looks for the collection and USB exports.
    m_opening = true;
    m_pModel->clicked(root);
    m_pModel->doubleClicked(root);
    m_opening = false;
    waitForRekordbox(usb, interactive, interactive ? 25 : 40);
}

void LibraryExplorer::waitForRekordbox(bool usb, bool interactive, int triesLeft, bool mayAsk) {
    const QModelIndex root = topNode(QStringLiteral("Rekordbox"));
    if (!root.isValid() || !m_pModel) {
        return;
    }
    QModelIndex found;
    for (int r = 0; r < m_pModel->rowCount(root); ++r) {
        const QModelIndex child = m_pModel->index(r, 0, root);
        const QString label = title(child);
        const bool collection = label.contains(QStringLiteral("rekordbox library"), Qt::CaseInsensitive);
        if (collection != usb && !label.startsWith(QLatin1Char('('))) {
            found = child;
            break;
        }
    }
    auto* pCard = static_cast<SourceCard*>(m_cards.value(usb ? 1 : 0));
    if (found.isValid()) {
        m_source = usb ? SourceKind::Usb : SourceKind::Rekordbox;
        m_libraryNode = found;
        if (pCard) {
            pCard->setStatus(QString());
        }
        saveSource();
        // Reading it takes a moment; BROWSE fills in its counts as it does.
        m_opening = true;
        m_pModel->doubleClicked(found);
        m_pModel->clicked(found);
        m_opening = false;
        showBrowse();
        QTimer::singleShot(1500, this, [this] {
            if (m_page == Page::Browse) {
                showBrowse();
            }
        });
        return;
    }
    if (triesLeft > 0) {
        QTimer::singleShot(250, this, [this, usb, interactive, triesLeft, mayAsk] {
            if (m_page == Page::Source) {
                waitForRekordbox(usb, interactive, triesLeft - 1, mayAsk);
            }
        });
        return;
    }
    if (!interactive) {
        // Opening the browser: not there now (e.g. the USB is unplugged).
        if (pCard) {
            pCard->setStatus(QString());
        }
        return;
    }
    if (usb) {
        // No rekordbox export on a USB: its files and folders.
        connectFolders();
        return;
    }
    // Ask for the folder once per click on the card: never again by itself
    // (cancelling the folder window used to bring it straight back).
    if (!mayAsk) {
        if (pCard) {
            pCard->setStatus(tr("No rekordbox library in that folder. Click to choose another."));
        }
        return;
    }
    if (pCard) {
        pCard->setStatus(tr("not found here. choose your rekordbox folder."));
    }
    if (RekordboxFeature::chooseFolder()) {
        waitForRekordbox(false, true, 25, false);
    } else if (pCard) {
        pCard->setStatus(tr("Click to look again, or to choose your rekordbox folder."));
    }
}

void LibraryExplorer::connectComputer() {
    const QString folder = QFileDialog::getExistingDirectory(m_pWindow, tr("Choose a folder of music"));
    if (folder.isEmpty()) {
        return;
    }
    if (m_pLibraryObject) {
        // Already in the library (it or a folder it's in): nothing to add
        // (Mixxx would say it can't add it).
        const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(folder));
        bool inLibrary = false;
        const QStringList roots =
                m_pLibraryObject->trackCollectionManager()->internalCollection()->getRootDirStrings();
        for (const QString& root : roots) {
            const QString r = QDir::cleanPath(QDir::fromNativeSeparators(root));
            if (clean.compare(r, Qt::CaseInsensitive) == 0 ||
                    clean.startsWith(r + QLatin1Char('/'), Qt::CaseInsensitive)) {
                inLibrary = true;
                break;
            }
        }
        if (!inLibrary) {
            m_pLibraryObject->requestAddDir(folder); // the library reads it in (and keeps it up to date)
        }
    }
    const QModelIndex tracks = topNode(QStringLiteral("Tracks"));
    if (!tracks.isValid()) {
        return;
    }
    m_source = SourceKind::Computer;
    m_libraryNode = tracks;
    saveSource();
    showBrowse();
}

void LibraryExplorer::connectFolders() {
    const QModelIndex computer = topNode(QStringLiteral("Computer"));
    if (!computer.isValid()) {
        return;
    }
    m_opening = true;
    m_pModel->doubleClicked(computer);
    m_opening = false;
    QModelIndex devices;
    for (int r = 0; r < m_pModel->rowCount(computer); ++r) {
        const QModelIndex child = m_pModel->index(r, 0, computer);
        if (title(child).contains(QStringLiteral("Devices"), Qt::CaseInsensitive)) {
            devices = child;
            if (title(child).contains(QStringLiteral("Removable"), Qt::CaseInsensitive)) {
                break;
            }
        }
    }
    m_source = SourceKind::Folders;
    m_libraryNode = QPersistentModelIndex();
    saveSource();
    m_backStack.clear();
    pushBack([this] {
        showSource();
    });
    showTree(devices.isValid() ? devices : computer, tr("USB"));
}

// ------------------------------------------------------------ track list --

void LibraryExplorer::computeCounts() {
    m_counts = Counts{};
    TrackModel* pTrackModel = trackModel();
    auto* pModel = trackView() ? trackView()->model() : nullptr;
    if (!pTrackModel || !pModel) {
        return;
    }
    using Id = TrackModel::SortColumnId;
    const int artist = pTrackModel->columnIndexFromSortColumnId(Id::Artist);
    const int album = pTrackModel->columnIndexFromSortColumnId(Id::Album);
    const int genre = pTrackModel->columnIndexFromSortColumnId(Id::Genre);
    const int key = pTrackModel->columnIndexFromSortColumnId(Id::Key);
    const int bpm = pTrackModel->columnIndexFromSortColumnId(Id::Bpm);
    QSet<QString> artists, albums, genres, keys;
    QSet<int> bpms;
    const int rows = pModel->rowCount();
    for (int row = 0; row < rows; ++row) {
        auto text = [pModel, row](int column) {
            return column < 0 ? QString() : pModel->index(row, column).data().toString().trimmed();
        };
        if (const QString v = text(artist); !v.isEmpty()) {
            artists.insert(v.toLower());
        }
        if (const QString v = text(album); !v.isEmpty()) {
            albums.insert(v.toLower());
        }
        if (const QString v = text(genre); !v.isEmpty()) {
            genres.insert(v.toLower());
        }
        if (const QString v = text(key); !v.isEmpty()) {
            keys.insert(v);
        }
        if (bpm >= 0) {
            const double b = pModel->index(row, bpm).data(Qt::EditRole).toDouble();
            if (b > 0) {
                bpms.insert(static_cast<int>(std::floor(b)));
            }
        }
    }
    m_counts.tracks = rows;
    m_counts.artists = static_cast<int>(artists.size());
    m_counts.albums = static_cast<int>(albums.size());
    m_counts.genres = static_cast<int>(genres.size());
    m_counts.keys = static_cast<int>(keys.size());
    m_counts.bpms = static_cast<int>(bpms.size());
    // Playlists: every list under the source (folders don't count).
    std::function<int(const QModelIndex&)> lists = [this, &lists](const QModelIndex& parent) {
        int n = 0;
        for (int r = 0; r < m_pModel->rowCount(parent); ++r) {
            const QModelIndex child = m_pModel->index(r, 0, parent);
            n += m_pModel->rowCount(child) > 0 ? lists(child) : 1;
        }
        return n;
    };
    if (m_source == SourceKind::Computer) {
        m_counts.playlists = 0;
        const QModelIndex node = topNode(QStringLiteral("Playlists"));
        if (node.isValid()) {
            m_counts.playlists = m_pModel->rowCount(node);
        }
    } else if (m_libraryNode.isValid()) {
        m_counts.playlists = lists(m_libraryNode);
    }
}

void LibraryExplorer::configureTable() {
    WTrackTableView* pView = trackView();
    TrackModel* pTrackModel = trackModel();
    if (!pView || !pTrackModel || !pView->model()) {
        return;
    }
    const TrackColumns c = columnsOf(pTrackModel);
    QHeaderView* pHeader = pView->horizontalHeader();
    const bool fresh = m_configuredModel != pView->model() || m_configuredView != pView;
    if (fresh) {
        m_configuredModel = pView->model();
        m_configuredView = pView;
        // Lists with their own controls (Auto DJ's buttons): just the tracks.
        if (QWidget* pPane = pView->parentWidget(); pPane && pPane != m_pLibrary) {
            const auto children = pPane->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
            for (QWidget* pChild : children) {
                if (pChild != pView && !pChild->isAncestorOf(pView) && !pChild->isWindow()) {
                    pChild->hide();
                }
            }
        }
        pView->setShowGrid(false);
        pView->setAlternatingRowColors(false);
        pView->setWordWrap(false);
        pView->setMouseTracking(true);
        pView->setFrameShape(QFrame::NoFrame);
        // A short menu of our own on right-click, not Mixxx's long one
        // (which can hide tracks or move their files to the trash).
        if (!pView->property("b2bMenu").toBool()) {
            pView->setProperty("b2bMenu", true);
            pView->viewport()->installEventFilter(this); // drops: folders too
            pView->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(pView, &QWidget::customContextMenuRequested, this, [this, pView](const QPoint& pos) {
                trackMenu(pView->viewport()->mapToGlobal(pos));
            });
        }
        // Whatever the skin's own table looks like (some are light).
        pView->setStyleSheet(QStringLiteral(
                "QTableView { background-color: #0a0e17; border: none; }"
                "QScrollBar:vertical { background: #0a0e17; width: 6px; margin: 0; border: none; }"
                "QScrollBar::handle:vertical { background: #26324a; border-radius: 3px; min-height: 24px; }"
                "QScrollBar::add-line, QScrollBar::sub-line { height: 0; border: none; }"
                "QScrollBar::add-page, QScrollBar::sub-page { background: none; }"));
        pView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        pView->verticalHeader()->hide();
        pHeader->hide();
        // Mixxx deletes a column's delegate when it shows another list, so
        // each list gets new ones.
        const int px = textPx();
        if (c.title >= 0) {
            pView->setItemDelegateForColumn(c.title, new TitleDelegate(pView, pTrackModel, c, px));
        }
        if (c.date >= 0) {
            pView->setItemDelegateForColumn(c.date, new DateDelegate(pView, px));
        }
        if (c.bpm >= 0) {
            pView->setItemDelegateForColumn(c.bpm, new BpmDelegate(pView, px));
        }
        if (c.key >= 0) {
            pView->setItemDelegateForColumn(c.key, new KeyDelegate(pView, px));
        }
        m_analysisQueued.clear();
    }
    // Only Track, Date added, BPM, Key, in that order (Mixxx may restore its
    // own column layout when a list is shown again: checked every time).
    const QList<int> wanted = {c.title, c.date, c.bpm, c.key};
    int visual = 0;
    for (int column : wanted) {
        if (column < 0) {
            continue;
        }
        if (pHeader->isSectionHidden(column)) {
            pHeader->showSection(column);
        }
        if (pHeader->visualIndex(column) != visual) {
            pHeader->moveSection(pHeader->visualIndex(column), visual);
        }
        ++visual;
    }
    for (int column = 0; column < pView->model()->columnCount(); ++column) {
        if (!wanted.contains(column) && !pHeader->isSectionHidden(column)) {
            pHeader->hideSection(column);
        }
    }
    const int px = textPx();
    const int row = rowHeight();
    if (pView->verticalHeader()->defaultSectionSize() != row) {
        pView->verticalHeader()->setMinimumSectionSize(row);
        pView->verticalHeader()->setDefaultSectionSize(row);
    }
    const int bpmW = c.bpm >= 0 ? px * 4 + 4 : 0;
    const int keyW = c.key >= 0 ? px * 3 + 4 : 0;
    // The title comes first: on a small screen the date gets just enough.
    const bool tight = pView->viewport()->width() - bpmW - keyW - px * 7 < px * 16;
    const int dateW = c.date >= 0 ? (tight ? px * 6 : px * 7) : 0;
    const int titleW = std::max(px * 10, pView->viewport()->width() - dateW - bpmW - keyW);
    auto setWidth = [pHeader](int column, int width) {
        if (column >= 0 && pHeader->sectionSize(column) != width) {
            pHeader->resizeSection(column, width);
        }
    };
    setWidth(c.title, titleW);
    setWidth(c.date, dateW);
    setWidth(c.bpm, bpmW);
    setWidth(c.key, keyW);
    QFont f = pView->font();
    f.setPixelSize(px);
    if (pView->font() != f) {
        pView->setFont(f);
    }
    m_pColumns->setFixedHeight(px + 10);
    m_pColumns->setView(pView, c, px, pView->model()->rowCount());
}

void LibraryExplorer::queueAnalysis() {
    // Tracks with no BPM or key yet: analyse them (in the background) so the
    // list fills in.
    TrackModel* pTrackModel = trackModel();
    auto* pModel = trackView() ? trackView()->model() : nullptr;
    if (!pTrackModel || !pModel || !m_pLibraryObject) {
        return;
    }
    // Only lists of the Mixxx library's own tracks. Elsewhere (rekordbox,
    // iTunes, a folder...) asking for a track's id adds it to the library,
    // reading the file, and doing that for every row froze (and crashed) the
    // app; those tracks are analysed when they're loaded.
    if (!qobject_cast<LibraryTableModel*>(pModel) && !qobject_cast<TrackSetTableModel*>(pModel)) {
        return;
    }
    const TrackColumns c = columnsOf(pTrackModel);
    QList<AnalyzerScheduledTrack> tracks;
    const int rows = std::min(pModel->rowCount(), 2000);
    for (int row = 0; row < rows; ++row) {
        const bool noBpm = c.bpm >= 0 && pModel->index(row, c.bpm).data(Qt::EditRole).toDouble() <= 0;
        const bool noKey = c.key >= 0 && pModel->index(row, c.key).data().toString().isEmpty();
        if (!noBpm && !noKey) {
            continue;
        }
        const TrackId id = pTrackModel->getTrackId(pModel->index(row, 0));
        if (!id.isValid()) {
            continue; // not in the library (yet): analysed when loaded
        }
        const qint64 key = id.toVariant().toLongLong();
        if (m_analysisQueued.contains(key)) {
            continue;
        }
        m_analysisQueued.insert(key);
        tracks.append(AnalyzerScheduledTrack(id));
    }
    if (!tracks.isEmpty()) {
        emit m_pLibraryObject->analyzeTracks(tracks);
    }
}

void LibraryExplorer::tick() {
    if (!m_pHost || m_page != Page::Tracks || !m_pHost->isVisible()) {
        return;
    }
    TrackModel* pTrackModel = trackModel();
    if (pTrackModel && m_configuredModel != trackView()->model()) {
        // A list read in the background (a rekordbox device) arrived.
        pTrackModel->search(m_pendingFilter);
    }
    configureTable();
    auto* pModel = trackView() ? trackView()->model() : nullptr;
    const bool empty = !pModel || pModel->rowCount() == 0;
    // (A list that can't be searched shows "- - -" in a disabled box.)
    const bool searching = m_pSearch && m_pSearch->isEnabled() && !m_pSearch->currentText().isEmpty();
    if (m_pSearch && m_pSearch->isVisible() != m_pSearch->isEnabled()) {
        m_pSearch->setVisible(m_pSearch->isEnabled());
    }
    // Nothing to show: say how to get music in (not while a search finds nothing).
    const bool showEmpty = empty && !searching && m_pendingFilter.isEmpty();
    if (showEmpty && m_pEmptyCard) {
        static_cast<SourceCard*>(m_pEmptyCard.data())
                ->setStatus(currentPlaylistId() >= 0 ? tr("Drag music files or folders here to add them") : QString());
    }
    if (showEmpty != m_pStack->isVisible()) {
        m_pStack->setVisible(showEmpty);
        m_pStack->setCurrentWidget(m_pEmpty);
        m_pColumns->setVisible(!showEmpty);
        if (m_pLibrary) {
            m_pLibrary->setVisible(!showEmpty);
        }
        if (m_pWindow) { // nothing to load
            const auto bars = m_pWindow->findChildren<QWidget*>(QRegularExpression(QStringLiteral("^LoadBar\\d$")));
            for (QWidget* pBar : bars) {
                pBar->setVisible(!showEmpty);
            }
        }
        m_pHost->setSizePolicy(QSizePolicy::Expanding, showEmpty ? QSizePolicy::Expanding : QSizePolicy::Maximum);
        m_pHost->setMaximumHeight(showEmpty ? QWIDGETSIZE_MAX : m_pHead->sizeHint().height() + m_pColumns->height());
    } else if (!showEmpty) {
        m_pHost->setMaximumHeight(m_pHead->sizeHint().height() + m_pColumns->height());
    }
    if (!empty) {
        // Like a CDJ, there's always a track under the cursor.
        WTrackTableView* pView = trackView();
        if (pView->selectionModel() && !pView->selectionModel()->hasSelection()) {
            pView->selectRow(0);
        }
        queueAnalysis();
    }
}

void LibraryExplorer::applyTextSize() {
    if (!m_pHost) {
        return;
    }
    const int px = textPx();
    QFont listFont = m_pList->font();
    listFont.setPixelSize(px);
    m_pList->setFont(listFont);
    m_pCategories->setFont(listFont);
    QFont heading = m_pHeading->font();
    heading.setPixelSize(px + 4);
    heading.setBold(true);
    m_pHeading->setFont(heading);
    QFont smallFont = m_pCrumb->font();
    smallFont.setPixelSize(std::max(7, px - 3));
    m_pCrumb->setFont(smallFont);
    m_pBack->setFont(smallFont);
    m_pTextSize->setFont(smallFont);
    if (m_pSearch) {
        m_pSearch->setFont(smallFont);
        m_pSearch->setFixedHeight(px + 8);
    }
    for (QWidget* pCard : std::as_const(m_cards)) {
        pCard->update();
    }
    m_configuredModel = nullptr; // rows and delegates again
    m_pList->doItemsLayout();
    m_pCategories->doItemsLayout();
}

void LibraryExplorer::cycleTextSize() {
    m_textSize = (m_textSize + 1) % 3;
    if (!m_settingsPath.isEmpty()) {
        QSettings ini(QDir(m_settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
        ini.setValue(QStringLiteral("browser_text"), m_textSize);
    }
    applyTextSize();
    tick();
}

// ------------------------------------------------------------ the knob --

bool LibraryExplorer::moveSelection(int delta) {
    LibraryExplorer* pSelf = s_instance;
    if (!pSelf || !pSelf->m_pHost || !pSelf->m_pHost->isVisible()) {
        return false;
    }
    QListWidget* pList = pSelf->m_page == Page::Browse ? pSelf->m_pCategories.data()
            : pSelf->m_page == Page::List              ? pSelf->m_pList.data()
                                                       : nullptr;
    if (pSelf->m_page == Page::Source) {
        // Move between the cards.
        int current = 0;
        for (int i = 0; i < pSelf->m_cards.size(); ++i) {
            if (static_cast<SourceCard*>(pSelf->m_cards[i])->selected()) {
                current = i;
            }
        }
        const int next = std::clamp(current + delta, 0, static_cast<int>(pSelf->m_cards.size()) - 1);
        for (int i = 0; i < pSelf->m_cards.size(); ++i) {
            static_cast<SourceCard*>(pSelf->m_cards[i])->setSelected(i == next);
        }
        return true;
    }
    if (pSelf->m_page == Page::Tracks) {
        // The track list: the cursor moves, like a CDJ's.
        WTrackTableView* pView = pSelf->trackView();
        QAbstractItemModel* pModel = pView ? pView->model() : nullptr;
        if (!pModel || pModel->rowCount() == 0) {
            return true;
        }
        const int row = pView->currentIndex().isValid() ? pView->currentIndex().row() : -1;
        const int next = std::clamp(row + delta, 0, pModel->rowCount() - 1);
        pView->selectRow(next);
        pView->scrollTo(pModel->index(next, 0));
        return true;
    }
    if (!pList) {
        return false;
    }
    const int count = pList->count();
    if (count > 0) {
        pList->setCurrentRow(std::clamp(pList->currentRow() + delta, 0, count - 1));
    }
    return true;
}

bool LibraryExplorer::snapshot(Snapshot* pOut) {
    LibraryExplorer* pSelf = s_instance;
    if (!pSelf || !pSelf->m_pHost || !pSelf->m_pHost->isVisible() || !pOut) {
        return false;
    }
    Snapshot snap;
    for (int n = 1; n <= 4; ++n) {
        if (ControlObject::get(ConfigKey(QStringLiteral("[B2BBooth]"), QStringLiteral("deck%1_browsing").arg(n))) > 0) {
            snap.deck = n;
            break;
        }
    }
    if (snap.deck == 0) {
        return false;
    }
    snap.heading = pSelf->m_pHeading ? pSelf->m_pHeading->text() : QString();
    snap.crumb = pSelf->m_pCrumb ? pSelf->m_pCrumb->text() : QString();
    constexpr int kRows = 10;
    auto fromList = [&snap](QListWidget* pList) {
        const int count = pList->count();
        const int current = pList->currentRow();
        const int first = std::clamp(current - kRows / 2, 0, std::max(0, count - kRows));
        for (int r = first; r < std::min(count, first + kRows); ++r) {
            QListWidgetItem* pItem = pList->item(r);
            snap.rows.append(pItem->text() + QLatin1Char('\t') + pItem->data(kDetailRole).toString());
        }
        snap.current = current >= first ? current - first : -1;
    };
    switch (pSelf->m_page) {
    case Page::Source:
        for (int i = 0; i < pSelf->m_cards.size(); ++i) {
            auto* pCard = static_cast<SourceCard*>(pSelf->m_cards[i]);
            snap.rows.append(pCard->title() + QLatin1Char('\t'));
            if (pCard->selected()) {
                snap.current = i;
            }
        }
        break;
    case Page::Browse:
        fromList(pSelf->m_pCategories);
        break;
    case Page::List:
        fromList(pSelf->m_pList);
        break;
    case Page::Tracks: {
        snap.tracks = true;
        WTrackTableView* pView = pSelf->trackView();
        TrackModel* pTrackModel = pSelf->trackModel();
        QAbstractItemModel* pModel = pView ? pView->model() : nullptr;
        if (!pModel || !pTrackModel || !pView->isVisible()) {
            break; // (the "load tracks" prompt)
        }
        const TrackColumns c = columnsOf(pTrackModel);
        snap.total = pModel->rowCount();
        const int current = pView->currentIndex().isValid() ? pView->currentIndex().row() : -1;
        const int top = std::max(0, pView->rowAt(0));
        const int first = current >= 0 && (current < top || current >= top + kRows) ? std::max(0, current - kRows / 2) : top;
        for (int r = first; r < std::min(snap.total, first + kRows); ++r) {
            QString title = c.title >= 0 ? pModel->index(r, c.title).data().toString().trimmed() : QString();
            if (title.isEmpty() && c.location >= 0) {
                title = tidyFileName(pModel->index(r, c.location).data().toString());
            }
            const QString artist = c.artist >= 0 ? pModel->index(r, c.artist).data().toString().trimmed() : QString();
            bool ok = false;
            const double bpm = c.bpm >= 0 ? pModel->index(r, c.bpm).data(Qt::EditRole).toDouble(&ok) : 0;
            const QString key = c.key >= 0 ? pModel->index(r, c.key).data().toString() : QString();
            snap.rows.append(QStringLiteral("%1").arg(r + 1, 3, 10, QLatin1Char('0')) + QLatin1Char('\t') +
                    (ok && bpm > 0 ? QString::number(bpm, 'f', 1) : QStringLiteral("--.-")) + QLatin1Char('\t') +
                    key + QLatin1Char('\t') + (artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title));
        }
        snap.current = current >= first ? current - first : -1;
        break;
    }
    }
    *pOut = snap;
    return true;
}

bool LibraryExplorer::goBack() {
    LibraryExplorer* pSelf = s_instance;
    if (!pSelf || !pSelf->m_pHost) {
        return false;
    }
    if (pSelf->m_backStack.isEmpty() && pSelf->m_page == Page::Source) {
        return false;
    }
    pSelf->back();
    return true;
}

void LibraryExplorer::openUsb() {
    LibraryExplorer* pSelf = s_instance;
    if (!pSelf || !pSelf->m_pHost) {
        return;
    }
    pSelf->showSource();
    pSelf->connectRekordbox(true, true);
}

bool LibraryExplorer::activateSelection() {
    LibraryExplorer* pSelf = s_instance;
    if (!pSelf || !pSelf->m_pHost || !pSelf->m_pHost->isVisible()) {
        return false;
    }
    if (pSelf->m_page == Page::Source) {
        for (QWidget* pCard : std::as_const(pSelf->m_cards)) {
            if (static_cast<SourceCard*>(pCard)->selected()) {
                static_cast<SourceCard*>(pCard)->click();
                return true;
            }
        }
        static_cast<SourceCard*>(pSelf->m_cards.value(0))->click();
        return true;
    }
    QListWidget* pList = pSelf->m_page == Page::Browse ? pSelf->m_pCategories.data()
            : pSelf->m_page == Page::List              ? pSelf->m_pList.data()
                                                       : nullptr;
    if (!pList || !pList->currentItem()) {
        return false;
    }
    emit pList->itemClicked(pList->currentItem());
    return true;
}

} // namespace mixxx::b2b
