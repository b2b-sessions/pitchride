#include "b2b/b2bwebsocket.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QtEndian>
#ifndef QT_NO_SSL
#include <QSslSocket>
#endif

#include "moc_b2bwebsocket.cpp"

namespace mixxx::b2b {

namespace {
const QByteArray kGuid = QByteArrayLiteral("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
constexpr quint8 kText = 0x1;
constexpr quint8 kBinary = 0x2;
constexpr quint8 kClose = 0x8;
constexpr quint8 kPing = 0x9;
constexpr quint8 kPong = 0xA;
constexpr quint8 kContinuation = 0x0;
} // namespace

WebSocketClient::WebSocketClient(QObject* pParent)
        : QObject(pParent) {
}

WebSocketClient::~WebSocketClient() {
    if (m_pSocket) {
        m_pSocket->disconnect(this);
        m_pSocket->abort();
        delete m_pSocket;
    }
}

void WebSocketClient::open(const QUrl& url, const QByteArray& bearerToken) {
    close();
    m_url = url;
    m_token = bearerToken;
    m_buffer.clear();
    const QString scheme = url.scheme().toLower();
    const bool secure = scheme == QLatin1String("wss") || scheme == QLatin1String("https");
    QTcpSocket* pSocket = nullptr;
    if (secure) {
#ifndef QT_NO_SSL
        auto* pSsl = new QSslSocket(this);
        connect(pSsl, &QSslSocket::encrypted, this, &WebSocketClient::onConnected);
        pSsl->connectToHostEncrypted(url.host(), static_cast<quint16>(url.port(443)));
        pSocket = pSsl;
#else
        emit closed(QStringLiteral("this build can't make secure connections"));
        return;
#endif
    } else {
        pSocket = new QTcpSocket(this);
        connect(pSocket, &QTcpSocket::connected, this, &WebSocketClient::onConnected);
        pSocket->connectToHost(url.host(), static_cast<quint16>(url.port(80)));
    }
    m_pSocket = pSocket;
    m_state = State::Connecting;
    pSocket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(pSocket, &QTcpSocket::readyRead, this, &WebSocketClient::onReadyRead);
    connect(pSocket, &QTcpSocket::disconnected, this, [this] {
        onGone(QStringLiteral("connection closed"));
    });
    connect(pSocket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        onGone(m_pSocket ? m_pSocket->errorString() : QStringLiteral("connection failed"));
    });
}

void WebSocketClient::close() {
    if (!m_pSocket) {
        m_state = State::Idle;
        return;
    }
    if (m_state == State::Open) {
        writeFrame(kClose, QByteArray::fromHex("03e8")); // 1000: normal
        m_pSocket->flush();
    }
    QTcpSocket* pSocket = m_pSocket;
    m_pSocket = nullptr;
    m_state = State::Idle;
    pSocket->disconnect(this);
    pSocket->disconnectFromHost();
    pSocket->deleteLater();
}

void WebSocketClient::onGone(const QString& reason) {
    if (m_state == State::Idle) {
        return;
    }
    QTcpSocket* pSocket = m_pSocket;
    m_pSocket = nullptr;
    m_state = State::Idle;
    if (pSocket) {
        pSocket->disconnect(this);
        pSocket->abort();
        pSocket->deleteLater();
    }
    emit closed(reason);
}

void WebSocketClient::onConnected() {
    QByteArray nonce(16, '\0');
    for (auto& c : nonce) {
        c = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }
    m_key = nonce.toBase64();
    QByteArray path = m_url.path(QUrl::FullyEncoded).toUtf8();
    if (path.isEmpty()) {
        path = "/";
    }
    if (m_url.hasQuery()) {
        path += '?' + m_url.query(QUrl::FullyEncoded).toUtf8();
    }
    QByteArray host = m_url.host().toUtf8();
    if (m_url.port() > 0) {
        host += ':' + QByteArray::number(m_url.port());
    }
    QByteArray request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + m_key +
            "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: Back2Back\r\n";
    if (!m_token.isEmpty()) {
        request += "Authorization: Bearer " + m_token + "\r\n";
    }
    request += "\r\n";
    m_state = State::Handshake;
    m_pSocket->write(request);
}

void WebSocketClient::onReadyRead() {
    if (!m_pSocket) {
        return;
    }
    m_buffer += m_pSocket->readAll();
    if (m_state == State::Handshake) {
        const int end = m_buffer.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_buffer.size() > 16384) {
                onGone(QStringLiteral("the server's answer was too long"));
            }
            return;
        }
        const QByteArray head = m_buffer.left(end);
        m_buffer.remove(0, end + 4);
        const QList<QByteArray> lines = head.split('\n');
        const QByteArray status = lines.value(0).trimmed();
        if (!status.contains(" 101")) {
            // "HTTP/1.1 403 Forbidden": say what the server said.
            onGone(QString::fromUtf8(status.mid(status.indexOf(' ') + 1)));
            return;
        }
        const QByteArray expected =
                QCryptographicHash::hash(QByteArray(m_key + kGuid), QCryptographicHash::Sha1).toBase64();
        bool accepted = false;
        for (const QByteArray& line : lines) {
            const int colon = line.indexOf(':');
            if (colon > 0 &&
                    line.left(colon).trimmed().toLower() == "sec-websocket-accept" &&
                    line.mid(colon + 1).trimmed() == expected) {
                accepted = true;
            }
        }
        if (!accepted) {
            onGone(QStringLiteral("not a WebSocket server"));
            return;
        }
        m_state = State::Open;
        emit opened();
    }
    if (m_state == State::Open) {
        readFrames();
    }
}

