/*  The standalone's own JUCEApplication, replacing JUCE's default one.

    Built only into the Standalone target, which defines
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1 (CMakeLists.txt) and then expects
    juce_CreateApplication() from us instead of instantiating its own
    StandaloneFilterApp -- see
    JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_Standalone.cpp.

    It exists for TWO differences from JUCE's default: the app must not open an
    audio INPUT device while launching, and MIDI keeps its origin on the way to
    the processor (StandaloneMidiSources.h). Everything else here is JUCE's own
    default app, kept deliberately identical so this file never becomes a
    second, drifting implementation of the standalone's lifecycle.

    The two lines that make the difference for the audio input:
      * the channel constraint {0 in, 2 out}, which is what
        StandalonePluginHolder reads to decide whether an input is required at
        all (it does NOT rewrite the processor's bus layout -- the stereo input
        bus stays, so Resynth's capture path and the plugin builds are
        untouched), and
      * parkStoredInputDevice(), because a stored input device name is restored
        by AudioDeviceManager::initialiseFromXML no matter how many input
        channels were requested.

    Both are undone on demand by openParkedInputDevice() when Resynth ext
    engages. The reasoning, and the measurement behind it, is in
    StandaloneAudioInput.h.
*/
// StandaloneAudioInput.h pulls in JuceHeader.h first and only then the
// standalone window header — that order matters: juce_StandaloneFilterWindow.h
// opens `namespace juce` and uses unqualified JUCE names, so it does not
// compile as a first include.
#include "StandaloneAudioInput.h"
#include "StandaloneMidiSources.h"

namespace t5ynth
{

class StandaloneApp final : public juce::JUCEApplication
{
public:
    StandaloneApp()
    {
        juce::PropertiesFile::Options options;
        options.applicationName     = juce::CharPointer_UTF8(JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
       #if JUCE_LINUX || JUCE_BSD
        options.folderName          = "~/.config";
       #else
        options.folderName          = "";
       #endif

        appProperties.setStorageParameters(options);
    }

    const juce::String getApplicationName() override           { return juce::CharPointer_UTF8(JucePlugin_Name); }
    const juce::String getApplicationVersion() override        { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override                 { return true; }
    void anotherInstanceStarted(const juce::String&) override  {}

    void initialise(const juce::String&) override
    {
        if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            jassertfalse;   // no display, so no window can be created
            return;
        }

        auto* settings = appProperties.getUserSettings();

        // Before the holder is built: the stored input device is taken out of
        // the setup JUCE is about to restore, and remembered. Without this the
        // saved audioInputDeviceName is reopened at launch even though zero
        // input channels are requested below.
        if (settings != nullptr)
            standalone::parkStoredInputDevice(*settings);

        // {0 in, 2 out}: no input device is opened while the app starts up.
        const juce::Array<juce::StandalonePluginHolder::PluginInOuts> outputOnly {
            juce::StandalonePluginHolder::PluginInOuts { 0, 2 } };

        auto holder = std::make_unique<juce::StandalonePluginHolder>(
            settings,
            false,                       // the PropertiesFile stays ours
            juce::String{},              // no preferred device name
            nullptr,                     // no preferred setup override
            outputOnly,
            false);                      // desktop: no MIDI auto-open, as JUCE's default

        midiSources = std::make_unique<standalone::MidiSourceRouter>(*holder);

        mainWindow = std::make_unique<juce::StandaloneFilterWindow>(
            getApplicationName(),
            juce::LookAndFeel::getDefaultLookAndFeel()
                .findColour(juce::ResizableWindow::backgroundColourId),
            std::move(holder));

        mainWindow->setVisible(true);
    }

    void shutdown() override
    {
        midiSources = nullptr;   // it points into the holder the window owns
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            juce::Timer::callAfterDelay(100, []
            {
                if (auto app = JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
        {
            quit();
        }
    }

private:
    juce::ApplicationProperties appProperties;
    std::unique_ptr<standalone::MidiSourceRouter> midiSources;
    std::unique_ptr<juce::StandaloneFilterWindow> mainWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StandaloneApp)
};

} // namespace t5ynth

juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new t5ynth::StandaloneApp();
}
