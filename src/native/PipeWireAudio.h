/*
 * PipeWireAudio.h — native PipeWire audio backend (Linux)
 *
 * A juce::AudioIODeviceType named "PipeWire" that talks to the PipeWire
 * daemon directly via libpipewire-0.3 instead of going through the ALSA or
 * JACK compatibility layers. libpipewire is dlopen'd at runtime (the same
 * strategy JUCE uses for JACK) so the binary carries no hard PipeWire
 * dependency: without the library the factory returns nullptr and the
 * driver simply doesn't exist; without a running daemon the driver exists
 * but enumerates no devices.
 */
#pragma once

#if defined(__linux__) && defined(SUPERSONIC_PIPEWIRE)

#include <juce_audio_devices/juce_audio_devices.h>
#include <memory>

std::unique_ptr<juce::AudioIODeviceType> createPipeWireAudioIODeviceType();

#endif
