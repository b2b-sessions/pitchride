#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <QWidget>

// This is a widget that is shown in the Mixxx main window
// until the skin is ready to use.
//
// B2B: PitchRide's own, whatever the skin (DESIGN.md 4): the page's blue,
// the loading maze (the route drawing in from the gap, the flower blooming,
// looping), the wordmark under it, and a thin loading bar in the action
// colour with what is loading. A skin's <LaunchImageStyle> is not used.
class LaunchImage: public QWidget {
    Q_OBJECT
  public:
    LaunchImage(QWidget* pParent, const QString& styleSheet);
    ~LaunchImage() override = default;
    void progress(int value, const QString& serviceName);

  protected:
    void paintEvent(QPaintEvent*) override;

  private:
    QElapsedTimer m_clock;
    QTimer m_frame;
    int m_target = 0;     // the progress reported (0..100)
    double m_shown = 0;   // the bar as drawn, catching up with it
    QString m_service;
};
