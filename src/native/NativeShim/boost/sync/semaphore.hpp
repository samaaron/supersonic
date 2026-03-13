// NativeShim stub — boost::sync::semaphore is not needed for the native JUCE backend.
// The real semaphore is only used by SC_AudioDriver (shared-memory IPC with IDE),
// which we replace entirely with JuceAudioCallback.
#pragma once

namespace boost { namespace sync {
class semaphore {
public:
    explicit semaphore(unsigned int = 0) {}
    void post() {}
    void wait() {}
};
}} // namespace boost::sync
