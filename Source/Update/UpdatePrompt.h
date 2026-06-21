#pragma once

#include "Update/UpdateChecker.h" // ReleaseInfo
#include "Update/UpdateInstaller.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::update
{

    // Non-blocking "a new version is available" dialog content.  Shown via
    // juce::DialogWindow::LaunchOptions::launchAsync (see MainComponent::showAboutDialog
    // for the pattern).  Ignoring it leaves the running version untouched.  On Upgrade
    // it owns + drives an UpdateInstaller, swapping the buttons for a progress bar.
    class UpdatePrompt : public juce::Component
    {
    public:
        UpdatePrompt (ReleaseInfo info, juce::String currentVersion)
            : release (std::move (info))
        {
            title.setText ("A new version of D-Router is available", juce::dontSendNotification);
            title.setFont (juce::FontOptions (18.0f, juce::Font::bold));
            title.setColour (juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible (title);

            versions.setText ("Current " + currentVersion + "    →    New " + release.tag,
                juce::dontSendNotification);
            versions.setColour (juce::Label::textColourId, juce::Colour::fromRGB (0, 255, 210));
            addAndMakeVisible (versions);

            notes.setMultiLine (true, true);
            notes.setReadOnly (true);
            notes.setScrollbarsShown (true);
            notes.setCaretVisible (false);
            notes.setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (28, 28, 34));
            notes.setColour (juce::TextEditor::textColourId, juce::Colour::fromRGB (200, 200, 206));
            notes.setColour (juce::TextEditor::outlineColourId, juce::Colour::fromRGB (60, 60, 68));
            notes.setText (release.notes.isNotEmpty() ? release.notes : release.name, false);
            addAndMakeVisible (notes);

            status.setColour (juce::Label::textColourId, juce::Colour::fromRGB (170, 170, 178));
            status.setJustificationType (juce::Justification::centredLeft);
            addChildComponent (status);

            progressBar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour::fromRGB (40, 40, 48));
            progressBar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour::fromRGB (0, 255, 210));
            addChildComponent (progressBar);

            upgradeBtn.setButtonText ("Upgrade");
            upgradeBtn.onClick = [this] { onUpgradeClicked(); };
            addAndMakeVisible (upgradeBtn);

            laterBtn.setButtonText ("Later");
            laterBtn.onClick = [this] { closeDialog(); };
            addAndMakeVisible (laterBtn);

            setSize (480, 360);
        }

        void paint (juce::Graphics& g) override { g.fillAll (juce::Colour::fromRGB (18, 18, 22)); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (20);
            title.setBounds (r.removeFromTop (30));
            versions.setBounds (r.removeFromTop (22));
            r.removeFromTop (8);

            auto buttons = r.removeFromBottom (34);
            laterBtn.setBounds (buttons.removeFromRight (100));
            buttons.removeFromRight (10);
            upgradeBtn.setBounds (buttons.removeFromRight (160));

            r.removeFromBottom (8);
            status.setBounds (r.removeFromBottom (22));
            progressBar.setBounds (r.removeFromBottom (18));
            r.removeFromBottom (8);
            notes.setBounds (r);
        }

    private:
        void onUpgradeClicked()
        {
            juce::String reason;
            if (!UpdateInstaller::canInstallInPlace (reason))
            {
                // Can't replace in place (App Translocation / read-only folder).
                // Degrade to opening the download page; keep the running app intact.
                status.setText (reason, juce::dontSendNotification);
                status.setVisible (true);
                upgradeBtn.setButtonText ("Open Download Page");
                upgradeBtn.onClick = [] { UpdateInstaller::releasesPageUrl().launchInDefaultBrowser(); };
                resized();
                return;
            }

            upgradeBtn.setEnabled (false);
            laterBtn.setButtonText ("Cancel");
            laterBtn.onClick = [this] { installer.cancel(); closeDialog(); };

            status.setText ("Downloading…", juce::dontSendNotification);
            status.setVisible (true);
            progressValue = 0.0;
            progressBar.setVisible (true);
            resized();

            juce::Component::SafePointer<UpdatePrompt> safe (this);
            installer.start (release, [safe] (double p) { if (safe != nullptr) safe->onProgress (p); }, [safe] (bool ok, juce::String e) { if (safe != nullptr) safe->onDone (ok, e); });
        }

        void onProgress (double p) { progressValue = p; } // ProgressBar polls the bound value

        void onDone (bool ok, juce::String error)
        {
            if (ok)
            {
                // Success: the installer is quitting the app and the swap script will
                // relaunch the new version.  Leave a final note in case there's a beat.
                progressBar.setVisible (false);
                status.setText ("Restarting D-Router…", juce::dontSendNotification);
                return;
            }

            if (error.isEmpty())
                return; // user cancelled -> dialog already closing

            progressBar.setVisible (false);
            status.setText (error, juce::dontSendNotification);
            upgradeBtn.setEnabled (true);
            laterBtn.setButtonText ("Close");
            laterBtn.onClick = [this] { closeDialog(); };
            resized();
        }

        void closeDialog()
        {
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        }

        ReleaseInfo release;
        juce::Label title, versions, status;
        juce::TextEditor notes;
        double progressValue = 0.0;
        juce::ProgressBar progressBar { progressValue };
        juce::TextButton upgradeBtn, laterBtn;
        UpdateInstaller installer;
    };

} // namespace dcr::update
