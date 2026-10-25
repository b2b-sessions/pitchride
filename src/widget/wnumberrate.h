//
// C++ Interface: wnumberpos
//
// Description:
//
//
// Author: Tue Haste Andersen <haste@diku.dk>, (C) 2003
//
// Copyright: See COPYING file that comes with this distribution
//
//
#pragma once

#include "widget/wnumber.h"

class ControlProxy;

class WNumberRate final : public WNumber {
    Q_OBJECT
  public:
    explicit WNumberRate(const QString& group, QWidget* parent = nullptr);

    void setup(const QDomNode& node, const SkinContext& context) override;

  private slots:
    void setValue(double dValue) override;

  private:
    ControlProxy* m_pRateRatio;
    // B2B: a skin text with %1 (the signed tempo, "+8.00", "0.00") and %2
    // (the range: "±6", "±10", "±16", "WIDE"), as a CDJ's screen.
    ControlProxy* m_pRateRange;
};
