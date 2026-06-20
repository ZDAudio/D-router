#include "UI/SettingsDialog.h"

#include <AudioToolbox/AudioToolbox.h>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace dcr {

namespace
{
    constexpr int rowH        = 28;
    constexpr int sectionGap  = 18;
    constexpr int infoW       = 18;
    constexpr int labelW      = 230;
    constexpr int editorW     = 180;
    constexpr int leftPad     = 12;
    constexpr int gap         = 6;
}

void InfoIcon::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    g.setColour (juce::Colour::fromRGB (90, 130, 200));
    g.fillEllipse (r);
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("i", getLocalBounds(), juce::Justification::centred);
}

SettingsDialog::SettingsDialog (const EngineSettings& initial) : working (initial)
{
    setSize (560, 660);

    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&fieldsHolder, false);
    viewport.setScrollBarsShown (true, false);

    // ====== Engine clock ======
    addSection ("Engine clock",
                "Internal sample rate + block size.  Per-device SRC converts in/out "
                "of these when a device runs at a different rate.");
    addDoubleChoiceField ("Engine sample rate", working.engineSampleRate,
        { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 }, "Hz",
        "All matrix processing happens at this rate.  Per-device SRC converts to/from "
        "the device's own rate when they differ.\n\n"
        "Recommended: 48000 (Apple default for modern audio), 44100 (legacy CDs / iTunes).");
    addIntChoiceField ("Engine block size", working.engineBlockSize,
        { 64, 128, 256, 512, 1024, 2048 }, "samples",
        "Number of samples processed per matrix block.  Smaller = lower latency, more "
        "CPU per second.  Larger = more stable but slower response.\n\n"
        "Recommended: 128 (default, low latency), 256 (balanced), 64 (ultra-low if CPU allows).");

    // ====== Ring buffers ======
    //  Free-form integers here used to allow values that produced gigabyte-
    //  sized rings on multi-channel devices; ComboBoxes limit the user to a
    //  curated set the engine is known to allocate without OOM.
    addSection ("Buffer safety",
                "One slider for all the ring / pre-fill sizes.  Drag RIGHT for safety "
                "(bigger buffers, more drift tolerance, more latency); LEFT for speed "
                "(smaller buffers, lowest latency, less margin for CPU spikes).");
    addBufferSafetyField (
        "Sets all five buffer sizes together (input/output ring x engineBlock, "
        "input/output ring x devBuf, output pre-fill).\n\n"
        "  Safest      -- biggest buffers; survives clock drift + CPU spikes, highest latency.\n"
        "  Safe        -- the default; robust for most setups.\n"
        "  Balanced    -- moderate latency, still forgiving.\n"
        "  Low latency -- tight timing; needs a healthy CPU and matched clocks.\n"
        "  Aggressive  -- smallest buffers, lowest latency; any hiccup = a dropout.\n\n"
        "If you hear pops / dropouts (xrun in Engine Monitor), drag toward Safe.");

    // ====== SRC ======
    addSection ("Sample rate converter",
                "Used only when a device's native SR differs from the engine SR.  "
                "Higher quality = better fidelity, more CPU + a bit more latency.");
    addUIntComboField ("SRC quality",
        working.srcQuality,
        { "Min", "Low", "Medium", "High", "Max" },
        { kAudioConverterQuality_Min,
          kAudioConverterQuality_Low,
          kAudioConverterQuality_Medium,
          kAudioConverterQuality_High,
          kAudioConverterQuality_Max },
        "Apple AudioConverter quality.  Higher = better aliasing rejection at higher CPU.  "
        "Only applies when device SR != engine SR (bypassed otherwise).\n\n"
        "Recommended: Max for music routing, Medium for ultra-low CPU.");
    addUIntComboField ("SRC complexity",
        working.srcComplexity,
        { "Linear", "Normal", "Mastering", "Minimum phase" },
        { kAudioConverterSampleRateConverterComplexity_Linear,
          kAudioConverterSampleRateConverterComplexity_Normal,
          kAudioConverterSampleRateConverterComplexity_Mastering,
          kAudioConverterSampleRateConverterComplexity_MinimumPhase },
        "SRC algorithm flavour.\n"
        "  Linear: cheapest, audible artifacts.\n"
        "  Normal: general-purpose.\n"
        "  Mastering: long linear-phase FIR, highest fidelity, more latency.\n"
        "  Minimum phase: shorter latency, asymmetric impulse.\n\n"
        "Recommended: Mastering for music, Normal for live monitoring (lower latency).");

    // ====== Matrix thread ======
    addSection ("Matrix processor thread",
                "How aggressively the audio worker polls for data, and how the "
                "trim / mute / fader changes ramp.  Touch only if you see "
                "high 'polls/block' in Engine Monitor or hear zipper noise.");
    addIntField ("Idle sleep",            working.matrixThreadSleepMicros, 50, 5000, "us",
        "When no input is ready, the matrix thread sleeps this long before polling again.  "
        "Shorter = faster response, more CPU.\n\n"
        "Recommended: 250 us (4000 polls/sec).  Don't go below 100 us without good reason.");
    addIntField ("Drain blocks per wake", working.matrixDrainPerWake,      1, 256, {},
        "Maximum blocks to process per wake cycle before yielding.  Prevents the matrix "
        "thread from monopolising a core if input is rapidly arriving.\n\n"
        "Recommended: 16.");
    addIntField ("Gain smoothing", working.gainSmoothingMs,     0, 500, "ms",
        "Time constant for per-route gain ramps.  Any time a trim / crosspoint / mute / "
        "group fader changes, the effective gain interpolates toward the new value with "
        "this time constant -- prevents the click / zipper noise of a sudden jump.\n"
        "Also makes device add/remove fade out gracefully before the engine restarts.\n\n"
        "Recommended: 25 ms.  Set to 0 for instant jumps (you will hear clicks).");

    // ====== UI ======
    addSection ("UI",
                "Visual refresh rates only.  Lower = less CPU spent on the UI, "
                "no effect on audio.");
    addIntField   ("Meter refresh rate",  working.meterTimerHz,      1, 120, "Hz",
        "How often level meters are repainted.\n\n"
        "Recommended: 30 Hz (smooth and easy on CPU).  60 for snappier feel; lower if "
        "the UI is sluggish with very many channels.");
    addFloatField ("Meter decay factor",  working.meterDecayFactor,  0.0f, 0.999f, {},
        "Per-frame multiplicative decay applied to the meter level.  0 = instant fall, "
        "1 = peak hold forever.\n\n"
        "Recommended: 0.92 (around -6 dB / 150 ms at 30 fps).");
    addIntField   ("Engine status refresh", working.statusTimerMs,   100, 10000, "ms",
        "How often the Engine Monitor panel (CPU bar, dropout / xrun counters, ring "
        "fills, latency table) is refreshed.  Higher = lower CPU spent on the diagnostic "
        "display.\n\n"
        "Recommended: 1000 ms (default).  Drop to 250 ms while debugging glitches; raise "
        "to 5000 ms when you don't care about live diagnostics.");

    // ====== Theme ======
    addSection ("Theme (6-char RGB hex)",
                "Accent color appears on knobs, fader caps, status panel.  "
                "Warning / critical colors are used by the engine monitor severity.");
    addHexColorField ("Accent color",   working.accentColorRGB,
        "Primary accent colour used for the title, knob arcs, fader caps, focus rings, "
        "active text in the status panel, and other 'alive' UI accents.\n\n"
        "Recommended: 00FFD2 (neon teal), 7AC8FF (cool blue), FFB300 (warm amber).");
    addHexColorField ("Warning color",  working.warningColorRGB,
        "Status panel colour when CPU exceeds the warn ratio or stalled exceeds the "
        "warn ratio.\n\nRecommended: FFCC00 (amber).");
    addHexColorField ("Critical color", working.criticalColorRGB,
        "Status panel colour when CPU/stalled exceed the critical ratios or a dropout "
        "occurred in the last 5 seconds.\n\nRecommended: FF3B30 (red).");

    fieldsHolder.setSize (520, nextRowY + 12);

    applyButton.setTooltip ("Apply changes to the running engine for this session only.\n"
                            "If you close the app, the previously saved values come back.");
    saveButton .setTooltip ("Apply AND write to disk so these values are the new defaults "
                            "on every launch.");
    resetButton.setTooltip ("Restore every field to its built-in default value.\n"
                            "You still need to click Save afterwards to persist that to disk.");

    applyButton .onClick = [this]
    {
        for (auto& a : applyActions) a();
        if (onClose) onClose (working, /*persistToDisk=*/false);
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (1);
    };
    saveButton .onClick = [this]
    {
        for (auto& a : applyActions) a();
        if (onClose) onClose (working, /*persistToDisk=*/true);
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (2);
    };
    cancelButton.onClick = [this]
    {
        if (onClose) onClose (std::nullopt, false);
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
    };
    resetButton.onClick = [this]
    {
        working = EngineSettings{};
        // Don't auto-close - user can review defaults and then Apply / Save.
        // But we DO need to refresh field values to show the defaults.  Easiest:
        // tell the host so it can re-launch.  For now we just close, treating
        // reset as an "apply" since the user explicitly chose to wipe.
        if (onClose) onClose (working, /*persistToDisk=*/false);
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (1);
    };

    addAndMakeVisible (applyButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (resetButton);
}

