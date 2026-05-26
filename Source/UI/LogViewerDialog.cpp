#include "UI/LogViewerDialog.h"

#include "Diagnostics/Logger.h"

namespace dcr {

LogViewerDialog::LogViewerDialog()
{
    header.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold));
    header.setColour (juce::Label::textColourId, juce::Colour::fromRGB (180, 200, 255));
    addAndMakeVisible (header);

    body.setMultiLine (true);
    body.setReadOnly (true);
    body.setScrollbarsShown (true);
    body.setCaretVisible (false);
    body.setColour (juce::TextEditor::backgroundColourId, juce::Colour::fromRGB (12, 12, 14));
    body.setColour (juce::TextEditor::textColourId,       juce::Colour::fromRGB (210, 210, 215));
    body.setFont   (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, 0));
    addAndMakeVisible (body);

    refreshBtn.onClick  = [this] { refresh(); };
    revealBtn .onClick  = [] {
        auto f = Logger::getCurrentLogFile();
        if (f.existsAsFile()) f.revealToUser();
        else                  Logger::getLogDirectory().revealToUser();
    };
    copyPathBtn.onClick = [] {
        juce::SystemClipboard::copyTextToClipboard (Logger::getCurrentLogFile().getFullPathName());
    };
    closeBtn .onClick   = [this]
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
    };

    addAndMakeVisible (refreshBtn);
    addAndMakeVisible (revealBtn);
    addAndMakeVisible (copyPathBtn);
    addAndMakeVisible (closeBtn);

    setSize (840, 560);
    refresh();
    // Live-tail at 1 Hz so the user can watch events stream in.
    startTimer (1000);
}

void LogViewerDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (24, 24, 28));
}

void LogViewerDialog::resized()
{
    auto r = getLocalBounds().reduced (10);

    auto top = r.removeFromTop (22);
    header.setBounds (top);
    r.removeFromTop (6);

    auto bottom = r.removeFromBottom (36);
    closeBtn   .setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (6);
    copyPathBtn.setBounds (bottom.removeFromRight (130));
    bottom.removeFromRight (6);
    revealBtn  .setBounds (bottom.removeFromRight (140));
    bottom.removeFromRight (6);
    refreshBtn .setBounds (bottom.removeFromRight (90));
    r.removeFromBottom (6);

    body.setBounds (r);
}

void LogViewerDialog::timerCallback() { refresh(); }

void LogViewerDialog::refresh()
{
    const auto path = Logger::getCurrentLogFile().getFullPathName();
    header.setText ("Session log:  " + path, juce::dontSendNotification);

    // Pull from the in-memory ring rather than re-reading the file -- much
    // faster and avoids tearing if the file is mid-flush.
    auto lines = Logger::getRecentLines (2000);
    const auto text = lines.joinIntoString ("\n");

    // Skip the setText (which triggers a full re-layout + scroll reset) if
    // nothing changed since the last tick.
    if (text == body.getText()) return;

    // Auto-tail: only re-snap to the bottom when the user hadn't scrolled
    // up to read history.  juce::TextEditor::getCaretPosition() == length
    // of text is a cheap proxy for "caret is at the end" which is true on
    // first show and after we last auto-tailed.
    const bool wasAtEnd = body.getCaretPosition() >= body.getText().length() - 1;
    body.setText (text, false);
    if (wasAtEnd)
        body.moveCaretToEnd();
}

void LogViewerDialog::launch()
{
    auto* content = new LogViewerDialog();

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle                  = "D-Router Diagnostics";
    o.content.setOwned             (content);
    o.dialogBackgroundColour       = juce::Colour::fromRGB (24, 24, 28);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar            = true;
    o.resizable                    = true;
    o.launchAsync();
}

} // namespace dcr
