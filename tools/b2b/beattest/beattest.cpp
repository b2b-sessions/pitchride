// Runs Mixxx's beat analysis on raw audio, for measuring and improving it.
//
//   ffmpeg -i track.mp3 -f f32le -ac 2 -ar 44100 track.f32
//   b2b-beattest track.f32 [qm|b2b] [constant 0|1]
//
// Prints: bpm, first beat (seconds), and every grid beat in the first 20 s.
#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <cstdio>
#include <vector>

#include "analyzer/plugins/analyzerqueenmarybeats.h"
#include "analyzer/plugins/analyzerb2bbeats.h"
#include "track/beatfactory.h"
#include "track/beats.h"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s track.f32 [qm|b2b] [constant 0|1]\n", argv[0]);
        return 2;
    }
    QFile f(QString::fromLocal8Bit(argv[1]));
    if (!f.open(QIODevice::ReadOnly)) {
        return 1;
    }
    const QByteArray raw = f.readAll();
    const auto* samples = reinterpret_cast<const CSAMPLE*>(raw.constData());
    const SINT count = raw.size() / static_cast<SINT>(sizeof(CSAMPLE));
    const QString which = argc > 2 ? QString::fromLatin1(argv[2]) : QStringLiteral("qm");
    const bool constant = argc > 3 ? QString::fromLatin1(argv[3]) != QStringLiteral("0") : true;
    const auto sampleRate = mixxx::audio::SampleRate(44100);

    std::unique_ptr<mixxx::AnalyzerBeatsPlugin> pPlugin;
    if (which == QStringLiteral("b2b")) {
        pPlugin = std::make_unique<mixxx::AnalyzerB2BBeats>();
    } else {
        pPlugin = std::make_unique<mixxx::AnalyzerQueenMaryBeats>();
    }
    pPlugin->initialize(sampleRate);
    constexpr SINT kBlock = 4096 * 2;
    for (SINT i = 0; i < count; i += kBlock) {
        pPlugin->processSamples(samples + i, std::min(kBlock, count - i));
    }
    pPlugin->finalize();

    mixxx::BeatsPointer pBeats;
    if (pPlugin->supportsBeatTracking()) {
        pBeats = BeatFactory::makePreferredBeats(pPlugin->getBeats(), {}, constant, sampleRate);
    } else {
        pBeats = mixxx::Beats::fromConstTempo(sampleRate, mixxx::audio::kStartFramePos, pPlugin->getBpm());
    }
    if (!pBeats) {
        std::printf("none\n");
        return 0;
    }
    const auto end = mixxx::audio::FramePos(count / 2.0);
    std::printf("bpm %.4f\n", pBeats->getBpmInRange(mixxx::audio::kStartFramePos, end).value());
    std::printf("first %.5f\n", pBeats->firstBeat().value() / 44100.0);
    std::printf("beats");
    for (auto it = pBeats->cbegin(); it != pBeats->cend() && it->value() < 44100.0 * 120; ++it) {
        std::printf(" %.5f", it->value() / 44100.0);
    }
    std::printf("\n");
    return 0;
}
