#include "spice/SpiceEngine.h"

#include "spice/NgspiceEngine.h"

namespace lvd::spice {

namespace {
std::shared_ptr<SpiceEngine> &replacement()
{
    static std::shared_ptr<SpiceEngine> engine;
    return engine;
}
} // namespace

SpiceEngine &defaultEngine()
{
    if (replacement())
        return *replacement();
    return NgspiceEngine::instance();
}

void setDefaultEngine(std::shared_ptr<SpiceEngine> engine)
{
    replacement() = std::move(engine);
}

} // namespace lvd::spice
