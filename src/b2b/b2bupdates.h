#pragma once

#include <QObject>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;

namespace mixxx::b2b {

/// Is there a newer PitchRide? Reads latest.json from the downloads site
/// (https://downloads.pitchride.co.uk/latest.json, or B2B_UPDATES_URL) when
/// the app starts and every few hours:
///   {"version", "min_version", "page", "downloads": {"windows" | "mac-intel" |
///    "mac-apple-silicon": {"version", "file", "size", "sha256"}}}
/// Newer for this computer: say so (and where to get it). Older than
/// min_version: too old to play (the app shows only the way to update).
/// No answer, or a computer with no download: nothing changes.
/// The installer itself is private: the site hands a signed-in app a link
/// (Updater). Its size and sha256 here are what the download must match.
class UpdateCheck : public QObject {
    Q_OBJECT
  public:
    explicit UpdateCheck(QObject* pParent = nullptr);

    /// This app's version (b2b::kVersion).
    static QString currentVersion();
    /// "1.2.3" against "1.10.0": <0, 0, >0.
    static int compare(const QString& a, const QString& b);

    /// A newer version for this computer ("" if none).
    QString newerVersion() const {
        return m_newer;
    }
    /// latest.json has been read at least once (until then, "no newer
    /// version" only means "not known yet").
    bool checked() const {
        return m_checked;
    }
    /// Older than the oldest allowed to play.
    bool required() const {
        return m_required;
    }
    /// Where to get it: the website's download page.
    QUrl page() const {
        return m_page;
    }
    /// The newer one's installer: its size in bytes and sha256 (64 hex
    /// characters, lowercase). 0 and "": not known.
    qint64 newerSize() const {
        return m_newerSize;
    }
    QString newerSha256() const {
        return m_newerSha256;
    }
    /// This computer's download in latest.json ("windows", "mac-intel",
    /// "mac-apple-silicon"; "" where there is none).
    static QString platform();

  signals:
    void changed();

  private:
    void check();

    QNetworkAccessManager* m_pNet;
    QTimer m_timer;
    bool m_checked = false;
    QString m_newer;
    qint64 m_newerSize = 0;
    QString m_newerSha256;
    bool m_required = false;
    QUrl m_page;
};

} // namespace mixxx::b2b
