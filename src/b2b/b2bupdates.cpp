#include "b2b/b2bupdates.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QtGlobal>
#include <b2b/version.hpp>

#include "moc_b2bupdates.cpp"

namespace mixxx::b2b {

namespace {
constexpr int kEveryMs = 6 * 60 * 60 * 1000; // a few times a day
constexpr int kFirstMs = 4000;               // soon after starting

/// A sha256 as latest.json writes it: 64 hex characters (else "").
QString sha256Of(const QJsonObject& build) {
    const QString sha = build.value(QStringLiteral("sha256")).toString().toLower();
    if (sha.size() != 64) {
        return QString();
    }
    for (const QChar c : sha) {
        if (!c.isDigit() && (c < QLatin1Char('a') || c > QLatin1Char('f'))) {
            return QString();
        }
    }
    return sha;
}
} // namespace

QString UpdateCheck::platform() {
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM)
    return QStringLiteral("mac-apple-silicon");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("mac-intel");
#else
    return qEnvironmentVariable("B2B_UPDATES_PLATFORM"); // (testing on Linux)
#endif
}

UpdateCheck::UpdateCheck(QObject* pParent)
        : QObject(pParent),
          m_pNet(new QNetworkAccessManager(this)),
          m_page(QStringLiteral("https://pitchride.co.uk/download")) {
    connect(&m_timer, &QTimer::timeout, this, &UpdateCheck::check);
    m_timer.start(kEveryMs);
    QTimer::singleShot(kFirstMs, this, &UpdateCheck::check);
}

QString UpdateCheck::currentVersion() {
    return QString::fromLatin1(::b2b::kVersion);
}

int UpdateCheck::compare(const QString& a, const QString& b) {
    const QStringList x = a.split(QLatin1Char('.'));
    const QStringList y = b.split(QLatin1Char('.'));
    for (int i = 0; i < std::max(x.size(), y.size()); ++i) {
        const int p = i < x.size() ? x[i].toInt() : 0;
        const int q = i < y.size() ? y[i].toInt() : 0;
        if (p != q) {
            return p < q ? -1 : 1;
        }
    }
    return 0;
}

void UpdateCheck::check() {
    const QString where = platform();
    if (where.isEmpty()) {
        return;
    }
    const QUrl url(qEnvironmentVariable("B2B_UPDATES_URL", QStringLiteral("https://downloads.pitchride.co.uk/latest.json")));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(15000);
    QNetworkReply* pReply = m_pNet->get(request);
    connect(pReply, &QNetworkReply::finished, this, [this, pReply, where] {
        pReply->deleteLater();
        if (pReply->error() != QNetworkReply::NoError) {
            return; // offline, or the site busy: try again later
        }
        const QJsonObject latest = QJsonDocument::fromJson(pReply->readAll()).object();
        const QJsonObject mine = latest.value(QStringLiteral("downloads")).toObject().value(where).toObject();
        const QString available = mine.value(QStringLiteral("version")).toString();
        const QString minimum = latest.value(QStringLiteral("min_version")).toString();
        const QString current = currentVersion();
        const QString newer = !available.isEmpty() && compare(available, current) > 0 ? available : QString();
        // Too old only if there is something newer to get.
        const bool required = !newer.isEmpty() && !minimum.isEmpty() && compare(current, minimum) < 0;
        // latest.json sits on a public bucket: whatever it says, the "get
        // the update" page is only ever one of ours.
        const QUrl page(latest.value(QStringLiteral("page")).toString());
        const QString host = page.host().toLower();
        if (page.isValid() && page.scheme() == QLatin1String("https") && page.userInfo().isEmpty() &&
                (host == QLatin1String("pitchride.co.uk") || host == QLatin1String("downloads.pitchride.co.uk"))) {
            m_page = page;
        }
        const qint64 size = newer.isEmpty() ? 0 : mine.value(QStringLiteral("size")).toInteger();
        const QString sha = newer.isEmpty() ? QString() : sha256Of(mine);
        const bool first = !m_checked;
        m_checked = true;
        if (first || newer != m_newer || required != m_required || size != m_newerSize || sha != m_newerSha256) {
            m_newer = newer;
            m_newerSize = size;
            m_newerSha256 = sha;
            m_required = required;
            qInfo() << "B2B: PitchRide" << current
                    << (newer.isEmpty() ? QStringLiteral("is up to date") : QStringLiteral("can update to ") + newer)
                    << (required ? "(required)" : "");
            emit changed();
        }
    });
}

} // namespace mixxx::b2b