void SettingsDialog::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (32, 32, 36));
}

void SettingsDialog::resized()
{
    auto r = getLocalBounds().reduced (12);

    auto bottom = r.removeFromBottom (36);
    saveButton  .setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (6);
    applyButton .setBounds (bottom.removeFromRight (90));
    bottom.removeFromRight (6);
    cancelButton.setBounds (bottom.removeFromRight (90));
    resetButton .setBounds (bottom.removeFromLeft (160));

    r.removeFromBottom (8);
    viewport.setBounds (r);
}

void SettingsDialog::addSection (const juce::String& heading,
                                 const juce::String& subtitle)
{
    if (nextRowY > 0) nextRowY += sectionGap;
    auto* h = new juce::Label ({}, heading);
    h->setFont (juce::FontOptions (13.0f, juce::Font::bold));
    h->setColour (juce::Label::textColourId, juce::Colour::fromRGB (180, 200, 255));
    h->setBounds (leftPad, nextRowY, fieldsHolder.getWidth() - leftPad - 8, rowH);
    fieldsHolder.addAndMakeVisible (*h);
    sectionHeads.add (h);
    nextRowY += rowH + 2;

    if (subtitle.isNotEmpty())
    {
        auto* sub = new juce::Label ({}, subtitle);
        sub->setFont (juce::FontOptions (11.0f, 0));
        sub->setColour (juce::Label::textColourId, juce::Colour::fromRGB (130, 130, 140));
        sub->setBounds (leftPad, nextRowY, fieldsHolder.getWidth() - leftPad - 8, rowH - 6);
        fieldsHolder.addAndMakeVisible (*sub);
        sectionHeads.add (sub);
        nextRowY += rowH - 4;
    }
}