void WebSocketClient::readFrames() {
    while (m_state == State::Open && m_buffer.size() >= 2) {
        const auto* p = reinterpret_cast<const uchar*>(m_buffer.constData());
        const bool fin = p[0] & 0x80;
        const quint8 opcode = p[0] & 0x0F;
        const bool masked = p[1] & 0x80;
        quint64 length = p[1] & 0x7F;
        int at = 2;
        if (length == 126) {
            if (m_buffer.size() < 4) {
                return;
            }
            length = qFromBigEndian<quint16>(p + 2);
            at = 4;
        } else if (length == 127) {
            if (m_buffer.size() < 10) {
                return;
            }
            length = qFromBigEndian<quint64>(p + 2);
            at = 10;
        }
        if (length > static_cast<quint64>(m_maxIncoming) ||
                static_cast<qint64>(m_part.size() + length) > m_maxIncoming) {
            onGone(QStringLiteral("the server sent too much"));
            return;
        }
        QByteArray mask;
        if (masked) {
            if (m_buffer.size() < at + 4) {
                return;
            }
            mask = m_buffer.mid(at, 4);
            at += 4;
        }
        if (static_cast<quint64>(m_buffer.size()) < at + length) {
            return;
        }
        QByteArray payload = m_buffer.mid(at, static_cast<int>(length));
        m_buffer.remove(0, at + static_cast<int>(length));
        if (masked) {
            for (int i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            }
        }
        if (opcode == kText || opcode == kBinary || opcode == kContinuation) {
            if (opcode != kContinuation) {
                m_partOpcode = opcode;
                m_part.clear();
            }
            if (fin && m_part.isEmpty()) {
                deliver(m_partOpcode, payload);
            } else {
                m_part.append(payload);
                if (fin) {
                    const QByteArray whole = std::move(m_part);
                    m_part = QByteArray();
                    deliver(m_partOpcode, whole);
                }
            }
            continue;
        }
        switch (opcode) {
        case kPing:
            writeFrame(kPong, payload);
            break;
        case kClose: {
            QString reason = QStringLiteral("the server closed the connection");
            if (payload.size() > 2) {
                reason = QString::fromUtf8(payload.mid(2));
            }
            writeFrame(kClose, payload.left(2));
            onGone(reason);
            return;
        }
        default:
            break; // pong: nothing for us
        }
    }
}

void WebSocketClient::writeFrame(quint8 opcode, const QByteArray& payload) {
    if (!m_pSocket) {
        return;
    }
    QByteArray frame;
    frame.reserve(payload.size() + 14);
    frame.append(static_cast<char>(0x80 | opcode));
    const qint64 n = payload.size();
    if (n < 126) {
        frame.append(static_cast<char>(0x80 | n));
    } else if (n < 65536) {
        frame.append(static_cast<char>(0x80 | 126));
        uchar len[2];
        qToBigEndian<quint16>(static_cast<quint16>(n), len);
        frame.append(reinterpret_cast<const char*>(len), 2);
    } else {
        frame.append(static_cast<char>(0x80 | 127));
        uchar len[8];
        qToBigEndian<quint64>(static_cast<quint64>(n), len);
        frame.append(reinterpret_cast<const char*>(len), 8);
    }
    const quint32 maskValue = QRandomGenerator::global()->generate();
    uchar mask[4];
    qToBigEndian<quint32>(maskValue, mask);
    frame.append(reinterpret_cast<const char*>(mask), 4);
    const int start = frame.size();
    frame.append(payload);
    char* data = frame.data() + start;
    for (qint64 i = 0; i < n; ++i) {
        data[i] = static_cast<char>(data[i] ^ mask[i & 3]);
    }
    m_pSocket->write(frame);
}

void WebSocketClient::deliver(quint8 opcode, const QByteArray& payload) {
    if (opcode == kText) {
        emit textReceived(QString::fromUtf8(payload));
    } else {
        emit binaryReceived(payload);
    }
}

bool WebSocketClient::sendText(const QString& text) {
    if (m_state != State::Open) {
        return false;
    }
    writeFrame(kText, text.toUtf8());
    return true;
}

bool WebSocketClient::sendBinary(const QByteArray& data) {
    if (m_state != State::Open) {
        return false;
    }
    writeFrame(kBinary, data);
    return true;
}

qint64 WebSocketClient::pendingBytes() const {
    if (!m_pSocket) {
        return 0;
    }
#ifndef QT_NO_SSL
    if (auto* pSsl = qobject_cast<QSslSocket*>(m_pSocket.data())) {
        return pSsl->bytesToWrite() + pSsl->encryptedBytesToWrite();
    }
#endif
    return m_pSocket->bytesToWrite();
}

} // namespace mixxx::b2b
