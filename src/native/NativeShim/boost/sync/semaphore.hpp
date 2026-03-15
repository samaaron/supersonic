// NativeShim stub — boost::sync::semaphore is not needed for the native JUCE backend.
// The real semaphore is used by SC_HiddenWorld::mQuitProgram for quit synchronization.
// Not needed in the native JUCE backend.
#pragma once

namespace boost { namespace sync {
class semaphore {
public:
    explicit semaphore(unsigned int = 0) {}
    void post() {}
    void wait() {}
};
}} // namespace boost::sync