void SettingsDialog::attachInfoIcon (const juce::String& tooltip)
{
    auto* icon = new InfoIcon();
    icon->setTooltip (tooltip);
    icon->setBounds (leftPad, nextRowY + 4, infoW, infoW);
    fieldsHolder.addAndMakeVisible (*icon);
    infoIcons.add (icon);
}

void SettingsDialog::addIntField (const juce::String& name, int& target,
                                  int minVal, int maxVal,
                                  const juce::String& unitHint,
                                  const juce::String& tooltip)
{
    attachInfoIcon (tooltip);

    auto* lbl = new juce::Label ({}, name + (unitHint.isEmpty() ? "" : "  (" + unitHint + ")"));
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setBounds (leftPad + infoW + gap, nextRowY, labelW, rowH - 4);
    lbl->setTooltip (tooltip);
    fieldsHolder.addAndMakeVisible (*lbl);
    labels.add (lbl);

    auto* ed = new juce::TextEditor();
    ed->setInputRestrictions (12, "-0123456789");
    ed->setText (juce::String (target), juce::dontSendNotification);
    ed->setBounds (leftPad + infoW + gap + labelW + gap, nextRowY, editorW, rowH - 4);
    fieldsHolder.addAndMakeVisible (*ed);
    editors.add (ed);

    applyActions.push_back ([&target, ed, minVal, maxVal]
    {
        target = juce::jlimit (minVal, maxVal, ed->getText().getIntValue());
    });

    nextRowY += rowH;
}

