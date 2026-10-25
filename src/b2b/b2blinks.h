#pragma once

#include <QObject>
#include <QPointer>
#include <QStringList>

class QLocalServer;

namespace mixxx::b2b {

/// pitchride:// links: the website's "Open in the app" button opens one
/// (pitchride://room?id=...): the app comes to the front on its Rooms page.
///
/// The app registers itself for them (Windows: HKCU\Software\Classes; Linux:
/// a .desktop file and xdg-mime), so the browser starts it with the link as
/// an argument. If the app is open already, that new copy hands the link to
/// it over a local socket and quits.
namespace links {

/// Takes the links out of the command line (so they aren't loaded as tracks).
QStringList takeFromArgs(int* pArgc, char** argv);
/// Hands them to the app that's already running; false if none is.
bool forwardToRunningApp(const QStringList& links);
/// Links the app was started with, for the controller once the window is up.
void setPending(const QStringList& links);
QStringList takePending();
/// Tell the system that pitchride:// links open this app.
void registerScheme();

} // namespace links

/// Receives links from later copies of the app (see above).
class LinkServer : public QObject {
    Q_OBJECT
  public:
    explicit LinkServer(QObject* pParent = nullptr);
    ~LinkServer() override;

  signals:
    void linkReceived(const QString& link);

  private:
    QPointer<QLocalServer> m_pServer;
};

} // namespace mixxx::b2b
