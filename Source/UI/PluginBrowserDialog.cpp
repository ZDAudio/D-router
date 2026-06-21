#include "UI/PluginBrowserDialog.h"

#include "Engine/AudioEngine.h"

namespace dcr
{

    namespace
    {
        juce::File getPluginListCacheFile()
        {
            return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                .getChildFile ("dcorerouter")
                .getChildFile ("au-list.xml");
        }
    }

    PluginBrowserDialog::PluginBrowserDialog (AudioEngine& e) : engine (e)
    {
        setSize (560, 480);

        title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        addAndMakeVisible (title);

        status.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        status.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (status);

        listBox.setModel (&listModel);
        listBox.setRowHeight (24);
        listBox.setColour (juce::ListBox::backgroundColourId, juce::Colour::fromRGB (22, 22, 26));
        addAndMakeVisible (listBox);

        rescanButton.onClick = [this] { startScan(); };
        cancelButton.onClick = [this] {
            if (onClose)
                onClose (std::nullopt);
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        };
        loadButton.onClick = [this] { chooseAndClose (listBox.getSelectedRow()); };

        addAndMakeVisible (rescanButton);
        addAndMakeVisible (cancelButton);
        addAndMakeVisible (loadButton);

        // Try to load cached plugin list.
        auto cache = getPluginListCacheFile();
        if (cache.existsAsFile())
        {
            if (auto xml = juce::parseXML (cache))
                engine.getKnownPluginList().recreateFromXml (*xml);
        }
        rebuildList();

        if (filteredList.empty())
            startScan();
        else
            status.setText (juce::String (filteredList.size()) + " plugins loaded from cache.",
                juce::dontSendNotification);

        startTimerHz (10);
    }

    PluginBrowserDialog::~PluginBrowserDialog()
    {
        stopTimer();
        stopScan();
    }

    void PluginBrowserDialog::stopScan()
    {
        stopRequested.store (true, std::memory_order_relaxed);
        if (scanThread && scanThread->joinable())
            scanThread->join();
        scanThread.reset();
        scanner.reset();
        scanRunning.store (false, std::memory_order_relaxed);
    }

    void PluginBrowserDialog::startScan()
    {
        if (scanRunning.load())
            return;
        stopScan();
        stopRequested.store (false, std::memory_order_relaxed);

        auto& fmt = *engine.getPluginFormatManager().getFormat (0); // AU
        auto paths = fmt.getDefaultLocationsToSearch();
        scanner = std::make_unique<juce::PluginDirectoryScanner> (
            engine.getKnownPluginList(), fmt, paths, true, juce::File {}, false);

        scanRunning.store (true, std::memory_order_relaxed);
        status.setText ("Scanning...", juce::dontSendNotification);
        scanThread = std::make_unique<std::thread> ([this] {
            juce::String name;
            while (!stopRequested.load (std::memory_order_relaxed))
            {
                if (!scanner->scanNextFile (true, name))
                    break;
                currentlyScanningFile = name;
            }
            scanRunning.store (false, std::memory_order_relaxed);

            // Save cache & refresh UI on message thread.
            juce::MessageManager::callAsync ([this] {
                if (auto xml = engine.getKnownPluginList().createXml())
                {
                    auto cache = getPluginListCacheFile();
                    cache.getParentDirectory().createDirectory();
                    xml->writeTo (cache);
                }
                rebuildList();
                status.setText (juce::String (filteredList.size()) + " plugins available.",
                    juce::dontSendNotification);
            });
        });
    }

    void PluginBrowserDialog::rebuildList()
    {
        filteredList.clear();
        for (const auto& d : engine.getKnownPluginList().getTypes())
            filteredList.push_back (d);
        std::sort (filteredList.begin(), filteredList.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
        listBox.updateContent();
        listBox.repaint();
    }

    void PluginBrowserDialog::timerCallback()
    {
        if (scanRunning.load())
            status.setText ("Scanning: " + currentlyScanningFile, juce::dontSendNotification);
    }

    void PluginBrowserDialog::chooseAndClose (int row)
    {
        if (row < 0 || row >= (int) filteredList.size())
            return;
        auto choice = filteredList[(size_t) row];
        if (onClose)
            onClose (choice);
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (1);
    }

    void PluginBrowserDialog::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour::fromRGB (32, 32, 36));
    }

    void PluginBrowserDialog::resized()
    {
        auto r = getLocalBounds().reduced (12);
        title.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        auto statusRow = r.removeFromTop (22);
        rescanButton.setBounds (statusRow.removeFromRight (90));
        statusRow.removeFromRight (8);
        status.setBounds (statusRow);
        r.removeFromTop (8);

        auto bottom = r.removeFromBottom (36);
        loadButton.setBounds (bottom.removeFromRight (90));
        bottom.removeFromRight (8);
        cancelButton.setBounds (bottom.removeFromRight (90));

        r.removeFromBottom (8);
        listBox.setBounds (r);
    }

    int PluginBrowserDialog::ListModel::getNumRows()
    {
        return (int) dialog.filteredList.size();
    }

    void PluginBrowserDialog::ListModel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
    {
        if (row < 0 || row >= (int) dialog.filteredList.size())
            return;
        auto& d = dialog.filteredList[(size_t) row];

        if (selected)
            g.fillAll (juce::Colour::fromRGB (60, 90, 140));
        else if (row % 2)
            g.fillAll (juce::Colour::fromRGB (28, 28, 32));

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (d.name, 8, 0, w - 200, h, juce::Justification::centredLeft, true);

        g.setColour (juce::Colour::fromRGB (160, 160, 170));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (d.manufacturerName, w - 200, 0, 192, h, juce::Justification::centredRight, true);
    }

    void PluginBrowserDialog::ListModel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
    {
        dialog.chooseAndClose (row);
    }

    void PluginBrowserDialog::launch (AudioEngine& engine,
        std::function<void (std::optional<juce::PluginDescription>)> cb)
    {
        auto* content = new PluginBrowserDialog (engine);
        content->onClose = std::move (cb);

        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = "Choose plugin";
        o.content.setOwned (content);
        o.dialogBackgroundColour = juce::Colour::fromRGB (32, 32, 36);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

} // namespace dcr
