#include "UI/DeviceVolumePanel.h"

#include <cmath>

namespace dcr {

// Poll the OS this often so strips track external volume changes (media keys /
// menu-bar slider / Audio MIDI Setup).  Each tick reads only the strips that
// actually have a controllable property, so dead devices cost no IPC.
static constexpr int kPollHz     = 8;
static constexpr int kStripWidth = 84;
static constexpr int kStripGap   = 6;

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
    fader.onDragStart   = [this] { dragging = true; };
    fader.onDragEnd     = [this] { dragging = false; if (vol.hasVolume()) vol.setVolume ((float) fader.getValue()); };
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
    mute.onClick = [this]
    {
        if (vol.hasMute()) { vol.setMute (mute.getToggleState()); lastMuted = mute.getToggleState(); }
    };
    addAndMakeVisible (mute);

    naLabel.setText ("N/A", juce::dontSendNotification);
    naLabel.setJustificationType (juce::Justification::centred);
    naLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (140, 140, 145));
    naLabel.setTooltip ("macOS exposes no volume control for this device");
    addChildComponent (naLabel);

    applyEnabledLook();
    pull();
}

void DeviceVolumePanel::Strip::applyEnabledLook()
{
    const bool v = vol.hasVolume();
    const bool m = vol.hasMute();
    fader.setEnabled (v);
    fader.setAlpha   (v ? 1.0f : 0.35f);
    // Hide the % readout when there's no controllable volume -- the "N/A"
    // overlay is the signal, and a never-set slider would otherwise show a raw
    // "0.0000" that reads as a real value.
    fader.setTextBoxStyle (v ? juce::Slider::TextBoxBelow : juce::Slider::NoTextBox, false, 52, 18);
    mute .setEnabled (m);
    mute .setAlpha   (m ? 1.0f : 0.35f);
    naLabel.setVisible (! v);
    dbLabel.setVisible (v && vol.hasDb());
}

void DeviceVolumePanel::Strip::pull()
{
    // Don't fight an active drag.
    if (vol.hasVolume() && ! dragging)
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
    nameLabel.setBounds (r.removeFromTop (30));
    mute.setBounds (r.removeFromBottom (22).reduced (10, 0));
    r.removeFromBottom (2);
    dbLabel.setBounds (r.removeFromBottom (15));
    r.removeFromBottom (2);
    fader.setBounds (r);
    naLabel.setBounds (r);   // overlay centred on the (disabled) fader area
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
                                                  13.0f, juce::Font::bold)));
    title.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
    addAndMakeVisible (title);

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

void DeviceVolumePanel::rebuild()
{
    strips.clear();   // OwnedArray deletes the child Strips

    for (const auto& di : engine.getDeviceInfo())
    {
        const bool wantThis = direction == Direction::Inputs ? (di.numInputChannels  > 0)
                                                             : (di.numOutputChannels > 0);
        if (! wantThis) continue;

        auto scope = direction == Direction::Inputs ? DeviceVolume::Scope::Input
                                                     : DeviceVolume::Scope::Output;
        auto* s = new Strip (di.name, scope);
        stripsHolder.addAndMakeVisible (s);
        strips.add (s);
    }

    emptyLabel.setVisible (strips.isEmpty());
    resized();
}

void DeviceVolumePanel::resumeUpdates() { startTimerHz (kPollHz); }

void DeviceVolumePanel::timerCallback()
{
    for (auto* s : strips) s->pull();
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
