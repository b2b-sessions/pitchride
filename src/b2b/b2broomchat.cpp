#include "b2b/b2broomchat.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QScrollBar>
#include <QTextBrowser>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <algorithm>

#include "b2b/b2bsite.h"
#include "b2b/b2btheme.h"
#include "b2b/b2bwebsocket.h"
#include "moc_b2broomchat.cpp"

namespace mixxx::b2b {

namespace {
constexpr int kMaxChat = 200;

QString chatStyle() {
    using namespace theme;
    return QStringLiteral(
                   "QLabel#Title { color: %1; %2 font-size: 11px; letter-spacing: 1px; background: transparent; }"
                   "QTextBrowser { background: transparent; color: %3; border: none; %4 font-size: 14px; }"
                   "QLineEdit { background: transparent; color: %3; border: 1px solid rgba(247, 214, 220, 0.6);"
                   " border-radius: 0; padding: 8px 12px; %2 font-size: 13px; selection-background-color: %5; }"
                   "QLineEdit:focus { border-color: %3; }"
                   "QLineEdit:disabled { color: %6; border-color: rgba(247, 214, 220, 0.25); }"
                   "QLabel#Note { color: %1; %2 font-size: 11px; background: transparent; }"
                   "QScrollBar:vertical { background: transparent; width: 6px; margin: 0; }"
                   "QScrollBar::handle:vertical { background: rgba(247, 214, 220, 0.22); min-height: 24px; }"
                   "QScrollBar::add-line, QScrollBar::sub-line { height: 0; }"
                   "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }")
            .arg(QLatin1String(kTypeMuted),
                    css(Face::Label),
                    QLatin1String(kType),
                    css(Face::Body),
                    QLatin1String(kAction),
                    QLatin1String(kFaint));
}
} // namespace

RoomChat::RoomChat(Site* pSite, QWidget* pParent)
        : QWidget(pParent),
          m_pSite(pSite),
          m_pSocket(new WebSocketClient(this)) {
    setObjectName(QStringLiteral("RoomChat"));
    setStyleSheet(chatStyle());
    // CHAT, the messages (newest at the bottom), and where you type under
    // them (like any chat).
    auto* pCol = new QVBoxLayout(this);
    m_pLayout = pCol;
    pCol->setContentsMargins(0, 0, 0, 0);
    pCol->setSpacing(6);
    m_pTitle = new QLabel(tr("chat"), this);
    m_pTitle->setObjectName(QStringLiteral("Title"));
    theme::caps(m_pTitle);
    pCol->addWidget(m_pTitle);
    m_pMessages = new QTextBrowser(this);
    m_pMessages->setOpenExternalLinks(true);
    m_pMessages->setFocusPolicy(Qt::NoFocus);
    m_pMessages->document()->setDocumentMargin(0);
    m_pMessages->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    pCol->addWidget(m_pMessages, 1);
    m_pInput = new QLineEdit(this);
    m_pInput->setPlaceholderText(tr("say something"));
    m_pInput->setMaxLength(500);
    connect(m_pInput, &QLineEdit::returnPressed, this, &RoomChat::send);
    pCol->addSpacing(4);
    pCol->addWidget(m_pInput);
    m_pNote = new QLabel(this);
    m_pNote->setObjectName(QStringLiteral("Note"));
    m_pNote->hide();
    pCol->addWidget(m_pNote);

    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &RoomChat::connectNow);
    connect(m_pSocket, &WebSocketClient::opened, this, [this] {
        m_wait = 1000;
        m_pNote->clear();
        m_pNote->hide();
    });
    connect(m_pSocket, &WebSocketClient::textReceived, this, &RoomChat::received);
    connect(m_pSocket, &WebSocketClient::closed, this, [this](const QString&) {
        if (!m_roomId.isEmpty()) {
            m_retry.start(m_wait);
            m_wait = std::min(m_wait * 2, 15000);
        }
    });
    setRoom(QString(), QString());
}

RoomChat::~RoomChat() {
    m_roomId.clear();
    m_pSocket->disconnect(this);
    m_pSocket->close();
}

void RoomChat::setRoom(const QString& roomId, const QString& invite) {
    if (roomId == m_roomId && invite == m_invite) {
        return;
    }
    m_roomId.clear(); // no reconnecting the old one
    m_retry.stop();
    m_pSocket->close();
    m_roomId = roomId;
    m_invite = invite;
    m_room = QJsonObject();
    m_messages.clear();
    m_unread = 0;
    emit unreadChanged(0);
    m_pInput->setEnabled(!roomId.isEmpty());
    m_pNote->setText(roomId.isEmpty() ? tr("the chat is your room's. host a room or go into one.") : QString());
    m_pNote->setVisible(!m_pNote->text().isEmpty());
    emit peopleChanged(QJsonArray());
    render();
    if (!roomId.isEmpty()) {
        m_wait = 1000;
        m_retry.start(0);
    }
}

