#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QUrl>

class QTcpSocket;

namespace mixxx::b2b {

/// A small WebSocket client (RFC 6455) over Qt's TCP / TLS sockets, enough
/// to send and hear messages from the B2B website, text and binary. (Qt's own WebSockets module isn't among Mixxx's dependencies.)
class WebSocketClient : public QObject {
    Q_OBJECT
  public:
    explicit WebSocketClient(QObject* pParent = nullptr);
    ~WebSocketClient() override;

    /// ws:// or wss:// (http:// and https:// are taken to mean the same).
    void open(const QUrl& url, const QByteArray& bearerToken);
    void close();
    bool isOpen() const {
        return m_state == State::Open;
    }
    /// False when not open.
    bool sendBinary(const QByteArray& data);
    bool sendText(const QString& text);
    /// The largest message taken from the server (1 MB unless set).
    void setMaxIncoming(qint64 bytes) {
        m_maxIncoming = bytes;
    }
    /// Bytes handed over but not yet on the network.
    qint64 pendingBytes() const;

  signals:
    void opened();
    void closed(const QString& reason);
    void textReceived(const QString& text);
    void binaryReceived(const QByteArray& data);

  private:
    enum class State { Idle, Connecting, Handshake, Open };
    void onConnected();
    void onReadyRead();
    void onGone(const QString& reason);
    void readFrames();
    void deliver(quint8 opcode, const QByteArray& payload);
    void writeFrame(quint8 opcode, const QByteArray& payload);

    QPointer<QTcpSocket> m_pSocket;
    State m_state = State::Idle;
    QUrl m_url;
    QByteArray m_token;
    QByteArray m_key;
    QByteArray m_buffer;
    qint64 m_maxIncoming = 1 << 20;
    // A message sent in pieces (continuation frames).
    quint8 m_partOpcode = 0;
    QByteArray m_part;
};

} // namespace mixxx::b2b
