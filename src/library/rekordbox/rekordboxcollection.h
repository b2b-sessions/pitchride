#pragma once

#include <QString>
#include <QStringList>

/// B2B: the rekordbox collection on this computer (rekordbox 6 / 7's
/// master.db), read so rekordbox users find their music and playlists in
/// the app with no export step.
///
/// master.db is an SQLCipher 4 database (AES-256-CBC pages, PBKDF2-SHA512
/// key) with a fixed key that every rekordbox install uses (the same one
/// pyrekordbox uses). decrypt() writes a plain SQLite copy we can read.
namespace rekordboxcollection {

/// Where rekordbox keeps master.db on this computer ("" if none found):
/// rekordbox's options.json (db-path), else its default folder.
QString findMasterDb();

/// Decrypt master.db into a plain SQLite file at outPath. Empty on success,
/// else why not.
QString decrypt(const QString& masterDbPath, const QString& outPath);

/// Whether this file looks like a rekordbox collection (master.db).
bool isMasterDb(const QString& path);

} // namespace rekordboxcollection
