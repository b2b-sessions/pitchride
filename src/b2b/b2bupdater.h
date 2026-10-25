#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QNetworkAccessManager;
class QNetworkReply;

namespace mixxx::b2b {

class Site;
class UpdateCheck;

/// Gets the new PitchRide and installs it, so nobody has to go back to the
/// website for each version.
///
/// When UpdateCheck finds a newer version, the updater asks the site for
/// the installer's private link (the same one the website's Download button
/// gets: signed in, and only for accounts that are in), downloads it
/// quietly, and checks its size and sha256 against latest.json. A file that
/// doesn't match is thrown away. Then it is ready.
///
/// Installing runs once the app has closed: a small script waits for this
/// app to be gone, runs the installer, and opens PitchRide again. The
/// installer replaces the old version (it keeps the same upgrade code).
///
/// On a Mac the installer is a disk image: the script checks the app in it
/// is signed by the same developer and accepted by macOS, then swaps it in
/// for this one (so PitchRide isn't sandboxed: a sandboxed app can't).
///
/// Windows and Mac. Elsewhere (and when anything goes wrong) the app keeps
/// sending people to the website, as before.
class Updater : public QObject {
    Q_OBJECT
  public:
    enum class State {
        Idle,    ///< nothing to get, or not started
        Getting, ///< downloading (percent())
        Ready,   ///< downloaded and checked: install() can run it
        Failed,  ///< didn't work (problem()): the website still does
    };

    Updater(Site* pSite, UpdateCheck* pCheck, QObject* pParent = nullptr);
    ~Updater() override;

    /// This computer can install updates itself (Windows; or a test).
    static bool supported();
    /// There's a newer version this app could get itself: supported, the
    /// account signed in and in, and latest.json gave its size and sha256.
    bool canGet() const;

    State state() const {
        return m_state;
    }
    /// While Getting: 0 to 100.
    int percent() const {
        return m_percent;
    }
    /// The version that's Ready (or being got).
    QString version() const {
        return m_version;
    }
    /// What went wrong, in the app's voice (Failed).
    QString problem() const {
        return m_problem;
    }

    /// Start getting the newer version (if it can and isn't already).
    void get();
    /// Ready: start the script that installs it once this app has closed,
    /// then opens PitchRide again. True if it started; the caller then
    /// closes the app.
    bool install();

    /// Where installers are kept while waiting to be installed.
    static QString folder();

  signals:
    void changed();

  private:
    void checkChanged();
    void gotLink(const QString& url, const QString& version);
    void readSome();
    void finished();
    void fail(const QString& problem);
    void setState(State state);
    void cancel();
    /// The installer's name in folder(), for a version ("" if not a
    /// version we'd write into a file name).
    static QString fileFor(const QString& version);
    /// An installer for the version that's already here and matches.
    bool alreadyHere();
    /// Old installers and half downloads: gone.
    void tidy(const QString& keep);

    Site* m_pSite;
    UpdateCheck* m_pCheck;
    QNetworkAccessManager* m_pNet;
    QPointer<QNetworkReply> m_pReply;
    QFile m_part;
    QCryptographicHash m_hash;
    qint64 m_got = 0;
    qint64 m_size = 0;
    QString m_sha256;
    QString m_version;
    State m_state = State::Idle;
    int m_percent = 0;
    QString m_problem;
    QTimer m_retry; // after a failed background download, try again later
};

} // namespace mixxx::b2b
