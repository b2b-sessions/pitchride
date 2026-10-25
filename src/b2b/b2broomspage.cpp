#include "b2b/b2broomspage.h"

#include "b2b/b2bupdates.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextDocument>
#include <QtMath>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

#include "b2b/b2bmaze.h"
#include "b2b/b2bsite.h"
#include "b2b/b2btheme.h"
#include "moc_b2broomspage.cpp"

namespace mixxx::b2b {

namespace {

// The report's number: always 17. (The empty page has its own,
// progress report 1, in its words.)
constexpr int kReportNumber = 17;
// The mockups' measures (design/reference/scripts/rooms2.py).
constexpr int kMargin = 48;
constexpr int kLeftColumn = 110;
constexpr int kSideColumn = 320;
constexpr int kSentenceWidth = 600;
constexpr int kContentWidth = 1280 - 2 * kMargin; // the mockups' page

QString pageStyle() {
    using namespace theme;
    return QStringLiteral(
                   // The page's gradient is painted by the page (paintPage):
                   // everything on it is see-through.
                   "QWidget#Page, QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"
                   "QLabel { color: %1; background: transparent; %2 font-size: 15px; }"
                   // Micro labels: DM Mono capitals, tracked out.
                   "QLabel#Label { %3 font-size: 11px; letter-spacing: 1px; color: %4; }"
                   "QLabel#Friend { %3 font-size: 10px; letter-spacing: 1px; color: %4; }"
                   "QLabel#Stat { %3 font-size: 12px; letter-spacing: 0px; color: %1; }"
                   "QLabel#Report { %2 font-size: 34px; font-weight: 700; }"
                   "QLabel#Summary { color: %4; }"
                   "QLabel#Sentence { color: #DFE6FF; font-size: 15px; }"
                   "QLabel#Note { color: %1; }"
                   "QLabel#Empty { color: %1; font-size: 17px; }"
                   "QLabel#Muted { color: %4; }"
                   "QLabel#RoomName { %5 font-size: 28px; letter-spacing: -1px; }"
                   "QLabel#MyRoomName { %5 font-size: 22px; letter-spacing: -1px; }"
                   "QLabel#Code { %6 font-size: 20px; letter-spacing: 2px; color: %1; }"
                   "QLabel#Tag { %3 font-size: 10px; letter-spacing: 1px; color: %4; border: 1px solid %4;"
                   " padding: 1px 6px; }"
                   "QLineEdit#CodeField { background: transparent; color: %1; border: 1px solid rgba(247, 214, 220, 0.5);"
                   " border-radius: 0; padding: 7px 12px; %6 font-size: 15px; letter-spacing: 1px;"
                   " selection-background-color: %7; }"
                   "QLineEdit#CodeField:focus { border-color: %1; }"
                   // Each entry: the faint keyline above it.
                   "QFrame#Entry { border: 0; border-top: 1px solid rgba(232, 72, 60, 0.45); background: transparent; }"
                   "QFrame#Rule { border: 0; border-top: 1px solid rgba(232, 72, 60, 0.45); background: transparent; }"
                   "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
                   "QScrollBar::handle:vertical { background: rgba(247, 214, 220, 0.25); min-height: 30px; }"
                   "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }"
                   "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }")
                   .arg(QLatin1String(kType),
                           css(Face::Voice),
                           css(Face::Label),
                           QLatin1String(kTypeMuted),
                           css(Face::Display),
                           css(Face::Readout),
                           QLatin1String(kAction)) +
            buttons() + menu();
}

QLabel* label(const QString& text, const char* name, QWidget* pParent) {
    auto* pLabel = new QLabel(pParent);
    // Plain text unless a caller says otherwise: room titles and names come
    // from other people, and Qt would otherwise render "<h1>..." as HTML.
    pLabel->setTextFormat(Qt::PlainText);
    pLabel->setText(text);
    if (name) {
        pLabel->setObjectName(QString::fromLatin1(name));
    }
    pLabel->setWordWrap(true);
    return pLabel;
}

/// Typed words that may run on: the mockups' line height (CSS 1.55, which
/// is 135% of the face's own line height in Qt).
void setTyped(QLabel* pLabel, const QString& text) {
    pLabel->setTextFormat(Qt::RichText);
    pLabel->setText(text.isEmpty() ? QString()
                                   : QStringLiteral("<div style=\"line-height: 135%;\">%1</div>")
                                             .arg(text.toHtmlEscaped()));
}

QLabel* typed(const QString& text, const char* name, QWidget* pParent) {
    QLabel* pLabel = label(QString(), name, pParent);
    setTyped(pLabel, text);
    return pLabel;
}

/// A micro label: DM Mono capitals.
QLabel* micro(const QString& text, const char* name, QWidget* pParent) {
    QLabel* pLabel = label(text, name, pParent);
    pLabel->setWordWrap(false);
    theme::caps(pLabel);
    return pLabel;
}

QPushButton* button(const QString& text, const char* name, QWidget* pParent) {
    auto* pButton = new QPushButton(text, pParent);
    if (name) {
        pButton->setObjectName(QString::fromLatin1(name));
    }
    pButton->setCursor(Qt::PointingHandCursor);
    theme::caps(pButton);
    return pButton;
}

/// A drawn line icon (theme::icon), in the type colour.
QLabel* iconLabel(theme::Icon which, QWidget* pParent) {
    constexpr int kSize = 14;
    auto* pLabel = new QLabel(pParent);
    pLabel->setPixmap(theme::icon(which, kSize, QColor(theme::kType)));
    pLabel->setFixedSize(kSize, kSize);
    return pLabel;
}

/// The small daisy by a friend's room's name.
class Flower : public QWidget {
  public:
    explicit Flower(int size, QWidget* pParent)
            : QWidget(pParent) {
        setFixedSize(size, size);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        maze::paintFlower(p, QRectF(rect()).center(), width() / 2.0);
    }
};

/// A number as the typed notes say it (twelve and under in words).
QString inWords(int n) {
    static const char* const kWords[] = {QT_TR_NOOP("nobody"),
            QT_TR_NOOP("one"),
            QT_TR_NOOP("two"),
            QT_TR_NOOP("three"),
            QT_TR_NOOP("four"),
            QT_TR_NOOP("five"),
            QT_TR_NOOP("six"),
            QT_TR_NOOP("seven"),
            QT_TR_NOOP("eight"),
            QT_TR_NOOP("nine"),
            QT_TR_NOOP("ten"),
            QT_TR_NOOP("eleven"),
            QT_TR_NOOP("twelve")};
    return n >= 0 && n <= 12 ? QObject::tr(kWords[n]) : QString::number(n);
}

QString nameOf(const QJsonValue& person) {
    return person.toObject().value(QStringLiteral("name")).toString().toLower();
}

int djsOf(const QJsonObject& room) {
    const QJsonValue djs = room.value(QStringLiteral("djs"));
    return djs.isArray() ? static_cast<int>(djs.toArray().size()) : djs.toInt();
}

/// Who's on the decks, the way an entry says it ("mia and theo are
/// playing."). The site sends the host and how many are on the decks;
/// side_b (the friend) and now_playing when it knows them.
QString sentence(const QJsonObject& room) {
    const QString a = nameOf(room.value(QStringLiteral("host")));
    const QString b = nameOf(room.value(QStringLiteral("side_b")));
    const QString host = a.isEmpty() ? QObject::tr("someone") : a;
    QString out;
    if (!b.isEmpty()) {
        out = QObject::tr("%1 and %2 are playing.").arg(host, b);
    } else if (djsOf(room) >= 2) {
        out = QObject::tr("%1 and a friend are playing.").arg(host);
    } else {
        out = QObject::tr("%1 is playing. room for one more.").arg(host);
    }
    const QString now = room.value(QStringLiteral("now_playing")).toString().toLower();
    if (!now.isEmpty()) {
        out += QLatin1Char(' ') + QObject::tr("now playing: %1.").arg(now);
    }
    return out;
}

void clear(QLayout* pLayout) {
    while (QLayoutItem* pItem = pLayout->takeAt(0)) {
        if (QWidget* pWidget = pItem->widget()) {
            pWidget->deleteLater();
        } else if (QLayout* pChild = pItem->layout()) {
            clear(pChild);
        }
        delete pItem;
    }
}

} // namespace

RoomsPage::RoomsPage(Site* pSite, QWidget* pParent)
        : QWidget(pParent),
          m_pSite(pSite) {
    setObjectName(QStringLiteral("RoomsPage"));
    setStyleSheet(pageStyle());
    auto* pOuter = new QVBoxLayout(this);
    pOuter->setContentsMargins(0, 0, 0, 0);
    m_pStack = new QStackedWidget(this);
    pOuter->addWidget(m_pStack);

    // ---- Signed out: the way in (nothing else can be used until then).
    m_pSignedOut = new QWidget(m_pStack);
    m_pSignedOut->setObjectName(QStringLiteral("Page"));
    {
        auto* pCol = new QVBoxLayout(m_pSignedOut);
        pCol->addStretch(2);
        pCol->addWidget(new MazeLogo(180, m_pSignedOut, false), 0, Qt::AlignHCenter);
        pCol->addSpacing(26);
        auto* pTitle = label(tr("sign in to play"), "Note", m_pSignedOut);
        pTitle->setAlignment(Qt::AlignHCenter);
        pCol->addWidget(pTitle);
        m_pSignInNote = label(tr("pitchride is for playing back to back with your friends. sign in with your google "
                                 "account to start: it opens in your browser."),
                "Muted",
                m_pSignedOut);
        m_pSignInNote->setAlignment(Qt::AlignHCenter);
        m_pSignInNote->setFixedWidth(460);
        // (No alignment on the label itself: its height must follow its wrapping.)
        auto* pNoteRow = new QHBoxLayout();
        pNoteRow->addStretch(1);
        pNoteRow->addWidget(m_pSignInNote);
        pNoteRow->addStretch(1);
        pCol->addLayout(pNoteRow);
        pCol->addSpacing(20);
        m_pSignIn = button(tr("Sign in with Google"), "Primary", m_pSignedOut);
        // Again while waiting: the sign-in page opens again (same address).
        connect(m_pSignIn, &QPushButton::clicked, m_pSite, &Site::signIn);
        pCol->addWidget(m_pSignIn, 0, Qt::AlignHCenter);
        m_pCancelSignIn = button(tr("Cancel"), "Quiet", m_pSignedOut);
        connect(m_pCancelSignIn, &QPushButton::clicked, m_pSite, &Site::cancelSignIn);
        pCol->addSpacing(8);
        pCol->addWidget(m_pCancelSignIn, 0, Qt::AlignHCenter);
        pCol->addStretch(3);
    }
    m_pStack->addWidget(m_pSignedOut);

    // ---- Signed in: the report.
    auto* pScroll = new QScrollArea(m_pStack);
    pScroll->setWidgetResizable(true);
    pScroll->setFrameShape(QFrame::NoFrame);
    auto* pPage = new QWidget(pScroll);
    pPage->setObjectName(QStringLiteral("Page"));
    // The page as the mockups lay it out, centred in a wider window.
    auto* pCentre = new QHBoxLayout(pPage);
    pCentre->setContentsMargins(kMargin, 34, kMargin, 40);
    pCentre->setSpacing(0);
    auto* pContent = new QWidget(pPage);
    pContent->setObjectName(QStringLiteral("Page"));
    // As wide as there is, up to the mockups' 1280, whatever's on it: the
    // margins grow round it (resizeEvent).
    m_pCentre = pCentre;
    pCentre->addWidget(pContent);
    auto* pRow = new QHBoxLayout(pContent);
    pRow->setContentsMargins(0, 0, 0, 0);
    pRow->setSpacing(56);

    // The left: the report and the rooms.
    auto* pLeft = new QVBoxLayout();
    pLeft->setSpacing(0);
    m_pDate = micro(QString(), "Label", pPage);
    pLeft->addWidget(m_pDate);
    pLeft->addSpacing(8);
    m_pReport = label(tr("progress report %1").arg(kReportNumber), "Report", pPage);
    pLeft->addWidget(m_pReport);
    pLeft->addSpacing(8);
    m_pSummary = label(QString(), "Summary", pPage);
    pLeft->addWidget(m_pSummary);
    pLeft->addSpacing(18);
    m_pEntries = new QVBoxLayout();
    m_pEntries->setSpacing(0);
    pLeft->addLayout(m_pEntries);
    m_pWait = new MazeWait(pPage);
    m_pWait->setOpaque(false);
    m_pWait->setFixedHeight(300);
    m_pWait->hide();
    pLeft->addWidget(m_pWait);
    m_pNote = label(QString(), "Muted", pPage);
    m_pNote->setMaximumWidth(kSentenceWidth);
    m_pNote->setMinimumWidth(240);
    m_pNote->hide();
    pLeft->addSpacing(14);
    pLeft->addWidget(m_pNote);
    pLeft->addStretch(1);
    pRow->addLayout(pLeft, 1);

    // The right: the maze, your room, have a code.
    auto* pRightBox = new QWidget(pPage);
    pRightBox->setFixedWidth(kSideColumn);
    auto* pRight = new QVBoxLayout(pRightBox);
    pRight->setSpacing(30);
    pRight->setContentsMargins(0, 6, 0, 0);
    pRight->addWidget(new MazeLogo(180, pRightBox, false), 0, Qt::AlignLeft);
    m_pSide = new QVBoxLayout();
    m_pSide->setSpacing(30);
    pRight->addLayout(m_pSide);
    auto* pCodeBlock = new QVBoxLayout();
    pCodeBlock->setSpacing(10);
    pCodeBlock->addWidget(micro(tr("have a code"), "Label", pRightBox));
    auto* pCodeRow = new QHBoxLayout();
    pCodeRow->setSpacing(8);
    m_pCode = new QLineEdit(pRightBox);
    m_pCode->setObjectName(QStringLiteral("CodeField"));
    m_pCode->setPlaceholderText(QStringLiteral("K7P-2QX"));
    m_pCode->setMaxLength(12);
    connect(m_pCode, &QLineEdit::returnPressed, this, &RoomsPage::joinTyped);
    pCodeRow->addWidget(m_pCode, 1);
    m_pJoin = button(tr("Join"), nullptr, pRightBox);
    connect(m_pJoin, &QPushButton::clicked, this, &RoomsPage::joinTyped);
    pCodeRow->addWidget(m_pJoin);
    pCodeBlock->addLayout(pCodeRow);
    m_pCodeNote = label(QString(), "Muted", pRightBox);
    m_pCodeNote->hide();
    pCodeBlock->addWidget(m_pCodeNote);
    pRight->addLayout(pCodeBlock);
    pRight->addStretch(1);
    pRow->addWidget(pRightBox, 0, Qt::AlignTop);

    pScroll->setWidget(pPage);
    m_pStack->addWidget(pScroll);

    // ---- Signed in, nobody on the decks and no room of yours: the maze
    // and a note, in the middle (rooms-empty.png).
    m_pEmpty = new QWidget(m_pStack);
    m_pEmpty->setObjectName(QStringLiteral("Page"));
    {
        auto* pCol = new QVBoxLayout(m_pEmpty);
        pCol->setSpacing(0);
        pCol->addStretch(1);
        pCol->addWidget(new MazeLogo(220, m_pEmpty, false), 0, Qt::AlignHCenter);
        pCol->addSpacing(22);
        auto* pNote = label(QString(), "Empty", m_pEmpty);
        pNote->setTextFormat(Qt::RichText);
        pNote->setText(QStringLiteral("<div align=\"center\" style=\"line-height: 135%;\">%1<br>%2</div>")
                        .arg(tr("progress report 1").toHtmlEscaped(),
                                tr("nobody is on the decks yet. i think someone will come.").toHtmlEscaped()));
        pNote->setFixedWidth(420);
        // A rich text label's height doesn't follow its line height: lay
        // the note out as the label will, and take its height.
        pNote->ensurePolished();
        QTextDocument layout;
        layout.setDefaultFont(pNote->font());
        layout.setDocumentMargin(0);
        layout.setHtml(pNote->text());
        layout.setTextWidth(420);
        pNote->setFixedHeight(qCeil(layout.size().height()) + 2);
        pCol->addWidget(pNote, 0, Qt::AlignHCenter);
        pCol->addSpacing(22);
        auto* pButtons = new QHBoxLayout();
        pButtons->setSpacing(10);
        pButtons->addStretch(1);
        QPushButton* pHost = button(tr("Host a room"), "Primary", m_pEmpty);
        connect(pHost, &QPushButton::clicked, this, &RoomsPage::hostRoom);
        pButtons->addWidget(pHost);
        QPushButton* pCode = button(tr("Join with code"), nullptr, m_pEmpty);
        connect(pCode, &QPushButton::clicked, this, &RoomsPage::joinWithCode);
        pButtons->addWidget(pCode);
        pButtons->addStretch(1);
        pCol->addLayout(pButtons);
        pCol->addStretch(1);
    }
    m_pStack->addWidget(m_pEmpty);

    // ---- Signed in, not invited yet: the code from the invite email.
    m_pInvite = new QWidget(m_pStack);
    m_pInvite->setObjectName(QStringLiteral("Page"));
    {
        auto* pCol = new QVBoxLayout(m_pInvite);
        pCol->addStretch(2);
        pCol->addWidget(new MazeLogo(180, m_pInvite, false), 0, Qt::AlignHCenter);
        pCol->addSpacing(26);
        auto* pTitle = label(tr("you're early"), "Note", m_pInvite);
        pTitle->setAlignment(Qt::AlignHCenter);
        pCol->addWidget(pTitle);
        auto* pNote = label(tr("pitchride is invite-only for now. enter the code from your invite email, or the one a "
                               "friend gave you."),
                "Muted",
                m_pInvite);
        pNote->setAlignment(Qt::AlignHCenter);
        pNote->setFixedWidth(460);
        auto* pNoteRow = new QHBoxLayout();
        pNoteRow->addStretch(1);
        pNoteRow->addWidget(pNote);
        pNoteRow->addStretch(1);
        pCol->addLayout(pNoteRow);
        pCol->addSpacing(20);
        auto* pRow = new QHBoxLayout();
        pRow->setSpacing(8);
        pRow->addStretch(1);
        m_pInviteCode = new QLineEdit(m_pInvite);
        m_pInviteCode->setObjectName(QStringLiteral("CodeField"));
        m_pInviteCode->setPlaceholderText(QStringLiteral("K7PM-2QXR"));
        m_pInviteCode->setMaxLength(14);
        m_pInviteCode->setFixedWidth(220);
        connect(m_pInviteCode, &QLineEdit::returnPressed, this, &RoomsPage::redeem);
        pRow->addWidget(m_pInviteCode);
        m_pRedeem = button(tr("Use code"), "Primary", m_pInvite);
        connect(m_pRedeem, &QPushButton::clicked, this, &RoomsPage::redeem);
        pRow->addWidget(m_pRedeem);
        pRow->addStretch(1);
        pCol->addLayout(pRow);
        pCol->addSpacing(10);
        m_pInviteNote = label(QString(), "Muted", m_pInvite);
        m_pInviteNote->setAlignment(Qt::AlignHCenter);
        m_pInviteNote->hide();
        pCol->addWidget(m_pInviteNote);
        pCol->addSpacing(14);
        auto* pWaitlist = button(tr("No code? Join the waitlist"), "Quiet", m_pInvite);
        connect(pWaitlist, &QPushButton::clicked, this, [this] {
            QDesktopServices::openUrl(m_pSite->url());
        });
        pCol->addWidget(pWaitlist, 0, Qt::AlignHCenter);
        pCol->addStretch(3);
    }
    m_pStack->addWidget(m_pInvite);

    // ---- Too old to play: the new one to download.
    m_pUpdate = new QWidget(m_pStack);
    m_pUpdate->setObjectName(QStringLiteral("Page"));
    {
        auto* pCol = new QVBoxLayout(m_pUpdate);
        pCol->addStretch(2);
        pCol->addWidget(new MazeLogo(180, m_pUpdate, false), 0, Qt::AlignHCenter);
        pCol->addSpacing(26);
        auto* pTitle = label(tr("time for the new pitchride"), "Note", m_pUpdate);
        pTitle->setAlignment(Qt::AlignHCenter);
        pCol->addWidget(pTitle);
        m_pUpdateNote = label(QString(), "Muted", m_pUpdate);
        m_pUpdateNote->setAlignment(Qt::AlignHCenter);
        m_pUpdateNote->setFixedWidth(460);
        auto* pNoteRow = new QHBoxLayout();
        pNoteRow->addStretch(1);
        pNoteRow->addWidget(m_pUpdateNote);
        pNoteRow->addStretch(1);
        pCol->addLayout(pNoteRow);
        pCol->addSpacing(20);
        m_pUpdateButton = button(tr("Download the new version"), "Primary", m_pUpdate);
        connect(m_pUpdateButton, &QPushButton::clicked, this, [this] {
            if (m_selfUpdate && m_updateState == Updater::State::Ready) {
                emit updateNowClicked();
            } else {
                QDesktopServices::openUrl(m_updatePage);
            }
        });
        pCol->addWidget(m_pUpdateButton, 0, Qt::AlignHCenter);
        pCol->addStretch(3);
    }
    m_pStack->addWidget(m_pUpdate);

    connect(m_pSite, &Site::signedInChanged, this, &RoomsPage::rebuild);
    connect(m_pSite, &Site::meChanged, this, &RoomsPage::rebuild);
    connect(m_pSite, &Site::roomsChanged, this, &RoomsPage::rebuild);
    connect(m_pSite, &Site::signInProblem, this, [this](const QString& message) {
        m_pSignInNote->setText(message);
    });
    // The time at the top, to the minute.
    m_clock.setInterval(15000);
    connect(&m_clock, &QTimer::timeout, this, &RoomsPage::updateClock);
    updateClock();
    rebuild();
}

void RoomsPage::setSessionMenu(QMenu* pMenu) {
    m_pSessionMenu = pMenu;
}

void RoomsPage::setHostRoom(const QJsonObject& room) {
    if (room != m_myRoom) {
        m_myRoom = room;
        rebuild();
    }
}

void RoomsPage::setGuestRoom(const QJsonObject& room, const QString& status) {
    if (room != m_guestRoom || status != m_guestStatus) {
        m_guestRoom = room;
        m_guestStatus = status;
        rebuild();
    }
}

void RoomsPage::paintEvent(QPaintEvent* pEvent) {
    QPainter p(this);
    theme::paintPage(p, this, pEvent->rect());
}

void RoomsPage::resizeEvent(QResizeEvent* pEvent) {
    QWidget::resizeEvent(pEvent);
    const int side = std::max(kMargin, (width() - kContentWidth) / 2);
    m_pCentre->setContentsMargins(side, 34, side, 40);
}

void RoomsPage::showEvent(QShowEvent* pEvent) {
    QWidget::showEvent(pEvent);
    m_pSite->watchRooms(true);
    updateClock();
    m_clock.start();
}

void RoomsPage::hideEvent(QHideEvent* pEvent) {
    QWidget::hideEvent(pEvent);
    m_pSite->watchRooms(false);
    m_clock.stop();
}

void RoomsPage::updateClock() {
    // "tuesday 20 october · 00:10" (shown in capitals, as a label).
    const QDateTime now = QDateTime::currentDateTime();
    const QLocale english(QLocale::English);
    m_pDate->setText(english.toString(now, QStringLiteral("dddd d MMMM")).toLower() + QStringLiteral(" · ") +
            now.toString(QStringLiteral("HH:mm")));
}

void RoomsPage::rebuild() {
    const bool signedIn = m_pSite->signedIn();
    m_pSignIn->setText(m_pSite->signingIn() ? tr("Open the sign-in page again") : tr("Sign in with Google"));
    m_pCancelSignIn->setVisible(m_pSite->signingIn());
    if (m_pSite->signingIn()) {
        m_pSignInNote->setText(tr("finish signing in in your browser, then come back here."));
    } else if (m_pSignInNote->text() == tr("finish signing in in your browser, then come back here.")) {
        m_pSignInNote->setText(tr("pitchride is for playing back to back with your friends. sign in with "
                                  "your google account to start: it opens in your browser."));
    }
    // Too old to play: the way to update. Signed out on a computer that
    // updates itself, sign in first (then the app gets the new version
    // itself, rather than sending you to the website).
    if (m_updateRequired && (signedIn || !Updater::supported())) {
        m_pStack->setCurrentWidget(m_pUpdate);
        return;
    }
    m_pStack->setCurrentIndex(signedIn ? 1 : 0);
    if (!signedIn) {
        return;
    }
    if (!m_pSite->canPlay()) {
        m_pStack->setCurrentWidget(m_pInvite);
        return;
    }

    // The right column: your own open room (as its host sees it), or the
    // room you're playing in, or a way to open one.
    clear(m_pSide);
    if (!m_myRoom.isEmpty()) {
        m_pSide->addWidget(myRoomBlock(m_myRoom));
    }
    if (!m_guestRoom.isEmpty()) {
        m_pSide->addWidget(guestBlock());
    }
    if (m_myRoom.isEmpty() && m_guestRoom.isEmpty()) {
        m_pSide->addWidget(noRoomBlock());
    }

    // Everyone's open public rooms (the site's order: newest first).
    clear(m_pEntries);
    const QString myId = m_myRoom.value(QStringLiteral("id")).toString();
    const QString guestId = m_guestRoom.value(QStringLiteral("id")).toString();
    int count = 0;
    int friends = 0;
    for (const QJsonValue& value : m_pSite->rooms()) {
        const QJsonObject room = value.toObject();
        const QString id = room.value(QStringLiteral("id")).toString();
        if (id == myId || id == guestId) {
            continue;
        }
        m_pEntries->addWidget(entry(room, count == 0));
        ++count;
        if (room.value(QStringLiteral("friend")).toBool()) {
            ++friends;
        }
    }
    if (count > 0) {
        auto* pRule = new QFrame(this);
        pRule->setObjectName(QStringLiteral("Rule"));
        pRule->setFixedHeight(1);
        m_pEntries->addWidget(pRule);
    }

    // One line on what's open ("five rooms are open tonight. two of them
    // are friends.").
    const int hour = QTime::currentTime().hour();
    const QString when = hour >= 18 || hour < 5 ? tr("tonight") : tr("right now");
    QString summary;
    if (count == 1) {
        summary = tr("one room is open %1.").arg(when);
    } else if (count > 1) {
        summary = tr("%1 rooms are open %2.").arg(inWords(count), when);
    }
    if (friends == 1) {
        summary += QLatin1Char(' ') + tr("one of them is a friend.");
    } else if (friends > 1) {
        summary += QLatin1Char(' ') + tr("%1 of them are friends.").arg(inWords(friends));
    }
    m_pSummary->setText(summary);
    m_pSummary->setVisible(!summary.isEmpty());

    const QString problem = m_pSite->roomsProblem();
    const bool looking = count == 0 && problem.isEmpty() && !m_pSite->roomsLoaded();
    m_pWait->setVisible(looking);
    if (looking) {
        m_pWait->setText(tr("looking for rooms."));
    }
    QString note;
    if (count == 0 && !problem.isEmpty()) {
        note = problem;
    } else if (count == 0 && m_pSite->roomsLoaded()) {
        note = m_myRoom.isEmpty() ? tr("no rooms are open yet.") : tr("no other rooms are open right now.");
    }
    setTyped(m_pNote, note);
    m_pNote->setVisible(!note.isEmpty());
    // Nobody at all, and no room of yours: the empty page.
    const bool empty = count == 0 && problem.isEmpty() && m_pSite->roomsLoaded() && m_myRoom.isEmpty() &&
            m_guestRoom.isEmpty();
    m_pStack->setCurrentIndex(empty ? 2 : 1);
}

QWidget* RoomsPage::entry(const QJsonObject& room, bool first) {
    auto* pEntry = new QFrame(this);
    pEntry->setObjectName(QStringLiteral("Entry"));
    auto* pRow = new QHBoxLayout(pEntry);
    pRow->setContentsMargins(0, 16, 0, 16);
    pRow->setSpacing(22);
    const QString id = room.value(QStringLiteral("id")).toString();
    const bool isFriend = room.value(QStringLiteral("friend")).toBool();

    // Left: friend, and who's on the decks.
    auto* pLeftBox = new QWidget(pEntry);
    pLeftBox->setFixedWidth(kLeftColumn);
    auto* pLeft = new QVBoxLayout(pLeftBox);
    pLeft->setSpacing(6);
    pLeft->setContentsMargins(0, 2, 0, 0);
    if (isFriend) {
        pLeft->addWidget(micro(tr("friend"), "Friend", pLeftBox));
    }
    auto stat = [&](QLabel* pIcon, const QString& value, const QString& tip) {
        auto* pStat = new QHBoxLayout();
        pStat->setSpacing(7);
        pStat->addWidget(pIcon);
        QLabel* pValue = label(value, "Stat", pLeftBox);
        pValue->setToolTip(tip);
        pIcon->setToolTip(tip);
        pStat->addWidget(pValue);
        pStat->addStretch(1);
        pLeft->addLayout(pStat);
    };
    // On the decks, of how many the room holds (two: side a and side b).
    const int capacity = std::max(1, room.value(QStringLiteral("capacity")).toInt(2));
    const int djs = std::clamp(djsOf(room), 0, capacity);
    const bool full = djs >= capacity;
    stat(iconLabel(theme::Icon::Record, pLeftBox),
            QStringLiteral("%1/%2").arg(djs).arg(capacity),
            full ? tr("on the decks: the room is full") : tr("on the decks"));
    pLeft->addStretch(1);
    pRow->addWidget(pLeftBox);

    // Middle: the room's name (a flower if it's a friend's) and who's on.
    auto* pMiddle = new QVBoxLayout();
    pMiddle->setSpacing(8);
    auto* pName = new QHBoxLayout();
    pName->setSpacing(10);
    QLabel* pTitle = label(room.value(QStringLiteral("title")).toString().toLower(), "RoomName", pEntry);
    pTitle->setWordWrap(false);
    pName->addWidget(pTitle);
    if (isFriend) {
        pName->addWidget(new Flower(18, pEntry), 0, Qt::AlignVCenter);
    }
    pName->addStretch(1);
    pMiddle->addLayout(pName);
    auto* pWho = typed(sentence(room), "Sentence", pEntry);
    pWho->setMaximumWidth(kSentenceWidth);
    pWho->setMinimumWidth(240); // it wraps: never wider than the room there is
    pMiddle->addWidget(pWho);
    pMiddle->addStretch(1);
    pRow->addLayout(pMiddle, 1);

    // Right: Ask to join (the host lets you in or not); full: no asking.
    if (full) {
        QLabel* pFull = micro(tr("full"), "Tag", pEntry);
        pFull->setToolTip(tr("the room is full: two are playing"));
        pRow->addWidget(pFull, 0, Qt::AlignTop);
    } else {
        auto* pGo = button(tr("Ask to join"), first ? "Primary" : nullptr, pEntry);
        pGo->setToolTip(tr("ask the host to let you in"));
        connect(pGo, &QPushButton::clicked, this, [this, id] {
            askToJoin(id);
        });
        pRow->addWidget(pGo, 0, Qt::AlignTop);
    }
    return pEntry;
}

QWidget* RoomsPage::noRoomBlock() {
    auto* pBlock = new QWidget(this);
    auto* pCol = new QVBoxLayout(pBlock);
    pCol->setContentsMargins(0, 0, 0, 0);
    pCol->setSpacing(10);
    pCol->addWidget(micro(tr("your room"), "Label", pBlock));
    pCol->addWidget(typed(tr("you have no room open. open one and send a friend the code."), "Note", pBlock));
    pCol->addSpacing(4);
    auto* pHost = button(tr("Host a room"), "Primary", pBlock);
    connect(pHost, &QPushButton::clicked, this, &RoomsPage::hostRoom);
    pCol->addWidget(pHost, 0, Qt::AlignLeft);
    return pBlock;
}

QWidget* RoomsPage::guestBlock() {
    const QJsonObject& room = m_guestRoom;
    auto* pBlock = new QWidget(this);
    auto* pCol = new QVBoxLayout(pBlock);
    pCol->setContentsMargins(0, 0, 0, 0);
    pCol->setSpacing(8);
    pCol->addWidget(micro(m_guestStatus == QLatin1String("playing") ? tr("you're playing in") : tr("you asked to join"),
            "Label",
            pBlock));
    pCol->addWidget(label(room.value(QStringLiteral("title")).toString().toLower(), "MyRoomName", pBlock));
    const QString host = room.value(QStringLiteral("host")).toObject().value(QStringLiteral("name")).toString();
    QString status;
    if (m_guestStatus == QLatin1String("waiting")) {
        status = tr("waiting for %1 to let you in.").arg(host);
    } else if (m_guestStatus == QLatin1String("connecting")) {
        status = tr("%1 said yes. linking your decks.").arg(host);
    } else if (m_guestStatus == QLatin1String("playing")) {
        status = tr("your decks are linked with %1's. hand over and take control on the booth tab.").arg(host);
    } else {
        status = m_guestStatus;
    }
    pCol->addWidget(typed(status, "Note", pBlock));
    pCol->addSpacing(4);
    auto* pActions = new QHBoxLayout();
    pActions->setSpacing(8);
    if (m_pSessionMenu && m_guestStatus == QLatin1String("playing")) {
        auto* pOptions = button(tr("Options"), nullptr, pBlock);
        pOptions->setMenu(m_pSessionMenu);
        pActions->addWidget(pOptions);
    }
    auto* pLeave = button(m_guestStatus == QLatin1String("waiting") ? tr("Cancel") : tr("Leave the room"),
            "Danger",
            pBlock);
    connect(pLeave, &QPushButton::clicked, this, &RoomsPage::leavePlayingRequested);
    pActions->addWidget(pLeave);
    pActions->addStretch(1);
    pCol->addLayout(pActions);
    return pBlock;
}

QWidget* RoomsPage::myRoomBlock(const QJsonObject& room) {
    auto* pBlock = new QWidget(this);
    auto* pCol = new QVBoxLayout(pBlock);
    pCol->setContentsMargins(0, 0, 0, 0);
    pCol->setSpacing(8);
    const QString id = room.value(QStringLiteral("id")).toString();

    pCol->addWidget(micro(tr("your room"), "Label", pBlock));
    auto* pTitleRow = new QHBoxLayout();
    pTitleRow->setSpacing(10);
    QLabel* pTitle = label(room.value(QStringLiteral("title")).toString().toLower(), "MyRoomName", pBlock);
    pTitle->setWordWrap(false);
    pTitleRow->addWidget(pTitle);
    const QString visibility = room.value(QStringLiteral("visibility")).toString();
    pTitleRow->addWidget(micro(visibility == QLatin1String("private") ? tr("private") : tr("public"), "Tag", pBlock),
            0,
            Qt::AlignVCenter);
    pTitleRow->addStretch(1);
    pCol->addLayout(pTitleRow);

    // The code to send people.
    const QString code = room.value(QStringLiteral("code")).toString();
    if (!code.isEmpty()) {
        auto* pCodeRow = new QHBoxLayout();
        pCodeRow->setSpacing(12);
        pCodeRow->addWidget(label(code, "Code", pBlock));
        auto* pCopy = button(tr("Copy"), nullptr, pBlock);
        connect(pCopy, &QPushButton::clicked, this, [code, pCopy] {
            QApplication::clipboard()->setText(code);
            pCopy->setText(tr("Copied"));
            toast(pCopy, tr("code %1 copied. send it to the friend you want to play with.").arg(code));
        });
        pCodeRow->addWidget(pCopy);
        pCodeRow->addStretch(1);
        pCol->addLayout(pCodeRow);
    }

    // Who's asking to join, and who's on the decks with you.
    for (const QJsonValue& value : room.value(QStringLiteral("requests")).toArray()) {
        const QJsonObject r = value.toObject();
        const QString name = r.value(QStringLiteral("user")).toObject().value(QStringLiteral("name")).toString();
        const int rid = r.value(QStringLiteral("id")).toInt();
        pCol->addSpacing(4);
        pCol->addWidget(typed(tr("%1 wants to join.").arg(name.toLower()), "Note", pBlock));
        auto* pAsk = new QHBoxLayout();
        pAsk->setSpacing(8);
        auto* pYes = button(tr("Let them in"), "Primary", pBlock);
        auto* pNo = button(tr("Decline"), nullptr, pBlock);
        connect(pYes, &QPushButton::clicked, this, [this, id, rid] {
            emit decideRequested(id, rid, QStringLiteral("approve"));
        });
        connect(pNo, &QPushButton::clicked, this, [this, id, rid] {
            emit decideRequested(id, rid, QStringLiteral("decline"));
        });
        pAsk->addWidget(pYes);
        pAsk->addWidget(pNo);
        pAsk->addStretch(1);
        pCol->addLayout(pAsk);
    }
    for (const QJsonValue& value : room.value(QStringLiteral("on_decks")).toArray()) {
        const QJsonObject r = value.toObject();
        const QString name = r.value(QStringLiteral("user")).toObject().value(QStringLiteral("name")).toString();
        const int rid = r.value(QStringLiteral("id")).toInt();
        pCol->addSpacing(4);
        pCol->addWidget(typed(tr("%1 is on the decks with you.").arg(name.toLower()), "Note", pBlock));
        auto* pRemove = button(tr("Take them off"), nullptr, pBlock);
        connect(pRemove, &QPushButton::clicked, this, [this, id, rid] {
            emit decideRequested(id, rid, QStringLiteral("remove"));
        });
        pCol->addWidget(pRemove, 0, Qt::AlignLeft);
    }

    pCol->addSpacing(6);
    auto* pActions = new QHBoxLayout();
    pActions->setSpacing(8);
    if (m_pSessionMenu && !room.value(QStringLiteral("on_decks")).toArray().isEmpty()) {
        auto* pOptions = button(tr("Options"), nullptr, pBlock);
        pOptions->setToolTip(tr("how you play together, and handing over"));
        pOptions->setMenu(m_pSessionMenu);
        pActions->addWidget(pOptions);
    }
    auto* pClose = button(tr("Close room"), "Danger", pBlock);
    connect(pClose, &QPushButton::clicked, this, [this, id] {
        QMessageBox ask(QMessageBox::NoIcon,
                tr("Close room"),
                tr("close your room? your friend is let go too."),
                QMessageBox::NoButton,
                this);
        QPushButton* pYes = ask.addButton(tr("Close room"), QMessageBox::AcceptRole);
        ask.addButton(tr("Keep it open"), QMessageBox::RejectRole);
        ask.setDefaultButton(pYes);
        ask.exec();
        if (ask.clickedButton() != pYes) {
            return;
        }
        m_pSite->closeRoom(id, [this](const QJsonObject&, const QString& error) {
            if (!error.isEmpty()) {
                QMessageBox::warning(this, tr("Close room"), error);
            }
            m_myRoom = QJsonObject();
        });
    });
    pActions->addWidget(pClose);
    pActions->addStretch(1);
    pCol->addLayout(pActions);
    return pBlock;
}

void RoomsPage::askToJoin(const QString& roomId, const QString& invite) {
    emit joinRequested(roomId, invite);
}

void RoomsPage::hostRoom() {
    if (!m_myRoom.isEmpty()) {
        QMessageBox::information(this, tr("Host a room"),
                tr("you have a room open already. close it first to open another."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Host a room"));
    dialog.setStyleSheet(theme::dialog());
    dialog.setMinimumWidth(460);
    auto* pCol = new QVBoxLayout(&dialog);
    pCol->setContentsMargins(26, 24, 26, 22);
    pCol->setSpacing(10);
    pCol->addWidget(label(tr("host a room"), "Heading", &dialog));
    pCol->addWidget(micro(tr("Room name"), "Section", &dialog));
    auto* pName = new QLineEdit(&dialog);
    const QString me = m_pSite->me().value(QStringLiteral("name")).toString();
    pName->setText(me.isEmpty() ? tr("my room") : tr("%1's room").arg(me));
    pName->selectAll();
    pName->setMaxLength(80);
    pCol->addWidget(pName);
    pCol->addSpacing(8);
    pCol->addWidget(micro(tr("Who can come in"), "Section", &dialog));
    auto* pGroup = new QButtonGroup(&dialog);
    auto* pPublic = new QRadioButton(tr("public: listed on rooms for everyone"), &dialog);
    auto* pPrivate = new QRadioButton(tr("private: not listed. send your friend its code"), &dialog);
    pPublic->setChecked(true);
    for (auto* pRadio : {pPublic, pPrivate}) {
        pGroup->addButton(pRadio);
        pRadio->setCursor(Qt::PointingHandCursor);
        pCol->addWidget(pRadio);
    }
    // How you play: two decks each unless you choose otherwise (most people
    // bring a 2-channel controller; each plays their own two decks).
    pCol->addSpacing(8);
    pCol->addWidget(micro(tr("How you play"), "Section", &dialog));
    auto* pStyles = new QButtonGroup(&dialog);
    for (PlayStyle style : {PlayStyle::Split, PlayStyle::AllMine, PlayStyle::Turns}) {
        auto* pRadio = new QRadioButton(playStyleText(style), &dialog);
        pRadio->setCursor(Qt::PointingHandCursor);
        pRadio->setChecked(style == m_playStyle);
        pStyles->addButton(pRadio, static_cast<int>(style));
        pCol->addWidget(pRadio);
    }
    pCol->addSpacing(12);
    auto* pButtons = new QDialogButtonBox(&dialog);
    auto* pOpen = pButtons->addButton(tr("Open room"), QDialogButtonBox::AcceptRole);
    pOpen->setDefault(true);
    QPushButton* pCancel = pButtons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    for (QPushButton* pButton : {pOpen, pCancel}) {
        pButton->setCursor(Qt::PointingHandCursor);
        theme::caps(pButton);
    }
    connect(pButtons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(pButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    pCol->addWidget(pButtons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_playStyle = static_cast<PlayStyle>(std::max(0, pStyles->checkedId()));
    emit playStyleChosen(m_playStyle);
    const QString visibility = pPrivate->isChecked() ? QStringLiteral("private") : QStringLiteral("public");
    m_pSite->createRoom(pName->text().trimmed(), visibility, [this](const QJsonObject& room, const QString& error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(this, tr("Host a room"), error);
            return;
        }
        m_myRoom = room;
        rebuild();
        emit roomOpened();
    });
}

void RoomsPage::setUpdateRequired(bool required, const QString& newest, const QUrl& page) {
    m_updateRequired = required;
    m_updatePage = page;
    m_updateNewest = newest;
    showUpdate();
    rebuild();
}

void RoomsPage::setUpdateProgress(bool selfUpdate, Updater::State state, int percent, const QString& problem) {
    m_selfUpdate = selfUpdate;
    m_updateState = state;
    m_updatePercent = percent;
    m_updateProblem = problem;
    showUpdate();
}

void RoomsPage::showUpdate() {
    const QString current = UpdateCheck::currentVersion();
    // Getting it ourselves: the button says how it's going, then installs.
    if (m_selfUpdate && m_updateState != Updater::State::Failed) {
        const bool ready = m_updateState == Updater::State::Ready;
        m_pUpdateNote->setText(ready
                        ? tr("this version (%1) is too old to play with others. %2 is downloaded. update, "
                             "and pitchride closes, installs it and opens again.")
                                  .arg(current, m_updateNewest)
                        : tr("this version (%1) is too old to play with others. getting %2 for you now.")
                                  .arg(current, m_updateNewest));
        m_pUpdateButton->setText(ready ? tr("Update")
                                       : tr("Getting it \xC2\xB7 %1%").arg(m_updatePercent));
        m_pUpdateButton->setEnabled(ready);
        return;
    }
    // Otherwise (another computer, signed out, or it didn't work): the website.
    QString note = tr("this version (%1) is too old to play with others. download %2, open the installer, "
                      "and you're back in.")
                           .arg(current, m_updateNewest);
    if (m_selfUpdate && !m_updateProblem.isEmpty()) {
        note = m_updateProblem + QStringLiteral("\n") + note;
    }
    m_pUpdateNote->setText(note);
    m_pUpdateButton->setText(tr("Download the new version"));
    m_pUpdateButton->setEnabled(true);
}

void RoomsPage::redeem() {
    const QString code = m_pInviteCode->text().trimmed();
    if (code.isEmpty()) {
        m_pInviteCode->setFocus();
        return;
    }
    m_pRedeem->setEnabled(false);
    m_pInviteNote->hide();
    QPointer<RoomsPage> pSafe(this);
    m_pSite->redeemInvite(code, [pSafe](const QString& error) {
        if (!pSafe) {
            return;
        }
        pSafe->m_pRedeem->setEnabled(true);
        if (!error.isEmpty()) {
            setTyped(pSafe->m_pInviteNote, error.toLower());
            pSafe->m_pInviteNote->show();
            return;
        }
        pSafe->m_pInviteCode->clear();
        toast(pSafe->window(), tr("you're in. welcome to pitchride."));
    });
}

void RoomsPage::joinTyped() {
    const QString code = m_pCode->text().trimmed();
    if (code.isEmpty()) {
        m_pCode->setFocus();
        return;
    }
    m_pJoin->setEnabled(false);
    m_pCodeNote->hide();
    QPointer<RoomsPage> pSafe(this);
    m_pSite->findRoomByCode(code, [pSafe](const QJsonObject& room, const QString& error) {
        if (!pSafe) {
            return;
        }
        pSafe->m_pJoin->setEnabled(true);
        if (!error.isEmpty() || room.isEmpty()) {
            setTyped(pSafe->m_pCodeNote, error.isEmpty() ? tr("no open room has that code.") : error);
            pSafe->m_pCodeNote->show();
            return;
        }
        pSafe->m_pCode->clear();
        pSafe->askToJoin(room.value(QStringLiteral("id")).toString(), room.value(QStringLiteral("invite_token")).toString());
    });
}

void RoomsPage::joinWithCode() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Join with code"));
    dialog.setStyleSheet(theme::dialog());
    dialog.setMinimumWidth(420);
    auto* pCol = new QVBoxLayout(&dialog);
    pCol->setContentsMargins(26, 24, 26, 22);
    pCol->setSpacing(10);
    pCol->addWidget(label(tr("join with code"), "Heading", &dialog));
    pCol->addWidget(label(tr("the code the room's host sent you, like K7P-2QX."), "Muted", &dialog));
    auto* pCode = new QLineEdit(&dialog);
    pCode->setObjectName(QStringLiteral("CodeEdit"));
    pCode->setPlaceholderText(QStringLiteral("K7P-2QX"));
    pCode->setMaxLength(12);
    pCol->addWidget(pCode);
    auto* pProblem = label(QString(), "Muted", &dialog);
    pProblem->hide();
    pCol->addWidget(pProblem);
    auto* pButtons = new QDialogButtonBox(&dialog);
    auto* pFind = pButtons->addButton(tr("Join"), QDialogButtonBox::AcceptRole);
    pFind->setDefault(true);
    QPushButton* pCancel = pButtons->addButton(tr("Cancel"), QDialogButtonBox::RejectRole);
    for (QPushButton* pButton : {pFind, pCancel}) {
        pButton->setCursor(Qt::PointingHandCursor);
        theme::caps(pButton);
    }
    connect(pButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QPointer<QDialog> pDialog(&dialog);
    connect(pButtons, &QDialogButtonBox::accepted, &dialog, [this, pDialog, pCode, pProblem, pFind] {
        pFind->setEnabled(false);
        m_pSite->findRoomByCode(pCode->text(),
                [this, pDialog, pProblem, pFind](const QJsonObject& room, const QString& error) {
                    if (!pDialog) {
                        return; // closed meanwhile
                    }
                    pFind->setEnabled(true);
                    if (!error.isEmpty() || room.isEmpty()) {
                        pProblem->setText(error.isEmpty() ? tr("no open room has that code.") : error);
                        pProblem->show();
                        return;
                    }
                    askToJoin(room.value(QStringLiteral("id")).toString(),
                            room.value(QStringLiteral("invite_token")).toString());
                    pDialog->accept();
                });
    });
    pCol->addWidget(pButtons);
    dialog.exec();
}

} // namespace mixxx::b2b
