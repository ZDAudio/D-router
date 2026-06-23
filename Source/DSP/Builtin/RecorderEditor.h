#pragma once

#include "DSP/Builtin/RecorderProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{
    // Record/Stop + elapsed time + file size + input meter, with format / bit-depth
    // combos (APVTS-attached), an editable Name (filename prefix), and folder
    // choose / reveal.  A 15 Hz timer polls the processor's atomics.
    class RecorderEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
    {
    public:
        explicit RecorderEditor (RecorderProcessor& p)
            : juce::AudioProcessorEditor (p), rec (p)
        {
            using SA = juce::AudioProcessorValueTreeState;

            recordButton.onClick = [this] { toggleRecord(); };
            addAndMakeVisible (recordButton);

            timeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
            timeLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
            addAndMakeVisible (timeLabel);
            sizeLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (150, 150, 160));
            addAndMakeVisible (sizeLabel);

            formatBox.addItemList ({ "WAV", "FLAC", "AAC (.m4a)" }, 1);
            addAndMakeVisible (formatBox);
            formatAtt = std::make_unique<SA::ComboBoxAttachment> (rec.getValueTreeState(), "format", formatBox);

            bitsBox.addItemList ({ "16-bit", "24-bit", "32-bit float" }, 1);
            addAndMakeVisible (bitsBox);
            bitsAtt = std::make_unique<SA::ComboBoxAttachment> (rec.getValueTreeState(), "bits", bitsBox);

            nameField.setText (rec.namePrefix(), juce::dontSendNotification);
            nameField.onTextChange = [this] { rec.setNamePrefix (nameField.getText()); };
            addAndMakeVisible (nameField);

            folderButton.setButtonText ("Folder...");
            folderButton.onClick = [this] { chooseFolder(); };
            addAndMakeVisible (folderButton);

            revealButton.setButtonText ("Reveal");
            // Create the folder first so Reveal always opens something -- the
            // default folder may not exist until the first recording lands.
            revealButton.onClick = [this] {
                auto f = rec.outputFolder();
                f.createDirectory();
                f.revealToUser();
            };
            addAndMakeVisible (revealButton);

            folderLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (140, 140, 150));
            folderLabel.setFont (juce::FontOptions (11.0f));
            addAndMakeVisible (folderLabel);

            // Bit depth only applies to WAV (FLAC clamps to 16/24, AAC ignores
            // it), so re-evaluate the bits combo whenever the format changes.
            // Hooked after all controls exist so the initial refresh() below sets
            // the correct state.
            formatBox.onChange = [this] { refresh(); };

            setSize (380, 250);
            startTimerHz (15);
            refresh();
        }

        ~RecorderEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            recordButton.setBounds (r.removeFromTop (44));
            r.removeFromTop (6);
            meterArea = r.removeFromTop (14);
            r.removeFromTop (6);
            timeLabel.setBounds (r.removeFromTop (24));
            sizeLabel.setBounds (r.removeFromTop (18));
            r.removeFromTop (10);
            auto fmtRow = r.removeFromTop (24);
            formatBox.setBounds (fmtRow.removeFromLeft (160));
            fmtRow.removeFromLeft (8);
            bitsBox.setBounds (fmtRow);
            r.removeFromTop (6);
            nameField.setBounds (r.removeFromTop (24));
            r.removeFromTop (6);
            auto folderRow = r.removeFromTop (24);
            folderButton.setBounds (folderRow.removeFromLeft (90));
            folderRow.removeFromLeft (6);
            revealButton.setBounds (folderRow.removeFromLeft (80));
            r.removeFromTop (4);
            folderLabel.setBounds (r.removeFromTop (16));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (16, 16, 20));
            g.setColour (juce::Colour::fromRGB (30, 30, 36));
            g.fillRect (meterArea);
            const float db = juce::Decibels::gainToDecibels (rec.inputPeak(), -60.0f);
            const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            auto fill = meterArea.toFloat().withWidth ((float) meterArea.getWidth() * frac);
            g.setColour (db > -1.0f ? juce::Colour::fromRGB (255, 80, 60)
                                    : juce::Colour::fromRGB (0, 200, 120));
            g.fillRect (fill);
        }

    private:
        void toggleRecord()
        {
            if (rec.isRecording())
                rec.stopRecording();
            else
                rec.startRecording();
            refresh();
        }

        void chooseFolder()
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose recording folder", rec.outputFolder());
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::FileChooser& fc) {
                    auto f = fc.getResult();
                    if (f != juce::File())
                    {
                        rec.setOutputFolder (f);
                        refresh();
                    }
                });
        }

        void timerCallback() override
        {
            if (rec.isRecording())
            {
                timeLabel.setText (formatTime (rec.recordedSeconds()), juce::dontSendNotification);
                // File size needs a stat() syscall on the message (UI) thread; the
                // time readout comes from an atomic, so it stays the live one and
                // we poll size at ~4 Hz, not 15, to keep the UI thread off the disk.
                if (++sizeTick % 4 == 0)
                    sizeLabel.setText (juce::File::descriptionOfSizeInBytes (rec.currentFile().getSize()),
                        juce::dontSendNotification);
            }
            repaint (meterArea);
            updateButton();
        }

        void refresh()
        {
            const bool r = rec.isRecording();
            updateButton();
            formatBox.setEnabled (!r);
            bitsBox.setEnabled (!r && formatBox.getSelectedItemIndex() == 0); // bit depth: WAV only
            nameField.setEnabled (!r);
            folderButton.setEnabled (!r);
            folderLabel.setText (rec.outputFolder().getFullPathName(), juce::dontSendNotification);
            if (!r)
            {
                timeLabel.setText ("00:00:00", juce::dontSendNotification);
                sizeLabel.setText ({}, juce::dontSendNotification);
            }
        }

        void updateButton()
        {
            const bool r = rec.isRecording();
            recordButton.setButtonText (r ? "Stop" : "Record");
            recordButton.setColour (juce::TextButton::buttonColourId,
                r ? juce::Colour::fromRGB (200, 40, 40) : juce::Colour::fromRGB (50, 50, 58));
            recordButton.setEnabled (r || rec.audioFlowing());
        }

        static juce::String formatTime (double seconds)
        {
            const int total = (int) seconds;
            return juce::String::formatted ("%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
        }

        RecorderProcessor& rec;
        juce::TextButton recordButton { "Record" }, folderButton, revealButton;
        juce::Label timeLabel, sizeLabel, folderLabel;
        juce::ComboBox formatBox, bitsBox;
        juce::TextEditor nameField;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> formatAtt, bitsAtt;
        std::unique_ptr<juce::FileChooser> chooser;
        juce::Rectangle<int> meterArea;
        int sizeTick = 0; // throttles the file-size stat() in timerCallback
    };
} // namespace dcr::builtin