namespace
{
    // 5 presets, index 0 = most aggressive (lowest latency) .. 4 = safest.
    // Order of fields: { inputRingMultEng, inputRingMultDev,
    //                    outputRingMultEng, outputRingMultDev, outputPreFillBlocks }.
    struct BufferPreset { const char* name; int inEng, inDev, outEng, outDev, preFill; };
    const BufferPreset kBufferPresets[5] = {
        { "Aggressive",  2, 2,  3,  3,  1 },
        { "Low latency", 2, 2,  4,  4,  2 },
        { "Balanced",    3, 3,  6,  6,  4 },
        { "Safe",        3, 4,  8,  8,  8 },   // == engine defaults
        { "Safest",      4, 6, 12, 12, 16 },
    };

    // Pick the preset closest to the current field values (sum of abs diffs).
    int closestBufferPreset (const EngineSettings& s)
    {
        int best = 3, bestErr = std::numeric_limits<int>::max();
        for (int i = 0; i < 5; ++i)
        {
            const auto& p = kBufferPresets[i];
            const int err = std::abs (s.inputRingMultEng  - p.inEng)
                          + std::abs (s.inputRingMultDev  - p.inDev)
                          + std::abs (s.outputRingMultEng - p.outEng)
                          + std::abs (s.outputRingMultDev - p.outDev)
                          + std::abs (s.outputPreFillBlocks - p.preFill);
            if (err < bestErr) { bestErr = err; best = i; }
        }
        return best;
    }
}

void SettingsDialog::addBufferSafetyField (const juce::String& tooltip)
{
    attachInfoIcon (tooltip);

    auto* lbl = new juce::Label ({}, "Buffer safety");
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setBounds (leftPad + infoW + gap, nextRowY, labelW, rowH - 4);
    lbl->setTooltip (tooltip);
    fieldsHolder.addAndMakeVisible (*lbl);
    labels.add (lbl);

    auto* s = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
    s->setRange (0.0, 4.0, 1.0);
    s->setValue ((double) closestBufferPreset (working), juce::dontSendNotification);
    s->setTextBoxStyle (juce::Slider::TextBoxRight, true, 100, rowH - 4);
    s->textFromValueFunction = [] (double v)
    {
        const int i = juce::jlimit (0, 4, (int) std::lround (v));
        return juce::String (kBufferPresets[i].name);
    };
    s->valueFromTextFunction = [] (const juce::String& t)
    {
        for (int i = 0; i < 5; ++i) if (t == kBufferPresets[i].name) return (double) i;
        return 3.0;
    };
    // Wider than a normal editor so the named stops are easy to hit.
    const int sliderW = labelW + editorW;
    s->setBounds (leftPad + infoW + gap + labelW + gap, nextRowY, sliderW, rowH - 4);

    auto applyLevel = [this] (int level)
    {
        const auto& p = kBufferPresets[juce::jlimit (0, 4, level)];
        working.inputRingMultEng    = p.inEng;
        working.inputRingMultDev    = p.inDev;
        working.outputRingMultEng   = p.outEng;
        working.outputRingMultDev   = p.outDev;
        working.outputPreFillBlocks = p.preFill;
    };
    s->onValueChange = [s, applyLevel] { applyLevel ((int) std::lround (s->getValue())); };

    fieldsHolder.addAndMakeVisible (*s);
    sliders.add (s);

    // Commit on apply too, in case onValueChange never fired.
    applyActions.push_back ([s, applyLevel] { applyLevel ((int) std::lround (s->getValue())); });

    nextRowY += rowH;
}

void SettingsDialog::addDoubleField (const juce::String& name, double& target,
                                     double minVal, double maxVal,
                                     const juce::String& unitHint,
                                     const juce::String& tooltip)
{
    attachInfoIcon (tooltip);

    auto* lbl = new juce::Label ({}, name + (unitHint.isEmpty() ? "" : "  (" + unitHint + ")"));
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setBounds (leftPad + infoW + gap, nextRowY, labelW, rowH - 4);
    lbl->setTooltip (tooltip);
    fieldsHolder.addAndMakeVisible (*lbl);
    labels.add (lbl);

    auto* ed = new juce::TextEditor();
    ed->setInputRestrictions (16, "-0123456789.");
    ed->setText (juce::String (target, 4), juce::dontSendNotification);
    ed->setBounds (leftPad + infoW + gap + labelW + gap, nextRowY, editorW, rowH - 4);
    fieldsHolder.addAndMakeVisible (*ed);
    editors.add (ed);

    applyActions.push_back ([&target, ed, minVal, maxVal]
    {
        target = juce::jlimit (minVal, maxVal, ed->getText().getDoubleValue());
    });

    nextRowY += rowH;
}

