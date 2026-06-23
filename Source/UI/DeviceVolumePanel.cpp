#include "UI/DeviceVolumePanel.h"

#include <cmath>

namespace dcr
{

    // Poll the OS this often so strips track external volume changes (media keys /
    // menu-bar slider / Audio MIDI Setup).  Each tick reads only the strips that
    // actually have a controllable property, so dead devices cost no IPC.
    static constexpr int kPollHz = 8;
    static constexpr int kStripWidth = 84;
    static constexpr int kStripGap = 6;

    //==============================================================================
    //  Strip
    //==============================================================================
    DeviceVolumePanel::Strip::Strip (const juce::String& deviceName, DeviceVolume::Scope scope)
        : vol (deviceName, scope)
    {
        nameLabel.setText (deviceName, juce::dontSendNotification);
        nameLabel.setJustificationType (juce::Justification::centredTop);
        nameLabel.setMinimumHorizontalScale (0.6f);
        nameLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        nameLabel.setTooltip (deviceName);
        addAndMakeVisible (nameLabel);

        fader.setRange (0.0, 1.0, 0.0);
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 52, 18);
        fader.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
        fader.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() / 100.0; };
        fader.onValueChange = [this] { if (vol.hasVolume()) vol.setVolume ((float) fader.getValue()); };
        fader.onDragStart = [this] { dragging = true; };
        fader.onDragEnd = [this] { dragging = false; if (vol.hasVolume()) vol.setVolume ((float) fader.getValue()); };
        addAndMakeVisible (fader);

        // dB readout under the % box -- the actual device gain, matching Audio MIDI
        // Setup (read from the OS, not derived from the slider).
        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        dbLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (150, 200, 210));
        dbLabel.setTooltip ("Device gain in dB (matches Audio MIDI Setup)");
        addAndMakeVisible (dbLabel);

        mute.setClickingTogglesState (true);
        mute.setTooltip ("Mute this device");
        mute.onClick = [this] {
            if (vol.hasMute())
            {
                vol.setMute (mute.getToggleState());
                lastMuted = mute.getToggleState();
            }
        };
        addAndMakeVisible (mute);

        naLabel.setText ("N/A", juce::dontSendNotification);
        naLabel.setJustificationType (juce::Justification::centred);
        naLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (140, 140, 145));
        naLabel.setTooltip ("macOS exposes no volume control for this device");
        addChildComponent (naLabel);

        // Gold ★ in the top-left corner marking the current system default device on
        // this direction.  Hidden until the panel calls setIsDefault(true); purely
        // indicative, so it never intercepts clicks.
        starLabel.setText (juce::String::fromUTF8 ("\xe2\x98\x85"), juce::dontSendNotification);
        starLabel.setJustificationType (juce::Justification::centred);
        starLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (240, 200, 90));
        starLabel.setFont (juce::Font (juce::FontOptions (14.0f)));
        starLabel.setInterceptsMouseClicks (false, false);
        starLabel.setTooltip (scope == DeviceVolume::Scope::Output ? "System default output device"
                                                                   : "System default input device");
        addChildComponent (starLabel); // hidden until setIsDefault(true)

        applyEnabledLook();
        pull();
    }

    void DeviceVolumePanel::Strip::applyEnabledLook()
    {
        const bool v = vol.hasVolume();
        const bool m = vol.hasMute();
        fader.setEnabled (v);
        fader.setAlpha (v ? 1.0f : 0.35f);
        // Hide the % readout when there's no controllable volume -- the "N/A"
        // overlay is the signal, and a never-set slider would otherwise show a raw
        // "0.0000" that reads as a real value.
        fader.setTextBoxStyle (v ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox, false, 52, 18);
        mute.setEnabled (m);
        mute.setAlpha (m ? 1.0f : 0.35f);
        naLabel.setVisible (!v);
        dbLabel.setVisible (v && vol.hasDb());
    }

    void DeviceVolumePanel::Strip::setIsDefault (bool isDefault)
    {
        starLabel.setVisible (isDefault);
    }

    void DeviceVolumePanel::Strip::pull()
    {
        // Don't fight an active drag.
        if (vol.hasVolume() && !dragging)
        {
            const double g = (double) vol.getVolume();
            if (std::abs (g - fader.getValue()) > 1.0e-4)
                fader.setValue (g, juce::dontSendNotification);
        }
        if (vol.hasMute())
        {
            const bool m = vol.getMute();
            if (m != lastMuted)
            {
                lastMuted = m;
                mute.setToggleState (m, juce::dontSendNotification);
            }
        }
        if (vol.hasDb())
        {
            auto t = juce::String (vol.getVolumeDb(), 2) + " dB";
            if (t != dbLabel.getText())
                dbLabel.setText (t, juce::dontSendNotification);
        }
    }

    void DeviceVolumePanel::Strip::resized()
    {
        auto r = getLocalBounds().reduced (4);
        starLabel.setBounds (r.getX(), r.getY(), 16, 16); // top-left corner badge
        nameLabel.setBounds (r.removeFromTop (30));
        mute.setBounds (r.removeFromBottom (22).reduced (10, 0));
        r.removeFromBottom (2);
        dbLabel.setBounds (r.removeFromBottom (15));
        r.removeFromBottom (2);
        fader.setBounds (r);
        naLabel.setBounds (r); // overlay centred on the (disabled) fader area
    }

    //==============================================================================
    //  Panel
    //==============================================================================
    DeviceVolumePanel::DeviceVolumePanel (AudioEngine& eng, Direction dir)
        : engine (eng), direction (dir)
    {
        title.setText (dir == Direction::Inputs ? "INPUT DEVICES" : "OUTPUT DEVICES",
            juce::dontSendNotification);
        title.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
            13.0f,
            juce::Font::bold)));
        title.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
        addAndMakeVisible (title);

        // System-default device picker for this direction: lists ALL devices on this
        // direction (not just the routed strips below) and sets the macOS default
        // when changed -- a stand-in for System Settings > Sound.  Independent of
        // D-Router's own routing.
        defaultLabel.setText (dir == Direction::Inputs ? "System Input" : "System Output",
            juce::dontSendNotification);
        defaultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        defaultLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
        addAndMakeVisible (defaultLabel);

        defaultCombo.setTextWhenNothingSelected (juce::String::fromUTF8 ("\xe2\x80\x94")); // em dash
        defaultCombo.setTextWhenNoChoicesAvailable ("No devices");
        defaultCombo.setTooltip (dir == Direction::Inputs
                                     ? "Set the macOS default input device (System Settings > Sound > Input)"
                                     : "Set the macOS default output device (System Settings > Sound > Output)");
        defaultCombo.onChange = [this] { applyDefaultSelection(); };
        addAndMakeVisible (defaultCombo);

        viewport.setViewedComponent (&stripsHolder, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);

        emptyLabel.setJustificationType (juce::Justification::centred);
        emptyLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (130, 130, 135));
        emptyLabel.setText (dir == Direction::Inputs ? "No input devices in routing."
                                                     : "No output devices in routing.",
            juce::dontSendNotification);
        addChildComponent (emptyLabel);

        rebuild();
        startTimerHz (kPollHz);
    }

    SystemAudioDevices::Scope DeviceVolumePanel::defaultScope() const noexcept
    {
        return direction == Direction::Inputs ? SystemAudioDevices::Scope::Input
                                              : SystemAudioDevices::Scope::Output;
    }

    void DeviceVolumePanel::rebuild()
    {
        strips.clear(); // OwnedArray deletes the child Strips

        for (const auto& di : engine.getDeviceInfo())
        {
            const bool wantThis = direction == Direction::Inputs ? (di.numInputChannels > 0)
                                                                 : (di.numOutputChannels > 0);
            if (!wantThis)
                continue;
            // App-audio capture sources have no hardware volume control -- they live
            // in IN/OUT GROUPS (Soft-In), not the Audio Setup volume panel.
            if (di.isAppInput)
                continue;

            auto scope = direction == Direction::Inputs ? DeviceVolume::Scope::Input
                                                        : DeviceVolume::Scope::Output;
            auto* s = new Strip (di.name, scope);
            stripsHolder.addAndMakeVisible (s);
            strips.add (s);
        }

        emptyLabel.setVisible (strips.isEmpty());

        // (Re)populate the system-default picker with ALL devices on this direction.
        // Done only here (tab open / device change) -- the timer never rebuilds the
        // list, only the selection + ★.
        defaultDevices = SystemAudioDevices::list (defaultScope());
        defaultCombo.clear (juce::dontSendNotification);
        for (int i = 0; i < defaultDevices.size(); ++i)
            defaultCombo.addItem (defaultDevices.getReference (i).name, i + 1); // item-id 0 is reserved
        syncDefaultToOS();

        resized();
    }

    void DeviceVolumePanel::syncDefaultToOS()
    {
        const auto def = SystemAudioDevices::getDefault (defaultScope());

        // Reflect the OS default in the combo (matched by AudioDeviceID, not name, so
        // same-named devices don't collide).  Don't fight an open popup.
        if (!defaultCombo.isPopupActive())
        {
            int wantId = 0; // 0 == nothing selected
            if (def.deviceID != 0)
                for (int i = 0; i < defaultDevices.size(); ++i)
                    if (defaultDevices.getReference (i).deviceID == def.deviceID)
                    {
                        wantId = i + 1;
                        break;
                    }
            if (defaultCombo.getSelectedId() != wantId)
                defaultCombo.setSelectedId (wantId, juce::dontSendNotification);
        }

        // Light the ★ on whichever routed strip is the default device (by name --
        // consistent with D-Router's name-based device model; if the default isn't a
        // routed device, no strip lights and the combo alone shows it).
        for (auto* s : strips)
            s->setIsDefault (def.deviceID != 0 && s->vol.getDeviceName() == def.name);
    }

    void DeviceVolumePanel::applyDefaultSelection()
    {
        const int id = defaultCombo.getSelectedId();
        if (id <= 0 || id > defaultDevices.size())
            return; // programmatic clear / nothing selected

        const auto& dev = defaultDevices.getReference (id - 1);
        // setDefault failing (device just vanished?) leaves syncDefaultToOS to snap
        // the selection + ★ back to the OS's actual default on the next pass.
        SystemAudioDevices::setDefault (defaultScope(), dev.deviceID);
        syncDefaultToOS();
    }

    void DeviceVolumePanel::resumeUpdates() { startTimerHz (kPollHz); }

    void DeviceVolumePanel::timerCallback()
    {
        for (auto* s : strips)
            s->pull();
        syncDefaultToOS(); // track external default-device changes + keep ★ correct
    }

    void DeviceVolumePanel::paint (juce::Graphics& g)
    {
        g.setColour (juce::Colour::fromRGB (28, 30, 34));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    }

    void DeviceVolumePanel::resized()
    {
        auto r = getLocalBounds().reduced (8);
        title.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);

        // System-default device picker row: label on the left, combo filling the rest.
        auto comboRow = r.removeFromTop (24);
        defaultLabel.setBounds (comboRow.removeFromLeft (90));
        comboRow.removeFromLeft (4);
        defaultCombo.setBounds (comboRow.removeFromLeft (juce::jmin (260, comboRow.getWidth())));
        r.removeFromTop (6);

        emptyLabel.setBounds (r);
        viewport.setBounds (r);

        const int h = viewport.getMaximumVisibleHeight();
        int x = 0;
        for (auto* s : strips)
        {
            s->setBounds (x, 0, kStripWidth, h);
            x += kStripWidth + kStripGap;
        }
        stripsHolder.setSize (juce::jmax (x, r.getWidth()), h);
    }

} // namespace dcr
