#include "simulation_ui/OptimizationRunner.h"

#include "core/Document.h"
#include "core/Project.h"
#include "core/Schematic.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QPointer>

#include <cmath>

namespace lvd::sim {

OptimizationRunner::OptimizationRunner(QObject *parent) : QObject(parent) {}

OptimizationRunner::~OptimizationRunner()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

bool OptimizationRunner::start(std::shared_ptr<Project> snapshot, const QString &documentName,
                               const rf::OptimizationSetup &setup)
{
    if (m_running || !snapshot)
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_cancel = false;
    m_progressPosted = false;
    m_running = true;
    {
        const std::lock_guard lock(m_mutex);
        m_progress = Progress{};
        m_progress.maxEvaluations = setup.maxEvaluations;
    }
    QPointer<OptimizationRunner> self(this);
    m_thread = std::thread([this, self, snapshot = std::move(snapshot), documentName, setup]() mutable {
        // The analysis, to re-evaluate the goals of the best point.
        rf::LinearAnalysisSetup analysis;
        bool haveAnalysis = false;
        if (const Document *document = snapshot->document(documentName); document && document->schematic()) {
            for (const rf::LinearAnalysisSetup &candidate : rf::linearAnalyses(document->schematic()->simulation())) {
                if (candidate.id == setup.analysis || (setup.analysis.isEmpty() && !haveAnalysis)) {
                    analysis = candidate;
                    haveAnalysis = true;
                }
            }
        }
        rf::TouchstoneCache touchstones;
        QElapsedTimer clock;
        clock.start();
        qint64 lastPost = -1000;
        double goalsCost = INFINITY;
        const auto progress = [&](const rf::OptimizationProgress &p) {
            {
                const std::lock_guard lock(m_mutex);
                m_progress.evaluation = p.evaluation;
                m_progress.cost = p.cost;
                if (!std::isfinite(m_progress.bestCost) || p.bestCost < m_progress.bestCost) {
                    m_progress.bestCost = p.bestCost;
                    m_progress.history.emplace_back(p.evaluation, p.bestCost);
                }
                m_progress.best = p.best;
            }
            const qint64 now = clock.elapsed();
            if (now - lastPost < 100)
                return;
            lastPost = now;
            if (haveAnalysis && p.bestCost < goalsCost) {
                // One extra simulation per report: the goal status of the best point.
                goalsCost = p.bestCost;
                const rf::LinearResult linear =
                    rf::evaluateDesign(*snapshot, documentName, analysis, setup.variables, p.best, &touchstones);
                QList<rf::GoalEvaluation> goals = rf::evaluateGoals(linear, setup.goals);
                const std::lock_guard lock(m_mutex);
                m_progress.goals = std::move(goals);
            }
            if (!m_progressPosted.exchange(true)) {
                QMetaObject::invokeMethod(
                    this,
                    [self] {
                        if (!self)
                            return;
                        self->m_progressPosted = false;
                        Q_EMIT self->progressed();
                    },
                    Qt::QueuedConnection);
            }
        };
        auto result = std::make_shared<const rf::OptimizationResult>(
            rf::optimize(*snapshot, documentName, setup, progress, &m_cancel));
        {
            const std::lock_guard lock(m_mutex);
            if (!result->goalsBest.isEmpty())
                m_progress.goals = result->goalsBest;
            if (std::isfinite(result->bestCost)) {
                m_progress.bestCost = result->bestCost;
                m_progress.best = result->best;
                m_progress.history.emplace_back(result->evaluations, result->bestCost);
            }
            m_progress.evaluation = result->evaluations;
        }
        // The snapshot goes with the result, to be released on the runner's
        // thread (or with the discarded event when the runner is destroyed).
        QMetaObject::invokeMethod(
            this,
            [self, result, keep = std::move(snapshot)] {
                if (self)
                    self->deliver(result);
            },
            Qt::QueuedConnection);
    });
    return true;
}

void OptimizationRunner::cancel()
{
    if (m_running)
        m_cancel = true;
}

void OptimizationRunner::wait()
{
    if (m_thread.joinable())
        m_thread.join();
    QCoreApplication::sendPostedEvents(this);
}

OptimizationRunner::Progress OptimizationRunner::progress() const
{
    const std::lock_guard lock(m_mutex);
    return m_progress;
}

void OptimizationRunner::deliver(std::shared_ptr<const rf::OptimizationResult> result)
{
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
    Q_EMIT finished(std::move(result));
}

} // namespace lvd::sim
