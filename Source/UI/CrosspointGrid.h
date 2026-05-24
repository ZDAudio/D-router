#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr {

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

    // Reconfigure with the engine's current channel layout. Call after engine
    // (re)start.
    void setDimensions (int numIns, int numOuts, int cellSize);

    int getCellSize() const noexcept { return cellSize; }
    int getNumIns()   const noexcept { return numIns;   }
    int getNumOuts()  const noexcept { return numOuts;  }

    // Repaint a single cell (used after external state change, e.g. snapshot load).
    void invalidateCell (int outIdx, int inIdx);

    // Highlight the listed output columns (translucent overlay) - used to
    // visually link group cards in the bottom panel to their member output
    // channels in the matrix.
    void setHighlightedColumns (std::vector<int> cols);

    // Same idea for INPUT rows (input groups hover -> rows highlight).
    void setHighlightedRows    (std::vector<int> rows);

    // Set boundaries between different devices to draw visual separators across the grid
    void setDeviceBoundaries (std::vector<int> ins, std::vector<int> outs);

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> cellBounds (int outIdx, int inIdx) const noexcept;
    bool hitTestCell (int x, int y, int& outIdx, int& inIdx) const noexcept;
    void promptForDb (int outIdx, int inIdx);
    std::vector<std::pair<int, int>> getCellsOnLine (int x0, int y0, int x1, int y1);

    static float dbToLin (float db) noexcept;
    static float linToDb (float lin) noexcept;

    RoutingMatrix& matrix;
    int numIns   = 0;
    int numOuts  = 0;
    int cellSize = 36;

    // Drag state
    int  dragOut = -1, dragIn = -1;
    float dragStartDb = 0.0f;
    bool  draggedSinceMouseDown = false;

    // Shift click state
    int shiftStartOut = -1;
    int shiftStartIn  = -1;

    std::vector<int> highlightedColumns;
    std::vector<int> highlightedRows;
    std::vector<int> inputDeviceBoundaries;
    std::vector<int> outputDeviceBoundaries;
};

} // namespace dcr
