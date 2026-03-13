/*
 * test_main.cpp — Custom Catch2 main with JUCE initialisation.
 *
 * JUCE requires COM init on Windows (ScopedJuceInitialiser_GUI)
 * before any AudioDeviceManager or Thread operations.
 */
#include <catch2/catch_session.hpp>
#include <juce_events/juce_events.h>

int main(int argc, char* argv[]) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    return Catch::Session().run(argc, argv);
}
