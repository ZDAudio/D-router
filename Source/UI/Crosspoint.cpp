#include "UI/Crosspoint.h"

#include <cmath>

namespace dcr
{

    Crosspoint::Crosspoint()
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    float Crosspoint::dbToLin (float db) noexcept
    {
        if (db <= -60.0f)
            return 0.0f;
        return std::pow (10.0f, db * 0.05f);
    }

    float Crosspoint::linToDb (float lin) noexcept
    {
        if (lin <= 1.0e-6f)
            return -60.0f;
        return 20.0f * std::log10 (lin);
    }

    void Crosspoint::setLinearGain (float g)
    {
        gainLinear = juce::jlimit (0.0f, dbToLin (12.0f), g);
        repaint();
    }

    void Crosspoint::paint (juce::Graphics& g)
    {
        const auto r = getLocalBounds().toFloat().reduced (1.5f);
        const bool on = gainLinear > 1.0e-6f;

        g.setColour (juce::Colour::fromRGB (40, 40, 46));
        g.fillRoundedRectangle (r, 2.0f);

        if (on)
        {
            const float db = linToDb (gainLinear);
            // Map -60..+12 dB to 0..1 fill brightness.
            const float t = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 72.0f);
            auto col = juce::Colour::fromHSV (0.36f - t * 0.36f, 0.85f, 0.55f + t * 0.45f, 1.0f);
            g.setColour (col);
            g.fillRoundedRectangle (r, 2.0f);

            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.setFont (juce::FontOptions (10.0f));
            juce::String label = juce::String (db, 1) + " dB";
            if (db <= -59.0f)
                label = "-inf";
            g.drawText (label, r.toNearestInt(), juce::Justification::centred);
        }

        g.setColour (juce::Colour::fromRGB (70, 70, 76));
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
    }

    void Crosspoint::mouseDown (const juce::MouseEvent& e)
    {
        draggedSinceMouseDown = false;

        if (e.mods.isPopupMenu())
        {
            promptForDbValue();
            return;
        }

        dragStartDb = linToDb (gainLinear);
    }

    void Crosspoint::mouseDrag (const juce::MouseEvent& e)
    {
        if (e.mods.isPopupMenu())
            return;
        if (std::abs (e.getDistanceFromDragStartY()) < 3 && !draggedSinceMouseDown)
            return;
        draggedSinceMouseDown = true;

        // 1 dB per 4 vertical pixels (up = increase).
        float newDb = dragStartDb + (-e.getDistanceFromDragStartY()) * 0.25f;
        newDb = juce::jlimit (-60.0f, 12.0f, newDb);
        gainLinear = dbToLin (newDb);
        if (onChange)
            onChange (gainLinear);
        repaint();
    }

    void Crosspoint::mouseUp (const juce::MouseEvent& e)
    {
        if (e.mods.isPopupMenu())
            return;
        if (draggedSinceMouseDown)
            return;

        // Simple click toggles route.
        gainLinear = (gainLinear > 1.0e-6f) ? 0.0f : 1.0f;
        if (onChange)
            onChange (gainLinear);
        repaint();
    }

    void Crosspoint::mouseDoubleClick (const juce::MouseEvent&)
    {
        gainLinear = 1.0f;
        if (onChange)
            onChange (gainLinear);
        repaint();
    }

    void Crosspoint::promptForDbValue()
    {
        auto* dialog = new juce::AlertWindow ("Crosspoint gain",
            "Enter gain in dB (-60 to +12, or 'off')",
            juce::AlertWindow::NoIcon);
        dialog->addTextEditor ("db", juce::String (linToDb (gainLinear), 1));
        dialog->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        dialog->enterModalState (true,
            juce::ModalCallbackFunction::create ([this, dialog] (int result) {
                if (result == 1)
                {
                    auto txt = dialog->getTextEditorContents ("db").trim();
                    if (txt.equalsIgnoreCase ("off") || txt.equalsIgnoreCase ("-inf"))
                    {
                        gainLinear = 0.0f;
                    }
                    else
                    {
                        float db = juce::jlimit (-60.0f, 12.0f, txt.getFloatValue());
                        gainLinear = dbToLin (db);
                    }
                    if (onChange)
                        onChange (gainLinear);
                    repaint();
                }
                delete dialog;
            }),
            false);
    }

} // namespace dcr
