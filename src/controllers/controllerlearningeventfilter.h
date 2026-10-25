#pragma once

#include <QObject>

#include "control/controlobject.h"
#include "widget/controlwidgetconnection.h"

class QEvent;

struct ControlInfo {
    ControlInfo()
            : clickControl(NULL),
              emitOption(ControlParameterWidgetConnection::EMIT_ON_PRESS_AND_RELEASE),
              leftClickControl(NULL),
              leftEmitOption(ControlParameterWidgetConnection::EMIT_ON_PRESS_AND_RELEASE),
              rightClickControl(NULL),
              rightEmitOption(ControlParameterWidgetConnection::EMIT_ON_PRESS_AND_RELEASE) {
    }

    ControlObject* clickControl;
    ControlParameterWidgetConnection::EmitOption emitOption;
    ControlObject* leftClickControl;
    ControlParameterWidgetConnection::EmitOption leftEmitOption;
    ControlObject* rightClickControl;
    ControlParameterWidgetConnection::EmitOption rightEmitOption;
};

class ControllerLearningEventFilter : public QObject {
    Q_OBJECT
  public:
    ControllerLearningEventFilter(QObject* pParent = NULL);
    virtual ~ControllerLearningEventFilter();

    virtual bool eventFilter(QObject* pObject, QEvent* pEvent);

    void addWidgetClickInfo(QWidget* pWidget, Qt::MouseButton buttonState,
                            ControlObject* pControl,
                            ControlParameterWidgetConnection::EmitOption emitOption);

    /// B2B: the control a click on this skin widget is for (its left-click
    /// control, else its click control), without anything being clicked.
    /// False: the widget has none.
    bool controlFor(QWidget* pWidget, ConfigKey* pKey) const;

  public slots:
    void startListening();
    void stopListening();

  signals:
    void controlClicked(const ConfigKey& controlKey);

  private:
    QHash<QWidget*, ControlInfo> m_widgetControlInfo;
    bool m_bListening;
};
