#include "simulation_ui/BudgetRunner.h"

#include <QCoreApplication>
#include <QPointer>

namespace lvd::sim {

BudgetRunner::BudgetRunner(QObject *parent) : QObject(parent) {}

BudgetRunner::~BudgetRunner()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

bool BudgetRunner::start(Netlist netlist, rf::BudgetAnalysisSetup setup, Expression::Lookup scope,
                         QString projectDirectory, quint64 tag)
{
    if (m_running)
        return false;
    if (m_thread.joinable())
        m_thread.join();
    m_cancel = false;
    m_running = true;
    QPointer<BudgetRunner> self(this);
    rf::TouchstoneCache *touchstones = &m_touchstones;
    const std::atomic_bool *cancel = &m_cancel;
    m_thread = std::thread([self, netlist = std::move(netlist), setup = std::move(setup), scope = std::move(scope),
                            projectDirectory = std::move(projectDirectory), touchstones, cancel, tag] {
        // Built in place: the chain's stages point into its own netlist, so
        // the run must not be copied.
        auto run = std::make_shared<rf::BudgetRun>(rf::runBudget(netlist, setup, scope, projectDirectory, touchstones, cancel));
        if (cancel->load())
            run->cancelled = true;
        std::shared_ptr<const rf::BudgetRun> result = std::move(run);
        // The destructor joins this thread before `self` goes away; the
        // queued call runs in the runner's thread, where the check is meaningful.
        if (BudgetRunner *runner = self.data()) {
            QMetaObject::invokeMethod(
                runner, [self, result, tag] {
                    if (self)
                        self->deliver(result, tag);
                },
                Qt::QueuedConnection);
        }
    });
    return true;
}

void BudgetRunner::cancel()
{
    if (m_running)
        m_cancel = true;
}

void BudgetRunner::wait()
{
    if (m_thread.joinable())
        m_thread.join();
    QCoreApplication::sendPostedEvents(this);
}

void BudgetRunner::deliver(std::shared_ptr<const rf::BudgetRun> run, quint64 tag)
{
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
    Q_EMIT finished(std::move(run), tag);
}

} // namespace lvd::sim