void SettingsDialog::addFloatField (const juce::String& name, float& target,
                                    float minVal, float maxVal,
                                    const juce::String& unitHint,
                                    const juce::String& tooltip)
{
    attachInfoIcon (tooltip);

    auto* lbl = new juce::Label ({}, name + (unitHint.isEmpty() ? "" : "  (" + unitHint + ")"));
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setBounds (leftPad + infoW + gap, nextRowY, labelW, rowH - 4);
    lbl->setTooltip (tooltip);
    fieldsHolder.addAndMakeVisible (*lbl);
    labels.add (lbl);

    auto* ed = new juce::TextEditor();
    ed->setInputRestrictions (16, "-0123456789.");
    ed->setText (juce::String (target, 4), juce::dontSendNotification);
    ed->setBounds (leftPad + infoW + gap + labelW + gap, nextRowY, editorW, rowH - 4);
    fieldsHolder.addAndMakeVisible (*ed);
    editors.add (ed);

    applyActions.push_back ([&target, ed, minVal, maxVal]
    {
        target = juce::jlimit (minVal, maxVal, (float) ed->getText().getDoubleValue());
    });

    nextRowY += rowH;
}

void SettingsDialog::addUIntComboField (const juce::String& name, unsigned int& target,
                                        const juce::StringArray& labelsList,
                                        const std::vector<unsigned int>& values,
                                        const juce::String& tooltip)
{
    attachInfoIcon (tooltip);

    auto* lbl = new juce::Label ({}, name);
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setBounds (leftPad + infoW + gap, nextRowY, labelW, rowH - 4);
    lbl->setTooltip (tooltip);
    fieldsHolder.addAndMakeVisible (*lbl);
    labels.add (lbl);

    auto* cb = new juce::ComboBox();
    int selectId = 1;
    for (int i = 0; i < labelsList.size() && i < (int) values.size(); ++i)
    {
        cb->addItem (labelsList[i], i + 1);
        if (values[(size_t) i] == target) selectId = i + 1;
    }
    cb->setSelectedId (selectId, juce::dontSendNotification);
    cb->setBounds (leftPad + infoW + gap + labelW + gap, nextRowY, editorW, rowH - 4);
    fieldsHolder.addAndMakeVisible (*cb);
    combos.add (cb);

    applyActions.push_back ([&target, cb, values]
    {
        const int idx = cb->getSelectedId() - 1;
        if (idx >= 0 && idx < (int) values.size())
            target = values[(size_t) idx];
    });

    nextRowY += rowH;
}

void SettingsDialog::addIntChoiceField (const juce::String& name, int& target,
                                        const std::vector<int>& values,
                                        const juce::String& unitHint,
                                        const juce::String& tooltip)
{
    attachInfoIcon (tooltip);

    auto* lbl = new juce::Label ({}, name + (unitHint.isEmpty() ? "" : "  (" + unitHint + ")"));
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setBounds (leftPad + infoW + gap, nextRowY, labelW, rowH - 4);
    lbl->setTooltip (tooltip);
    fieldsHolder.addAndMakeVisible (*lbl);
    labels.add (lbl);

    auto* cb = new juce::ComboBox();
    int snapId = 1;
    int bestDiff = std::numeric_limits<int>::max();
    for (size_t i = 0; i < values.size(); ++i)
    {
        cb->addItem (juce::String (values[i]), (int) i + 1);
        const int d = std::abs (values[i] - target);
        if (d < bestDiff) { bestDiff = d; snapId = (int) i + 1; }
    }
    cb->setSelectedId (snapId, juce::dontSendNotification);
    cb->setBounds (leftPad + infoW + gap + labelW + gap, nextRowY, editorW, rowH - 4);
    fieldsHolder.addAndMakeVisible (*cb);
    combos.add (cb);

    applyActions.push_back ([&target, cb, values]
    {
        const int idx = cb->getSelectedId() - 1;
        if (idx >= 0 && idx < (int) values.size())
            target = values[(size_t) idx];
    });

    nextRowY += rowH;
}

