#pragma once

#include "core/ComponentDefinition.h"

#include <QList>
#include <QString>
#include <QWidget>

namespace lvd {

// Shows a symbol scaled to fit, optionally in several orientations side by
// side with a caption under each (to check pin directions at a glance).
class SymbolPreview : public QWidget
{
    Q_OBJECT
public:
    explicit SymbolPreview(QWidget *parent = nullptr);

    struct View {
        Orientation orientation;
        QString caption;
    };

    void setSymbol(const Symbol &symbol);
    void clear();
    bool hasSymbol() const { return m_hasSymbol; }

    // Default: one view in the normal orientation without caption.
    void setViews(const QList<View> &views);
    void setShowPinNames(bool show);
    // Upper limit of the drawing scale in pixels per unit, so small symbols do not become huge.
    void setMaximumScale(double scale);
    void setPlaceholderText(const QString &text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Symbol m_symbol;
    bool m_hasSymbol = false;
    QList<View> m_views;
    bool m_showPinNames = true;
    double m_maximumScale = 4;
    QString m_placeholder;
};

} // namespace lvd