void RoomChat::connectNow() {
    if (m_roomId.isEmpty()) {
        return;
    }
    QUrl url = m_pSite->url(QStringLiteral("/ws/rooms/%1").arg(m_roomId));
    if (!m_invite.isEmpty()) {
        url.setQuery(QUrlQuery{{QStringLiteral("invite"), m_invite}});
    }
    m_pSocket->open(url, m_pSite->token());
}

void RoomChat::setSlim(bool slim) {
    if (slim == m_slim) {
        return;
    }
    m_slim = slim;
    m_pTitle->setVisible(!slim);
    // Slim: the last message and the input side by side, one line.
    m_pLayout->setDirection(slim ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    m_pLayout->setSpacing(slim ? 16 : 6);
    m_pInput->setMaximumWidth(slim ? 360 : QWIDGETSIZE_MAX);
    m_pMessages->setVerticalScrollBarPolicy(slim ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    render();
}

void RoomChat::setOpen(bool open) {
    m_open = open;
    if (open && m_unread) {
        m_unread = 0;
        emit unreadChanged(0);
    }
}

void RoomChat::received(const QString& text) {
    const QJsonObject m = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = m.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("hello")) {
        m_room = m.value(QStringLiteral("room")).toObject();
        emit roomChanged(m_room);
        m_messages.clear();
        for (const QJsonValue& v : m.value(QStringLiteral("messages")).toArray()) {
            m_messages.append(v.toObject());
        }
        std::sort(m_messages.begin(), m_messages.end(), [](const QJsonObject& a, const QJsonObject& b) {
            return a.value(QStringLiteral("id")).toDouble() < b.value(QStringLiteral("id")).toDouble();
        });
        render();
    } else if (type == QLatin1String("chat")) {
        m_messages.append(m.value(QStringLiteral("message")).toObject());
        while (m_messages.size() > kMaxChat) {
            m_messages.removeFirst();
        }
        render();
        if (!m_open) {
            emit unreadChanged(++m_unread);
        }
    } else if (type == QLatin1String("deleted")) {
        const double id = m.value(QStringLiteral("id")).toDouble();
        m_messages.erase(std::remove_if(m_messages.begin(),
                                 m_messages.end(),
                                 [id](const QJsonObject& o) {
                                     return o.value(QStringLiteral("id")).toDouble() == id;
                                 }),
                m_messages.end());
        render();
    } else if (type == QLatin1String("room")) {
        m_room = m.value(QStringLiteral("room")).toObject();
        emit roomChanged(m_room);
    } else if (type == QLatin1String("presence")) {
        emit peopleChanged(m.value(QStringLiteral("people")).toArray());
    } else if (type == QLatin1String("error")) {
        m_pNote->setText(m.value(QStringLiteral("error")).toString().toLower());
        m_pNote->show();
    }
}

void RoomChat::render() {
    QScrollBar* pBar = m_pMessages->verticalScrollBar();
    const bool atEnd = pBar->value() >= pBar->maximum() - 4;
    // Names in copper, in the label face; the words in the body face.
    QString html;
    if (!m_roomId.isEmpty() && m_messages.isEmpty()) {
        html = QStringLiteral("<p style='color:%1; margin:0'>%2</p>")
                       .arg(QLatin1String(theme::kTypeMuted), tr("no messages yet. say hello."));
    }
    const int from = m_slim ? std::max(0, static_cast<int>(m_messages.size()) - 1) : 0;
    for (int i = from; i < m_messages.size(); ++i) {
        const QJsonObject& msg = m_messages[i];
        const QString name = msg.value(QStringLiteral("user")).toObject().value(QStringLiteral("name")).toString();
        html += QStringLiteral(
                "<p style='margin:0 0 9px 0'><span style=\"font-family:'DM Mono'; font-size:12px; color:%1\">%2</span>"
                "&nbsp;&nbsp;%3</p>")
                        .arg(QLatin1String(theme::kCopper),
                                name.toLower().toHtmlEscaped(),
                                msg.value(QStringLiteral("text")).toString().toHtmlEscaped());
    }
    m_pMessages->setHtml(html);
    if (atEnd || m_slim) {
        pBar->setValue(pBar->maximum());
    }
}

void RoomChat::send() {
    const QString text = m_pInput->text().simplified();
    if (text.isEmpty()) {
        return;
    }
    const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("type"), QStringLiteral("chat")},
                                                  {QStringLiteral("text"), text}})
                                    .toJson(QJsonDocument::Compact);
    if (!m_pSocket->sendText(QString::fromUtf8(json))) {
        m_pNote->setText(tr("not connected to the chat. trying again."));
        m_pNote->show();
        return;
    }
    m_pInput->clear();
    m_pNote->clear();
    m_pNote->hide();
}

} // namespace mixxx::b2b
