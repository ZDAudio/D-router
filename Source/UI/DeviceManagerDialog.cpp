#include "UI/DeviceManagerDialog.h"

namespace dcr {

DeviceManagerDialog::DeviceManagerDialog (AudioEngine& e,
                                          const std::vector<AudioEngine::DeviceSpec>& current)
    : engine (e)
{
    title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    addAndMakeVisible (title);

    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&rowsHolder, false);
    viewport.setScrollBarsShown (true, false);

    auto ins  = engine.getAvailableInputDevices();
    auto outs = engine.getAvailableOutputDevices();
    juce::StringArray allNames;
    for (auto& n : ins)  allNames.addIfNotAlreadyThere (n);
    for (auto& n : outs) allNames.addIfNotAlreadyThere (n);
    // Alphabetical sort (case-insensitive) so the device list is predictable
    // regardless of CoreAudio's discovery order.
    allNames.sortNatural();

    for (auto& name : allNames)
    {
        auto* row = new Row();
        row->name = name;
        row->hasInput  = ins .contains (name);
        row->hasOutput = outs.contains (name);

        row->nameLabel.setText (name, juce::dontSendNotification);
        row->nameLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

        row->inputBtn .setButtonText ("In");
        row->outputBtn.setButtonText ("Out");
        row->inputBtn .setEnabled (row->hasInput);
        row->outputBtn.setEnabled (row->hasOutput);

        // Pre-tick based on current selection.
        for (auto& sp : current)
        {
            if (sp.name == name)
            {
                if (sp.wantInput  && row->hasInput)  row->inputBtn .setToggleState (true, juce::dontSendNotification);
                if (sp.wantOutput && row->hasOutput) row->outputBtn.setToggleState (true, juce::dontSendNotification);
            }
        }

        rowsHolder.addAndMakeVisible (row->nameLabel);
        rowsHolder.addAndMakeVisible (row->inputBtn);
        rowsHolder.addAndMakeVisible (row->outputBtn);
        rows.add (row);
    }

    okButton.onClick = [this]
    {
        std::vector<AudioEngine::DeviceSpec> sel;
        for (int i = 0; i < rows.size(); ++i)
        {
            auto* r = rows[i];
            const bool wIn  = r->inputBtn .getToggleState() && r->hasInput;
            const bool wOut = r->outputBtn.getToggleState() && r->hasOutput;
            if (wIn || wOut) sel.push_back ({ r->name, wIn, wOut });
        }
        // Close the dialog first; defer the (possibly slow) callback so the
        // window can finish its dismissal animation immediately.
        auto cb = onClose;
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (1);
        if (cb)
            juce::MessageManager::callAsync ([cb, sel] { cb (sel); });
    };
    cancelButton.onClick = [this]
    {
        auto cb = onClose;
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
        if (cb)
            juce::MessageManager::callAsync ([cb] { cb (std::nullopt); });
    };
    addAndMakeVisible (okButton);
    addAndMakeVisible (cancelButton);

    setSize (520, 460);
}

void DeviceManagerDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (32, 32, 36));
}

void DeviceManagerDialog::resized()
{
    auto r = getLocalBounds().reduced (12);
    title.setBounds (r.removeFromTop (28));
    r.removeFromTop (8);

    auto bottom = r.removeFromBottom (36);
    cancelButton.setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (8);
    okButton.setBounds (bottom.removeFromRight (90));
    r.removeFromBottom (8);

    viewport.setBounds (r);

    const int rowH = 30;
    rowsHolder.setSize (r.getWidth() - 16, rowH * rows.size() + 4);

    for (int i = 0; i < rows.size(); ++i)
    {
        auto* row = rows[i];
        const int y = i * rowH + 2;
        const int w = rowsHolder.getWidth();
        row->nameLabel.setBounds (4, y, w - 130, rowH - 4);
        row->inputBtn .setBounds (w - 122, y + 4, 56, rowH - 8);
        row->outputBtn.setBounds (w - 60,  y + 4, 56, rowH - 8);
    }
}

void DeviceManagerDialog::launch (AudioEngine& engine,
                                  const std::vector<AudioEngine::DeviceSpec>& current,
                                  std::function<void (std::optional<std::vector<AudioEngine::DeviceSpec>>)> callback)
{
    auto* content = new DeviceManagerDialog (engine, current);
    content->onClose = std::move (callback);

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle                  = "Devices";
    o.content.setOwned             (content);
    o.componentToCentreAround      = nullptr;
    o.dialogBackgroundColour       = juce::Colour::fromRGB (32, 32, 36);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar            = true;
    o.resizable                    = true;
    o.launchAsync();
}

} // namespace dcr
