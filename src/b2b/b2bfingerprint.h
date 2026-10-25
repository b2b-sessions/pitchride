#pragma once

#include <QString>
#include <cstdint>

namespace mixxx::b2b {

/// Identifies an audio file by its contents, so two DJs can tell whether
/// they have the same track even if it lives at a different path or has a
/// different file name.
///
/// FNV-1a over the file size, the first 1 MiB and the last 1 MiB. Fast enough
/// to run when a track loads. Returns 0 if the file cannot be read.
uint64_t fingerprintFile(const QString& path);

QString fingerprintToHex(uint64_t fingerprint);
uint64_t fingerprintFromHex(const QString& hex);

} // namespace mixxx::b2b
