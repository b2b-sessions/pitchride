#include "b2b/b2bfakemidi.h"

#include <QFileInfo>

#include "moc_b2bfakemidi.cpp"
#include "util/time.h"

namespace mixxx::b2b {

FakeMidiController::FakeMidiController(const QString& name, const QString& path)
        : MidiController(name),
          m_path(path) {
    setDeviceCategory(QStringLiteral("Fake (testing)"));
    setInputDevice(true);
    setOutputDevice(true);
    m_poll.setInterval(50);
    connect(&m_poll, &QTimer::timeout, this, &FakeMidiController::readMore);
}

FakeMidiController::~FakeMidiController() {
    if (isOpen()) {
        close();
    }
}

int FakeMidiController::open() {
    if (isOpen()) {
        return -1;
    }
    // Only what's written from now on.
    m_read = QFileInfo(m_path).exists() ? QFileInfo(m_path).size() : 0;
    startEngine();
    applyMapping();
    setOpen(true);
    m_poll.start();
    qInfo() << "B2B: fake MIDI" << getName() << "reading" << m_path;
    return 0;
}

int FakeMidiController::close() {
    if (!isOpen()) {
        return -1;
    }
    m_poll.stop();
    stopEngine();
    MidiController::close();
    setOpen(false);
    return 0;
}

void FakeMidiController::readMore() {
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= m_read) {
        return;
    }
    file.seek(m_read);
    // Whole lines only (one may still be being written).
    const QByteArray fresh = file.readAll();
    const int end = fresh.lastIndexOf('\n');
    if (end < 0) {
        return;
    }
    m_read += end + 1;
    const QList<QByteArray> lines = fresh.left(end).split('\n');
    for (const QByteArray& line : lines) {
        const QList<QByteArray> parts = line.simplified().split(' ');
        if (parts.size() != 3) {
            continue;
        }
        bool ok1 = false;
        bool ok2 = false;
        bool ok3 = false;
        const auto status = static_cast<unsigned char>(parts[0].toUInt(&ok1, 16));
        const auto control = static_cast<unsigned char>(parts[1].toUInt(&ok2, 16));
        const auto value = static_cast<unsigned char>(parts[2].toUInt(&ok3, 16));
        if (ok1 && ok2 && ok3) {
            receivedShortMessage(status, control, value, mixxx::Time::elapsed());
        }
    }
}

FakeMidiEnumerator::FakeMidiEnumerator(const QString& name, const QString& path)
        : m_name(name),
          m_path(path) {
}

FakeMidiEnumerator::~FakeMidiEnumerator() {
    qDeleteAll(m_devices);
}

QList<Controller*> FakeMidiEnumerator::queryDevices() {
    if (m_devices.isEmpty()) {
        m_devices << new FakeMidiController(m_name, m_path);
    }
    return m_devices;
}

} // namespace mixxx::b2b