void SettingsDialog::addDoubleChoiceField (const juce::String& name, double& target,
                                           const std::vector<double>& values,
                                           const juce::String& unitHint,
                                           const juce::String& tooltip)
{
    attachInfoIcon (tooltip);

    auto* lbl = new juce::Label ({}, name + (unitHint.isEmpty() ? "" : "  (" + unitHint + ")"));
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setBounds (leftPad + infoW + gap, nextRowY, labelW, rowH - 4);
    lbl->setTooltip (tooltip);
    fieldsHolder.addAndMakeVisible (*lbl);
    labels.add (lbl);

    auto* cb = new juce::ComboBox();
    int snapId = 1;
    double bestDiff = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < values.size(); ++i)
    {
        cb->addItem (juce::String (values[i]), (int) i + 1);
        const double d = std::abs (values[i] - target);
        if (d < bestDiff) { bestDiff = d; snapId = (int) i + 1; }
    }
    cb->setSelectedId (snapId, juce::dontSendNotification);
    cb->setBounds (leftPad + infoW + gap + labelW + gap, nextRowY, editorW, rowH - 4);
    fieldsHolder.addAndMakeVisible (*cb);
    combos.add (cb);

    applyActions.push_back ([&target, cb, values]
    {
        const int idx = cb->getSelectedId() - 1;
        if (idx >= 0 && idx < (int) values.size())
            target = values[(size_t) idx];
    });

    nextRowY += rowH;
}

void SettingsDialog::addHexColorField (const juce::String& name, unsigned int& target,
                                       const juce::String& tooltip)
{
    attachInfoIcon (tooltip);

    auto* lbl = new juce::Label ({}, name);
    lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    lbl->setBounds (leftPad + infoW + gap, nextRowY, labelW, rowH - 4);
    lbl->setTooltip (tooltip);
    fieldsHolder.addAndMakeVisible (*lbl);
    labels.add (lbl);

    auto* ed = new juce::TextEditor();
    ed->setInputRestrictions (6, "0123456789abcdefABCDEF");
    ed->setText (juce::String::toHexString ((int) target).paddedLeft ('0', 6).toUpperCase(),
                 juce::dontSendNotification);
    ed->setBounds (leftPad + infoW + gap + labelW + gap, nextRowY, editorW, rowH - 4);
    fieldsHolder.addAndMakeVisible (*ed);
    editors.add (ed);

    // Small color swatch next to the editor.
    auto* swatchLbl = new juce::Label ({}, "  ");
    swatchLbl->setColour (juce::Label::backgroundColourId,
                          juce::Colour (0xFF000000u | target));
    swatchLbl->setBounds (leftPad + infoW + gap + labelW + gap + editorW + 6, nextRowY, 24, rowH - 4);
    fieldsHolder.addAndMakeVisible (*swatchLbl);
    labels.add (swatchLbl);

    ed->onTextChange = [ed, swatchLbl]
    {
        const auto hex = ed->getText().getHexValue32() & 0xFFFFFF;
        swatchLbl->setColour (juce::Label::backgroundColourId,
                              juce::Colour (0xFF000000u | (unsigned int) hex));
        swatchLbl->repaint();
    };

    applyActions.push_back ([&target, ed]
    {
        target = (unsigned int) (ed->getText().getHexValue32() & 0xFFFFFF);
    });

    nextRowY += rowH;
}

void SettingsDialog::launch (const EngineSettings& initial,
                             std::function<void (std::optional<EngineSettings>, bool)> cb)
{
    auto* content = new SettingsDialog (initial);
    content->onClose = std::move (cb);

    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle                  = "Settings";
    o.content.setOwned             (content);
    o.dialogBackgroundColour       = juce::Colour::fromRGB (32, 32, 36);
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar            = true;
    o.resizable                    = true;
    o.launchAsync();
}

} // namespace dcr
