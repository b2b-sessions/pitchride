#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QResizeEvent>

#include "widget/openglwindow.h"
#include "widget/tooltipqopengl.h"
#include "widget/wglwidget.h"

namespace {
QList<WGLWidget*>& registry() {
    static QList<WGLWidget*> s_widgets;
    return s_widgets;
}
} // namespace

std::atomic<int> WGLWidget::s_captureGeneration{0};

const QList<WGLWidget*>& WGLWidget::allWidgets() {
    return registry();
}

void WGLWidget::requestCapture(bool on) {
    if (!on) {
        s_captureGeneration = 0;
        for (WGLWidget* pWidget : registry()) {
            pWidget->m_capturedFrame = QImage();
            pWidget->m_capturedGeneration = 0;
        }
        return;
    }
    int next = s_captureGeneration.load() + 1;
    if (next <= 0) {
        next = 1;
    }
    s_captureGeneration = next;
}

void WGLWidget::captureBeforeSwap() {
    const int generation = s_captureGeneration.load();
    if (generation == 0 || generation == m_capturedGeneration) {
        return;
    }
    QOpenGLContext* pContext = m_pOpenGLWindow->context();
    if (!pContext || QOpenGLContext::currentContext() != pContext) {
        return;
    }
    m_capturedGeneration = generation;
    const qreal ratio = m_pOpenGLWindow->devicePixelRatio();
    const int w = static_cast<int>(m_pOpenGLWindow->width() * ratio);
    const int h = static_cast<int>(m_pOpenGLWindow->height() * ratio);
    if (w <= 0 || h <= 0) {
        return;
    }
    QOpenGLFunctions* f = pContext->functions();
    GLint previous = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    f->glBindFramebuffer(GL_FRAMEBUFFER, pContext->defaultFramebufferObject());
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    QImage frame(w, h, QImage::Format_RGBX8888);
    f->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, frame.bits());
    f->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    // GL rows run bottom to top.
    m_capturedFrame = frame.mirrored(false, true);
}

WGLWidget::WGLWidget(QWidget* pParent)
        : QWidget(pParent),
          m_pOpenGLWindow(nullptr),
          m_pContainerWidget(nullptr),
          m_pTrackDropTarget(nullptr) {
    registry().append(this);
    // When the widget is resized or moved, the QOpenGLWindow visibly resizes
    // or moves before the widgets do. This can be solved by calling
    //   setAttribute(Qt::WA_PaintOnScreen);
    // here, but this comes with a clear performance penalty and drop in
    // frame rate.
}

WGLWidget::~WGLWidget() {
    registry().removeAll(this);
    ToolTipQOpenGL::singleton().stop();
    if (m_pOpenGLWindow) {
        m_pOpenGLWindow->widgetDestroyed();
    }
}

QPaintDevice* WGLWidget::paintDevice() {
    makeCurrentIfNeeded();
    return m_pOpenGLWindow;
}

void WGLWidget::setTrackDropTarget(TrackDropTarget* pTarget) {
    m_pTrackDropTarget = pTarget;
}

TrackDropTarget* WGLWidget::trackDropTarget() const {
    return m_pTrackDropTarget;
}

void WGLWidget::showEvent(QShowEvent* event) {
    if (!m_pOpenGLWindow) {
        m_pOpenGLWindow = new OpenGLWindow(this);
        m_pContainerWidget = createWindowContainer(m_pOpenGLWindow, this);
        m_pContainerWidget->resize(size());
        m_pContainerWidget->show();
        m_pContainerWidget->setAutoFillBackground(true);
    }
    QWidget::showEvent(event);
}

void WGLWidget::resizeEvent(QResizeEvent* event) {
    if (m_pContainerWidget) {
        m_pContainerWidget->resize(event->size());
    }
    QWidget::resizeEvent(event);
}

bool WGLWidget::isContextValid() const {
    return m_pOpenGLWindow && m_pOpenGLWindow->context() && m_pOpenGLWindow->context()->isValid();
}

void WGLWidget::makeCurrentIfNeeded() {
    if (m_pOpenGLWindow && m_pOpenGLWindow->context() != QOpenGLContext::currentContext()) {
        m_pOpenGLWindow->makeCurrent();
    }
}

void WGLWidget::doneCurrent() {
    if (m_pOpenGLWindow) {
        m_pOpenGLWindow->doneCurrent();
    }
}

void WGLWidget::paintGL() {
    // to be implemented in derived widgets if needed
}

void WGLWidget::initializeGL() {
    // to be implemented in derived widgets if needed
}

void WGLWidget::resizeGL(int w, int h) {
    Q_UNUSED(w);
    Q_UNUSED(h);
    // to be implemented in derived widgets if needed
}

void WGLWidget::swapBuffers() {
    if (shouldRender()) {
        captureBeforeSwap();
        m_pOpenGLWindow->context()->swapBuffers(m_pOpenGLWindow->context()->surface());
    }
}

bool WGLWidget::shouldRender() const {
    return m_pOpenGLWindow && m_pOpenGLWindow->isExposed();
}

QRegion WGLWidget::capturedVisibleRegion() const {
    // The GL window's container covers this widget, so this widget's own
    // visibleRegion() is empty: ask the container.
    return m_pContainerWidget ? m_pContainerWidget->visibleRegion() : QRegion();
}

QOpenGLWindow* WGLWidget::getOpenGLWindow() const {
    return m_pOpenGLWindow;
}
