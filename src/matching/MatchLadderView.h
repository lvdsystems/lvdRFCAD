#pragma once

#include "matching/MatchingSynthesis.h"

#include <QWidget>

#include <optional>

namespace lvd {

// Schematic-like drawing of a matching ladder between a source and a load.
class MatchLadderView : public QWidget
{
public:
    explicit MatchLadderView(QWidget *parent = nullptr);

    void setSolution(const std::optional<rf::MatchSolution> &solution);
    const std::optional<rf::MatchSolution> &solution() const { return m_solution; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::optional<rf::MatchSolution> m_solution;
};

// "12 - j30 Ω"
QString formatImpedance(std::complex<double> z, int digits = 4);

} // namespace lvd
