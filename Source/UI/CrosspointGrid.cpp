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

void CrosspointGrid::setHighlightedRows (std::vector<int> rows)
{
    highlightedRows = std::move (rows);
    repaint();
}

void CrosspointGrid::setDeviceBoundaries (std::vector<int> ins, std::vector<int> outs)
{
    inputDeviceBoundaries = std::move (ins);
    outputDeviceBoundaries = std::move (outs);
    repaint();
}

void CrosspointGrid::paint (juce::Graphics& g)
{
    // Background.
    g.fillAll (juce::Colour::fromRGB (12, 12, 14));

    // Only iterate cells inside the dirty clip rectangle.  Critical for
    // scrolling matrices with hundreds of thousands of cells.
    const auto clip = g.getClipBounds();
    const int mStart = juce::jmax (0, clip.getX() / cellSize);
    const int mEnd   = juce::jmin (numOuts, (clip.getRight() + cellSize - 1) / cellSize);
    const int nStart = juce::jmax (0, clip.getY() / cellSize);
    const int nEnd   = juce::jmin (numIns,  (clip.getBottom() + cellSize - 1) / cellSize);

    // Monospaced font for value readout
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, 0));

    for (int m = mStart; m < mEnd; ++m)
    {
        for (int n = nStart; n < nEnd; ++n)
        {
            auto r = cellBounds (m, n).toFloat().reduced (1.0f);
            const float gain = matrix.getCrosspoint (m, n);
            const bool on = gain > 1.0e-6f;

            if (! on)
            {
                // Inactive patchpoint style
                g.setColour (juce::Colour::fromRGB (20, 20, 24));
                g.fillRect (r);

                // Small central patch socket indicator (plus sign / crosshair)
                float cx = r.getCentreX();
                float cy = r.getCentreY();
                g.setColour (juce::Colour::fromRGB (40, 48, 48));
                g.drawLine (cx - 2.0f, cy, cx + 2.0f, cy, 1.0f);
                g.drawLine (cx, cy - 2.0f, cx, cy + 2.0f, 1.0f);
                
                // Dark outer cell frame
                g.setColour (juce::Colour::fromRGB (28, 28, 34));
                g.drawRect (r, 0.5f);
            }
            else
            {
                const float db = linToDb (gain);
                
                // Color scaling: Teal for attenuation (<0 dB), Amber/Orange for boost (>0 dB)
                juce::Colour cellCol;
                if (db <= 0.0f)
                {
                    float factor = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); // -60..0
                    juce::Colour minColor = juce::Colour::fromRGB (0, 45, 50);     // deep teal
                    juce::Colour maxColor = juce::Colour::fromRGB (0, 255, 210);   // neon teal
                    cellCol = minColor.interpolatedWith (maxColor, factor);
                }
                else
                {
                    float factor = juce::jlimit (0.0f, 1.0f, db / 12.0f);          // 0..12
                    juce::Colour minColor = juce::Colour::fromRGB (0, 255, 210);   // neon teal
                    juce::Colour maxColor = juce::Colour::fromRGB (255, 149, 0);   // amber/orange
                    cellCol = minColor.interpolatedWith (maxColor, factor);
                }

                // Active cell background
                g.setColour (cellCol);
                g.fillRect (r);

                // Glow-like active border
                g.setColour (juce::Colours::white.withAlpha (0.4f));
                g.drawRect (r, 1.0f);

                if (cellSize >= 28)   // skip text for very small cells
                {
                    // Draw text in high-contrast: black text on very bright cyan background,
                    // or white text on deep background.
                    const float brightness = cellCol.getBrightness();
                    g.setColour (brightness > 0.6f ? juce::Colours::black : juce::Colours::white);
                    
                    juce::String label = db <= -59.0f ? juce::String ("OFF")
                                                      : juce::String (db, 1);
                    g.drawText (label, r.toNearestInt(), juce::Justification::centred);
                }
            }
        }
    }

    // Hover highlight overlay: custom semi-transparent cyan matching the theme
    if (! highlightedColumns.empty())
    {
        g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.08f));
        for (int m : highlightedColumns)
            if (m >= 0 && m < numOuts)
                g.fillRect (m * cellSize, 0, cellSize, getHeight());
    }
    if (! highlightedRows.empty())
    {
        g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.08f));
        for (int n : highlightedRows)
            if (n >= 0 && n < numIns)
                g.fillRect (0, n * cellSize, getWidth(), cellSize);
    }

    // Draw device separation lines inside the grid (Requirement 2)
    g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.35f)); // glowing neon cyan
    for (int bound : inputDeviceBoundaries)
    {
        float y = (float) bound * (float) cellSize;
        g.drawLine (0.0f, y, (float) getWidth(), y, 1.0f);
    }
    for (int bound : outputDeviceBoundaries)
    {
        float x = (float) bound * (float) cellSize;
        g.drawLine (x, 0.0f, x, (float) getHeight(), 1.0f);
    }

    // Highlight shift-click start cell (Requirement 1 feedback)
    if (shiftStartOut >= 0 && shiftStartOut < numOuts && shiftStartIn >= 0 && shiftStartIn < numIns)
    {
        auto r = cellBounds (shiftStartOut, shiftStartIn).toFloat().reduced (1.0f);
        g.setColour (juce::Colour::fromRGB (0, 255, 210));
        g.drawRect (r, 2.0f);
    }
}

void CrosspointGrid::mouseDown (const juce::MouseEvent& e)
{
    dragOut = dragIn = -1;
    draggedSinceMouseDown = false;
    int m, n;
    if (! hitTestCell (e.x, e.y, m, n)) return;

    if (e.mods.isShiftDown())
    {
        if (shiftStartOut == -1)
        {
            shiftStartOut = m;
            shiftStartIn = n;
            repaint();
        }
        else
        {
            int startOut = shiftStartOut;
            int startIn = shiftStartIn;
            shiftStartOut = -1;
            shiftStartIn = -1;

            auto cells = getCellsOnLine (startOut, startIn, m, n);
            for (auto& cell : cells)
            {
                const float cur = matrix.getCrosspoint (cell.first, cell.second);
                matrix.setCrosspoint (cell.first, cell.second, cur > 1.0e-6f ? 0.0f : 1.0f);
                invalidateCell (cell.first, cell.second);
            }
            repaint();
        }
        return; // Skip normal drag setup
    }
    else
    {
        shiftStartOut = -1;
        shiftStartIn = -1;
        repaint();
    }

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

std::vector<std::pair<int, int>> CrosspointGrid::getCellsOnLine (int x0, int y0, int x1, int y1)
{
    std::vector<std::pair<int, int>> cells;
    int dx = std::abs (x1 - x0);
    int dy = std::abs (y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int x = x0;
    int y = y0;

    while (true)
    {
        cells.push_back ({ x, y });
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy)
        {
            err -= dy;
            x += sx;
        }
        if (e2 < dx)
        {
            err += dx;
            y += sy;
        }
    }
    return cells;
}

} // namespace dcr
