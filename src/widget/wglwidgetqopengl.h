#pragma once

#ifndef WGLWIDGET_H
#error "Do not include this file, include wglwidget.h instead"
#endif

#include <QImage>
#include <QList>
#include <QWidget>
#include <atomic>

////////////////////////////////
// QOpenGLWindow based WGLWidget
////////////////////////////////

class QPaintDevice;
class QOpenGLWindow;
class OpenGLWindow;
class TrackDropTarget;

class WGLWidget : public QWidget {
  public:
    WGLWidget(QWidget* parent);
    ~WGLWidget();

    bool isContextValid() const;

    bool shouldRender() const;

    void makeCurrentIfNeeded();
    void doneCurrent();

    void swapBuffers();

    // called (indirectly) by WaveformWidgetFactory
    virtual void paintGL();
    // called by OpenGLWindow
    virtual void resizeGL(int w, int h);
    virtual void initializeGL();

    void setTrackDropTarget(TrackDropTarget* pTarget);
    TrackDropTarget* trackDropTarget() const;

    QOpenGLWindow* getOpenGLWindow() const;

    // Screen sharing (B2B): a QWidget::grab() leaves GL widgets blank, so
    // while a capture is wanted each widget keeps a copy of the frame it
    // shows, read back just before its next swap.
    /// Every GL widget there is.
    static const QList<WGLWidget*>& allWidgets();
    /// Ask every widget for a fresh copy at its next swap (0 stops).
    static void requestCapture(bool on);
    /// The last frame this widget showed (null until one was read back).
    /// The part of this widget not covered by others (window coordinates
    /// relative to this widget).
    QRegion capturedVisibleRegion() const;
    const QImage& capturedFrame() const {
        return m_capturedFrame;
    }

  protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

    QPaintDevice* paintDevice();

  private:
    OpenGLWindow* m_pOpenGLWindow;
    QWidget* m_pContainerWidget;
    TrackDropTarget* m_pTrackDropTarget;

    void captureBeforeSwap();
    QImage m_capturedFrame;
    int m_capturedGeneration = 0;
    static std::atomic<int> s_captureGeneration;
};
