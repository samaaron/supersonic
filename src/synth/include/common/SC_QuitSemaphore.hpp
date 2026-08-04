// Quit-synchronisation semaphore for SC_HiddenWorld::mQuitProgram.
// Deliberately a no-op on every SuperSonic target: the embedding engine
// owns process lifecycle, and World_WaitForQuit is never the blocking
// path (this replaces the identical no-op boost::sync::semaphore shim).
#pragma once

namespace sc { namespace sync {
class semaphore {
public:
    explicit semaphore(unsigned int = 0) {}
    void post() {}
    void wait() {}
};
}} // namespace sc::sync
