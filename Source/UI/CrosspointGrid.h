#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr
{

    class RoutingMatrix;

    // A single Component that paints the entire crosspoint grid and dispatches
    // mouse events to individual cells.  Replaces the prior per-cell Crosspoint
    // approach so the UI scales to N*M = O(100k) cells without spawning that
    // many juce::Component instances.
    //
    //   Click       = toggle on/off (at unity / zero).
    //   Vertical    = drag-adjust gain (-60 .. +12 dB).
    //   Double clk  = reset to 0 dB.
    //   Right click = prompt for numeric dB value.
    class CrosspointGrid : public juce::Component
    {
    public:
        explicit CrosspointGrid (RoutingMatrix& matrix);

        // Each visible row or column may correspond to a RANGE of engine
        // channels (when a device is collapsed in the matrix view).  count==1
        // is the normal "expanded" case; count>1 makes the cell render as a
        // device-level aggregate (lit iff any underlying engine crosspoint
        // has gain > 0), and clicks on aggregate cells are no-ops.
        struct CellSpan
        {
            int firstCh = 0;
            int count = 1;
        };

        // Reconfigure with the engine's current channel layout. Call after engine
        // (re)start.  If spans aren't supplied, every visible row/column is a
        // 1:1 normal channel.
        void setDimensions (int numIns, int numOuts, int cellSize, std::vector<CellSpan> inSpans = {}, std::vector<CellSpan> outSpans = {});

        int getCellSize() const noexcept { return cellSize; }
        int getNumIns() const noexcept { return numIns; }
        int getNumOuts() const noexcept { return numOuts; }

        // Repaint a single cell (used after external state change, e.g. snapshot load).
        void invalidateCell (int outIdx, int inIdx);

        // Highlight the listed output columns (translucent overlay) - used to
        // visually link group cards in the bottom panel to their member output
        // channels in the matrix.
        void setHighlightedColumns (std::vector<int> cols);

        // Same idea for INPUT rows (input groups hover -> rows highlight).
        void setHighlightedRows (std::vector<int> rows);

        // Set boundaries between different devices to draw visual separators across the grid
        void setDeviceBoundaries (std::vector<int> ins, std::vector<int> outs);

        // Fired when the user clicks an "aggregate" cell (any cell where at
        // least one side is a collapsed device).  MatrixView wires this to
        // expand both involved devices so the user can route precisely.
        // Arguments are visible row / column indices into the label list.
        std::function<void (int outIdx, int inIdx)> onAggregateCellClicked;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        // Hover tracking: paints a 3x3 light-grey halo around the cell the
        // mouse is over, so the user can see context at a glance (which
        // input/output channels are next to the one they're aiming at).
        void mouseMove (const juce::MouseEvent&) override;
        void mouseEnter (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;

    private:
        juce::Rectangle<int> cellBounds (int outIdx, int inIdx) const noexcept;
        bool hitTestCell (int x, int y, int& outIdx, int& inIdx) const noexcept;
        void promptForDb (int outIdx, int inIdx);
        std::vector<std::pair<int, int>> getCellsOnLine (int x0, int y0, int x1, int y1);

        static float dbToLin (float db) noexcept;
        static float linToDb (float lin) noexcept;

        RoutingMatrix& matrix;
        int numIns = 0;
        int numOuts = 0;
        int cellSize = 36;

        // Per visible row / column, which range of engine channels does it
        // represent.  When a span has count == 1 the cell is a normal
        // crosspoint; when count > 1 it's a "device aggregate" cell whose
        // visible state is the OR-reduce of every underlying (firstCh..firstCh+count).
        std::vector<CellSpan> inSpans;
        std::vector<CellSpan> outSpans;

        // Helpers -- safe even when spans are empty (treats every row/col as 1:1).
        CellSpan inSpan (int vIn) const noexcept;
        CellSpan outSpan (int vOut) const noexcept;
        bool cellIsAggregate (int vOut, int vIn) const noexcept;
        bool aggregateActive (int vOut, int vIn) const noexcept;

        // Translate visible row/column to engine channel.  CRITICAL: visible
        // index != engine index once any device is collapsed -- using `m` as
        // the engine channel in matrix.get/setCrosspoint shifts every route
        // visually rightward / downward past the fold point (the actual
        // engine state stays correct, but the wrong cell lights up).
        int engOutForCol (int vOut) const noexcept { return outSpan (vOut).firstCh; }
        int engInForRow (int vIn) const noexcept { return inSpan (vIn).firstCh; }

        // Drag state
        int dragOut = -1, dragIn = -1;
        float dragStartDb = 0.0f;
        bool draggedSinceMouseDown = false;

        // Shift click state
        int shiftStartOut = -1;
        int shiftStartIn = -1;

        std::vector<int> highlightedColumns;
        std::vector<int> highlightedRows;
        std::vector<int> inputDeviceBoundaries;
        std::vector<int> outputDeviceBoundaries;

        // Hover cell (-1 == no hover).  When this changes we invalidate the
        // row stripe, column stripe and a (2D+1)-cell diagonal bounding box
        // around the hovered cell at BOTH the old and new positions, so the
        // halo cleanly clears as the mouse moves and doesn't leave ghost
        // grey cells behind.
        int hoverOut = -1, hoverIn = -1;
        void repaintHoverHalo (int outIdx, int inIdx);

        // Max diagonal extent (in cells) drawn from the hovered cell.  Looks
        // essentially "infinite" inside any reasonable viewport while keeping
        // the per-hover invalidation rect bounded -- otherwise a 48x66 grid
        // would invalidate the whole canvas every time the cursor crossed a
        // cell boundary, and the visible ghost cells past the cap looked like
        // the UI was running at 1-3 fps.
        static constexpr int kHaloDiagonalCells = 24;
    };

} // namespace dcr
