#include "UI/CrosspointGrid.h"

#include "Routing/RoutingMatrix.h"

#include <cmath>

namespace dcr
{

    CrosspointGrid::CrosspointGrid (RoutingMatrix& m) : matrix (m)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setOpaque (true);
    }

    // ---- Hover tracking ----------------------------------------------------
    // Halo extends in 8 directions (full row, full column, both diagonals).
    // Must invalidate every region that has halo pixels in either the OLD
    // or the NEW position, otherwise we leave ghost cells behind that the
    // user perceives as the UI being stuck.  Row + column are easy (cellSize
    // stripes); the diagonals are bounded by a square of side
    // (2*kHaloDiagonalCells + 1) * cellSize around the hovered cell.
    // JUCE coalesces repaints between vsync ticks so even fast mouse motion
    // only triggers ~60 paints/sec.
    void CrosspointGrid::repaintHoverHalo (int outIdx, int inIdx)
    {
        if (outIdx < 0 || inIdx < 0 || cellSize <= 0)
            return;

        // Row stripe (full width).
        repaint (0, inIdx * cellSize, getWidth(), cellSize);
        // Column stripe (full height).
        repaint (outIdx * cellSize, 0, cellSize, getHeight());

        // Bounding square for the diagonals.  See paint() for the matching
        // kHaloDiagonalCells used when actually drawing them.
        const int D = kHaloDiagonalCells;
        const int x0 = juce::jmax (0, (outIdx - D)) * cellSize;
        const int y0 = juce::jmax (0, (inIdx - D)) * cellSize;
        const int x1 = juce::jmin (numOuts, outIdx + D + 1) * cellSize;
        const int y1 = juce::jmin (numIns, inIdx + D + 1) * cellSize;
        repaint (x0, y0, x1 - x0, y1 - y0);
    }

    void CrosspointGrid::mouseMove (const juce::MouseEvent& e)
    {
        const int prevOut = hoverOut;
        const int prevIn = hoverIn;

        int m, n;
        if (hitTestCell (e.x, e.y, m, n))
        {
            if (m == prevOut && n == prevIn)
                return;
            hoverOut = m;
            hoverIn = n;
        }
        else
        {
            if (prevOut < 0 && prevIn < 0)
                return;
            hoverOut = -1;
            hoverIn = -1;
        }

        if (prevOut >= 0)
            repaintHoverHalo (prevOut, prevIn);
        if (hoverOut >= 0)
            repaintHoverHalo (hoverOut, hoverIn);
    }

    void CrosspointGrid::mouseEnter (const juce::MouseEvent& e) { mouseMove (e); }

    void CrosspointGrid::mouseExit (const juce::MouseEvent&)
    {
        if (hoverOut < 0 && hoverIn < 0)
            return;
        const int p = hoverOut, q = hoverIn;
        hoverOut = -1;
        hoverIn = -1;
        if (p >= 0)
            repaintHoverHalo (p, q);
    }

    float CrosspointGrid::dbToLin (float db) noexcept
    {
        if (db <= -60.0f)
            return 0.0f;
        return std::pow (10.0f, db * 0.05f);
    }

    float CrosspointGrid::linToDb (float lin) noexcept
    {
        if (lin <= 1.0e-6f)
            return -60.0f;
        return 20.0f * std::log10 (lin);
    }

    void CrosspointGrid::setDimensions (int newIns, int newOuts, int newCellSize, std::vector<CellSpan> newInSpans, std::vector<CellSpan> newOutSpans)
    {
        numIns = newIns;
        numOuts = newOuts;
        cellSize = newCellSize;
        inSpans = std::move (newInSpans);
        outSpans = std::move (newOutSpans);
        setSize (numOuts * cellSize, numIns * cellSize);
        repaint();
    }

    CrosspointGrid::CellSpan CrosspointGrid::inSpan (int vIn) const noexcept
    {
        if (vIn >= 0 && vIn < (int) inSpans.size())
            return inSpans[(size_t) vIn];
        return { vIn, 1 };
    }

    CrosspointGrid::CellSpan CrosspointGrid::outSpan (int vOut) const noexcept
    {
        if (vOut >= 0 && vOut < (int) outSpans.size())
            return outSpans[(size_t) vOut];
        return { vOut, 1 };
    }

    bool CrosspointGrid::cellIsAggregate (int vOut, int vIn) const noexcept
    {
        return inSpan (vIn).count > 1 || outSpan (vOut).count > 1;
    }

    bool CrosspointGrid::aggregateActive (int vOut, int vIn) const noexcept
    {
        const auto si = inSpan (vIn);
        const auto so = outSpan (vOut);
        // Already in engine-channel space here (firstCh + count).
        for (int m = so.firstCh; m < so.firstCh + so.count; ++m)
            for (int n = si.firstCh; n < si.firstCh + si.count; ++n)
                if (matrix.getCrosspoint (m, n) > 1.0e-6f)
                    return true;
        return false;
    }

    juce::Rectangle<int> CrosspointGrid::cellBounds (int m, int n) const noexcept
    {
        return { m * cellSize, n * cellSize, cellSize, cellSize };
    }

    bool CrosspointGrid::hitTestCell (int x, int y, int& outIdx, int& inIdx) const noexcept
    {
        if (cellSize <= 0)
            return false;
        outIdx = x / cellSize;
        inIdx = y / cellSize;
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
        // Sampled perf log -- log every Nth paint with the clip rect size so we
        // can tell if paint() is being hammered (e.g. by hover-halo invalidation
        // storms) or whether it's stable.
        {
            static thread_local int counter = 0;
            if ((counter++ % 60) == 0)
            {
                const auto cb = g.getClipBounds();
                juce::Logger::writeToLog ("CrosspointGrid::paint #" + juce::String (counter)
                                          + " clip=" + juce::String (cb.getWidth()) + "x"
                                          + juce::String (cb.getHeight()));
            }
        }

        // Background.
        g.fillAll (juce::Colour::fromRGB (12, 12, 14));

        // Only iterate cells inside the dirty clip rectangle.  Critical for
        // scrolling matrices with hundreds of thousands of cells.
        const auto clip = g.getClipBounds();
        const int mStart = juce::jmax (0, clip.getX() / cellSize);
        const int mEnd = juce::jmin (numOuts, (clip.getRight() + cellSize - 1) / cellSize);
        const int nStart = juce::jmax (0, clip.getY() / cellSize);
        const int nEnd = juce::jmin (numIns, (clip.getBottom() + cellSize - 1) / cellSize);

        // Monospaced font for value readout
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.5f, 0));

        for (int m = mStart; m < mEnd; ++m)
        {
            for (int n = nStart; n < nEnd; ++n)
            {
                auto r = cellBounds (m, n).toFloat().reduced (1.0f);

                // Aggregate cell: at least one of the row/column spans covers
                // more than one engine channel.  Render as an aggregate -- a single
                // "device pair" cell, lit if ANY underlying x-point routes,
                // dark otherwise.  No gain readout, no toggle on click (the
                // user expands the device first to route precisely).
                if (cellIsAggregate (m, n))
                {
                    const bool active = aggregateActive (m, n);
                    if (active)
                    {
                        g.setColour (juce::Colour::fromRGB (0, 170, 200)); // muted teal
                        g.fillRect (r);
                        g.setColour (juce::Colours::white.withAlpha (0.35f));
                        g.drawRect (r, 1.0f);
                    }
                    else
                    {
                        g.setColour (juce::Colour::fromRGB (24, 24, 30)); // dim
                        g.fillRect (r);
                        g.setColour (juce::Colour::fromRGB (40, 40, 46));
                        g.drawRect (r, 0.5f);
                    }
                    continue;
                }

                // m / n are VISIBLE indices; once a device is collapsed they
                // diverge from engine channel indices, so we have to translate
                // through the per-row/column span before hitting the matrix.
                const int engM = engOutForCol (m);
                const int engN = engInForRow (n);

                // Blocked (virtual-device self-loop): draw a dim red locked cell
                // with a slash so the user sees it can't be routed.
                if (matrix.isBlocked (engM, engN))
                {
                    g.setColour (juce::Colour::fromRGB (40, 20, 22));
                    g.fillRect (r);
                    g.setColour (juce::Colour::fromRGB (150, 60, 60));
                    g.drawLine (r.getX() + 3.0f, r.getBottom() - 3.0f, r.getRight() - 3.0f, r.getY() + 3.0f, 1.2f); // back-slash
                    g.setColour (juce::Colour::fromRGB (70, 35, 38));
                    g.drawRect (r, 0.5f);
                    continue;
                }

                const float gain = matrix.getCrosspoint (engM, engN);
                const bool on = gain > 1.0e-6f;

                if (!on)
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
                        juce::Colour minColor = juce::Colour::fromRGB (0, 45, 50); // deep teal
                        juce::Colour maxColor = juce::Colour::fromRGB (0, 255, 210); // neon teal
                        cellCol = minColor.interpolatedWith (maxColor, factor);
                    }
                    else
                    {
                        float factor = juce::jlimit (0.0f, 1.0f, db / 12.0f); // 0..12
                        juce::Colour minColor = juce::Colour::fromRGB (0, 255, 210); // neon teal
                        juce::Colour maxColor = juce::Colour::fromRGB (255, 149, 0); // amber/orange
                        cellCol = minColor.interpolatedWith (maxColor, factor);
                    }

                    // Active cell background
                    g.setColour (cellCol);
                    g.fillRect (r);

                    // Glow-like active border
                    g.setColour (juce::Colours::white.withAlpha (0.4f));
                    g.drawRect (r, 1.0f);

                    if (cellSize >= 28) // skip text for very small cells
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
        if (!highlightedColumns.empty())
        {
            g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.08f));
            for (int m : highlightedColumns)
                if (m >= 0 && m < numOuts)
                    g.fillRect (m * cellSize, 0, cellSize, getHeight());
        }
        if (!highlightedRows.empty())
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

        // ---- Hover halo: full cross + both diagonals ------------------------
        // The 8 directions from the hovered cell extend infinitely to the
        // edges of the grid: full ROW (left+right), full COLUMN (up+down),
        // NW-SE diagonal, NE-SW diagonal.  Light grey overlay so all four
        // lines are visible at the same time without becoming dominant.
        // Center cell gets a brighter border to pin down the actual target.
        if (hoverOut >= 0 && hoverIn >= 0)
        {
            const auto halo = juce::Colours::white.withAlpha (0.06f);

            // Full row + full column -- 2 fillRect calls cover all 4 cardinal
            // arms of the cross in one shot each.  Visible-clip restricts the
            // actual GPU work to whatever's on screen.
            g.setColour (halo);
            g.fillRect (0, hoverIn * cellSize, numOuts * cellSize, cellSize);
            g.fillRect (hoverOut * cellSize, 0, cellSize, numIns * cellSize);

            // Two diagonals.  Walk a single offset d in both directions; clip
            // bound stays per-cell.  Cap at kHaloDiagonalCells -- past ~24
            // cells the diagonal is off-screen on any sensible viewport size
            // anyway, and capping keeps repaintHoverHalo's invalidation rect
            // bounded so we don't have to repaint the whole grid on every
            // cursor move.
            const int maxD = juce::jmin (kHaloDiagonalCells,
                juce::jmax (numOuts, numIns));
            for (int d = -maxD; d <= maxD; ++d)
            {
                if (d == 0)
                    continue; // center already covered by row/col
                const int mPlus = hoverOut + d;
                const int nPlus = hoverIn + d;
                const int nMinus = hoverIn - d;
                if (mPlus >= 0 && mPlus < numOuts)
                {
                    if (nPlus >= 0 && nPlus < numIns)
                        g.fillRect (cellBounds (mPlus, nPlus));
                    if (nMinus >= 0 && nMinus < numIns)
                        g.fillRect (cellBounds (mPlus, nMinus));
                }
            }

            // Center cell -- subtle bright border to mark the actual target.
            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.drawRect (cellBounds (hoverOut, hoverIn).toFloat().reduced (1.0f), 1.0f);
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
        if (!hitTestCell (e.x, e.y, m, n))
            return;

        // Aggregate cells (one or both sides is a collapsed device) aren't
        // routable directly.  Fire the click hook so MatrixView can auto-
        // expand both involved devices -- one tap on a lit aggregate cell
        // takes the user straight to the precise crosspoints inside.
        if (cellIsAggregate (m, n))
        {
            if (onAggregateCellClicked)
                onAggregateCellClicked (m, n);
            return;
        }

        // Blocked self-loop crosspoint -- not routable.
        if (matrix.isBlocked (engOutForCol (m), engInForRow (n)))
            return;

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

                // getCellsOnLine returns VISIBLE cell coords; matrix.setCrosspoint
                // wants engine channels.  Translate each before touching the
                // matrix, but keep the original visible coords for invalidate
                // (which addresses on-screen rects, not engine state).
                auto cells = getCellsOnLine (startOut, startIn, m, n);
                for (auto& cell : cells)
                {
                    const int em = engOutForCol (cell.first);
                    const int en = engInForRow (cell.second);
                    const float cur = matrix.getCrosspoint (em, en);
                    matrix.setCrosspoint (em, en, cur > 1.0e-6f ? 0.0f : 1.0f);
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

        dragOut = m;
        dragIn = n;
        const int engM = engOutForCol (m);
        const int engN = engInForRow (n);
        dragStartDb = linToDb (matrix.getCrosspoint (engM, engN));
        if (e.mods.isPopupMenu())
        {
            promptForDb (m, n);
            dragOut = dragIn = -1;
        }
    }

    void CrosspointGrid::mouseDrag (const juce::MouseEvent& e)
    {
        if (dragOut < 0 || e.mods.isPopupMenu())
            return;
        if (std::abs (e.getDistanceFromDragStartY()) < 3 && !draggedSinceMouseDown)
            return;
        draggedSinceMouseDown = true;
        const float newDb = juce::jlimit (-60.0f, 12.0f, dragStartDb + (-e.getDistanceFromDragStartY()) * 0.25f);
        matrix.setCrosspoint (engOutForCol (dragOut), engInForRow (dragIn), dbToLin (newDb));
        invalidateCell (dragOut, dragIn);
    }

    void CrosspointGrid::mouseUp (const juce::MouseEvent& e)
    {
        if (dragOut < 0 || e.mods.isPopupMenu())
        {
            dragOut = dragIn = -1;
            return;
        }
        if (!draggedSinceMouseDown)
        {
            const int em = engOutForCol (dragOut);
            const int en = engInForRow (dragIn);
            const float cur = matrix.getCrosspoint (em, en);
            matrix.setCrosspoint (em, en, cur > 1.0e-6f ? 0.0f : 1.0f);
            invalidateCell (dragOut, dragIn);
        }
        dragOut = dragIn = -1;
    }

    void CrosspointGrid::mouseDoubleClick (const juce::MouseEvent& e)
    {
        int m, n;
        if (!hitTestCell (e.x, e.y, m, n))
            return;
        if (cellIsAggregate (m, n))
            return;
        if (matrix.isBlocked (engOutForCol (m), engInForRow (n)))
            return;
        matrix.setCrosspoint (engOutForCol (m), engInForRow (n), 1.0f);
        invalidateCell (m, n);
    }

    void CrosspointGrid::promptForDb (int m, int n)
    {
        // m / n are visible coords; engine coords are needed for the matrix
        // get/set calls.  invalidateCell stays in visible coord space.
        const int engM = engOutForCol (m);
        const int engN = engInForRow (n);

        auto* dialog = new juce::AlertWindow ("Crosspoint gain",
            "Enter gain in dB (-60 to +12, or 'off')",
            juce::AlertWindow::NoIcon);
        dialog->addTextEditor ("db", juce::String (linToDb (matrix.getCrosspoint (engM, engN)), 1));
        dialog->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        dialog->enterModalState (true,
            juce::ModalCallbackFunction::create ([this, dialog, m, n, engM, engN] (int result) {
                if (result == 1)
                {
                    auto txt = dialog->getTextEditorContents ("db").trim();
                    float gain;
                    if (txt.equalsIgnoreCase ("off") || txt.equalsIgnoreCase ("-inf"))
                        gain = 0.0f;
                    else
                        gain = dbToLin (juce::jlimit (-60.0f, 12.0f, txt.getFloatValue()));
                    matrix.setCrosspoint (engM, engN, gain);
                    invalidateCell (m, n);
                }
                delete dialog;
            }),
            false);
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
            if (x == x1 && y == y1)
                break;
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
