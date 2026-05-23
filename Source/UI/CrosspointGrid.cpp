#include "UI/CrosspointGrid.h"

#include "Routing/RoutingMatrix.h"

#include <cmath>

namespace dcr {

CrosspointGrid::CrosspointGrid (RoutingMatrix& m) : matrix (m)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    setOpaque (true);
}

float CrosspointGrid::dbToLin (float db) noexcept
{
    if (db <= -60.0f) return 0.0f;
    return std::pow (10.0f, db * 0.05f);
}

float CrosspointGrid::linToDb (float lin) noexcept
{
    if (lin <= 1.0e-6f) return -60.0f;
    return 20.0f * std::log10 (lin);
}

void CrosspointGrid::setDimensions (int newIns, int newOuts, int newCellSize)
{
    numIns   = newIns;
    numOuts  = newOuts;
    cellSize = newCellSize;
    setSize (numOuts * cellSize, numIns * cellSize);
    repaint();
}

juce::Rectangle<int> CrosspointGrid::cellBounds (int m, int n) const noexcept
{
    return { m * cellSize, n * cellSize, cellSize, cellSize };
}

bool CrosspointGrid::hitTestCell (int x, int y, int& outIdx, int& inIdx) const noexcept
{
    if (cellSize <= 0) return false;
    outIdx = x / cellSize;
    inIdx  = y / cellSize;
    return outIdx >= 0 && outIdx < numOuts && inIdx >= 0 && inIdx < numIns;
}

void CrosspointGrid::invalidateCell (int m, int n)
{
    if (m >= 0 && m < numOuts && n >= 0 && n < numIns)
        repaint (cellBounds (m, n));
}

void CrosspointGrid::setHighlightedColumns (std::vector<int> cols)
{
    highlightedColumns = std::move (cols);
    repaint();
}

void CrosspointGrid::paint (juce::Graphics& g)
{
    // Background.
    g.fillAll (juce::Colour::fromRGB (24, 24, 28));

    // Only iterate cells inside the dirty clip rectangle.  Critical for
    // scrolling matrices with hundreds of thousands of cells.
    const auto clip = g.getClipBounds();
    const int mStart = juce::jmax (0, clip.getX() / cellSize);
    const int mEnd   = juce::jmin (numOuts, (clip.getRight() + cellSize - 1) / cellSize);
    const int nStart = juce::jmax (0, clip.getY() / cellSize);
    const int nEnd   = juce::jmin (numIns,  (clip.getBottom() + cellSize - 1) / cellSize);

    g.setFont (juce::FontOptions (10.0f));

    for (int m = mStart; m < mEnd; ++m)
    {
        for (int n = nStart; n < nEnd; ++n)
        {
            auto r = cellBounds (m, n).toFloat().reduced (1.5f);
            const float gain = matrix.getCrosspoint (m, n);
            const bool on = gain > 1.0e-6f;

            g.setColour (juce::Colour::fromRGB (40, 40, 46));
            g.fillRoundedRectangle (r, 2.0f);

            if (on)
            {
                const float db = linToDb (gain);
                const float t  = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 72.0f);
                auto col = juce::Colour::fromHSV (0.36f - t * 0.36f, 0.85f, 0.55f + t * 0.45f, 1.0f);
                g.setColour (col);
                g.fillRoundedRectangle (r, 2.0f);

                if (cellSize >= 28)   // skip text for very small cells
                {
                    g.setColour (juce::Colours::white.withAlpha (0.9f));
                    juce::String label = db <= -59.0f ? juce::String ("-inf")
                                                      : juce::String (db, 1) + " dB";
                    g.drawText (label, r.toNearestInt(), juce::Justification::centred);
                }
            }

            g.setColour (juce::Colour::fromRGB (70, 70, 76));
            g.drawRoundedRectangle (r, 2.0f, 0.5f);
        }
    }

    // Hover highlight overlay over entire highlighted columns.
    if (! highlightedColumns.empty())
    {
        g.setColour (juce::Colour::fromRGBA (255, 220, 80, 38));
        for (int m : highlightedColumns)
            if (m >= 0 && m < numOuts)
                g.fillRect (m * cellSize, 0, cellSize, getHeight());
    }
}

void CrosspointGrid::mouseDown (const juce::MouseEvent& e)
{
    dragOut = dragIn = -1;
    draggedSinceMouseDown = false;
    int m, n;
    if (! hitTestCell (e.x, e.y, m, n)) return;
    dragOut = m; dragIn = n;
    dragStartDb = linToDb (matrix.getCrosspoint (m, n));
    if (e.mods.isPopupMenu())
    {
        promptForDb (m, n);
        dragOut = dragIn = -1;
    }
}

void CrosspointGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (dragOut < 0 || e.mods.isPopupMenu()) return;
    if (std::abs (e.getDistanceFromDragStartY()) < 3 && ! draggedSinceMouseDown) return;
    draggedSinceMouseDown = true;
    const float newDb = juce::jlimit (-60.0f, 12.0f,
                                       dragStartDb + (-e.getDistanceFromDragStartY()) * 0.25f);
    matrix.setCrosspoint (dragOut, dragIn, dbToLin (newDb));
    invalidateCell (dragOut, dragIn);
}

void CrosspointGrid::mouseUp (const juce::MouseEvent& e)
{
    if (dragOut < 0 || e.mods.isPopupMenu()) { dragOut = dragIn = -1; return; }
    if (! draggedSinceMouseDown)
    {
        const float cur = matrix.getCrosspoint (dragOut, dragIn);
        matrix.setCrosspoint (dragOut, dragIn, cur > 1.0e-6f ? 0.0f : 1.0f);
        invalidateCell (dragOut, dragIn);
    }
    dragOut = dragIn = -1;
}

void CrosspointGrid::mouseDoubleClick (const juce::MouseEvent& e)
{
    int m, n;
    if (! hitTestCell (e.x, e.y, m, n)) return;
    matrix.setCrosspoint (m, n, 1.0f);
    invalidateCell (m, n);
}

void CrosspointGrid::promptForDb (int m, int n)
{
    auto* dialog = new juce::AlertWindow ("Crosspoint gain",
                                          "Enter gain in dB (-60 to +12, or 'off')",
                                          juce::AlertWindow::NoIcon);
    dialog->addTextEditor ("db", juce::String (linToDb (matrix.getCrosspoint (m, n)), 1));
    dialog->addButton ("OK",     1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    dialog->enterModalState (true,
        juce::ModalCallbackFunction::create ([this, dialog, m, n] (int result)
        {
            if (result == 1)
            {
                auto txt = dialog->getTextEditorContents ("db").trim();
                float gain;
                if (txt.equalsIgnoreCase ("off") || txt.equalsIgnoreCase ("-inf"))
                    gain = 0.0f;
                else
                    gain = dbToLin (juce::jlimit (-60.0f, 12.0f, txt.getFloatValue()));
                matrix.setCrosspoint (m, n, gain);
                invalidateCell (m, n);
            }
            delete dialog;
        }), false);
}

} // namespace dcr
