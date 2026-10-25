#pragma once

#include <QFile>
#include <QTimer>

#include "controllers/controllerenumerator.h"
#include "controllers/midi/midicontroller.h"

namespace mixxx::b2b {

/// Development builds only (B2B_DEV_FAKE_MIDI), for testing without
/// hardware: B2B_FAKE_MIDI=<file> adds a MIDI controller called
/// B2B_FAKE_MIDI_NAME (default "DDJ-200") that plays every line appended to
/// the file as a MIDI message: "b0 13 7f" (status, control, value in hex).
class FakeMidiController : public MidiController {
    Q_OBJECT
  public:
    FakeMidiController(const QString& name, const QString& path);
    ~FakeMidiController() override;

    int open() override;
    int close() override;
    void sendShortMsg(unsigned char status, unsigned char byte1, unsigned char byte2) override {
        Q_UNUSED(status);
        Q_UNUSED(byte1);
        Q_UNUSED(byte2);
    }
    void sendBytes(const QByteArray& data) override {
        Q_UNUSED(data);
    }

  private:
    void readMore();

    QString m_path;
    qint64 m_read = 0;
    QTimer m_poll;
};

class FakeMidiEnumerator : public ControllerEnumerator {
  public:
    FakeMidiEnumerator(const QString& name, const QString& path);
    ~FakeMidiEnumerator() override;
    QList<Controller*> queryDevices() override;

  private:
    QString m_name;
    QString m_path;
    QList<Controller*> m_devices;
};

} // namespace mixxx::b2b
