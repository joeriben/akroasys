#pragma once
// After StandaloneAudioInput.h, which brings JuceHeader.h and the standalone window
// in the order the window header needs.
#include "midi/LaunchControlXLLeds.h"

/*  Keeps the origin of every MIDI message in the standalone.

    Each input is its own port, and the AudioDeviceManager says which one a message
    came from. JUCE's AudioProcessorPlayer drops that and merges everything into one
    buffer. So this sits in front of the player: the Launch Control XL's controllers
    go on marked as the XL's, and an expression pedal on channel 1 is no longer the
    same thing as XL fader 7. What the XL only says about itself, its answer to a mode
    switch and its reports on channel 7, stops here. Nothing else about the stream
    changes.

    The plugin formats do not get this: a host hands a plugin one MIDI stream with no
    origin, so there the processor falls back on the XL's channels.
*/
namespace t5ynth::standalone
{

class MidiSourceRouter final : public juce::MidiInputCallback
{
public:
    explicit MidiSourceRouter(juce::StandalonePluginHolder& holderToUse)
        : holder(holderToUse)
    {
        holder.deviceManager.removeMidiInputDeviceCallback({}, &holder.player);
        holder.deviceManager.addMidiInputDeviceCallback({}, this);
        LaunchControlXLLeds::midiSourcesKnown.store(true, std::memory_order_relaxed);
    }

    ~MidiSourceRouter() override
    {
        holder.deviceManager.removeMidiInputDeviceCallback({}, this);
        holder.deviceManager.addMidiInputDeviceCallback({}, &holder.player);
        LaunchControlXLLeds::midiSourcesKnown.store(false, std::memory_order_relaxed);
    }

    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override
    {
        if (source != nullptr)
        {
            const auto port = source->getName();
            if (LaunchControlXLLeds::isXlPort(port))
            {
                if (LaunchControlXLLeds::inputAnswersDawMode(port)
                    && (LaunchControlXLLeds::isDawModeSwitch(message.getRawData(), message.getRawDataSize(), true)
                        || LaunchControlXLLeds::isDawModeSwitch(message.getRawData(), message.getRawDataSize(), false)))
                    return;   // the device answering a switch; on this port it is never a key

                if (message.isController())
                {
                    if (LaunchControlXLLeds::isDawPort(port)
                        && message.getChannel() == LaunchControlXLLeds::kFeatureChannel
                        && LaunchControlXLLeds::isFeatureReport(message.getControllerNumber()))
                        return;   // the device reporting on itself; no control of anything

                    holder.player.handleIncomingMidiMessage(source, LaunchControlXLLeds::markAsXl(message));
                    return;
                }
            }
        }
        holder.player.handleIncomingMidiMessage(source, message);
    }

private:
    juce::StandalonePluginHolder& holder;
};

} // namespace t5ynth::standalone
