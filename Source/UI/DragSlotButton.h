#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace dcr
{

    // Small TextButton subclass used as the draggable + droppable "slot name"
    // widget in plugin-chain UIs (per-channel FX popup, per-group Card).
    //
    // Drag starts after a 5 px move so a normal click still fires the button's
    // onClick.  The drag description is the source slotIdx; on drop the target's
    // onSwap callback receives (fromSlot, toSlot).  Source and target must share
    // the same DragAndDropContainer ancestor (the popup or Card itself).
    class DragSlotButton : public juce::TextButton,
                           public juce::DragAndDropTarget
    {
    public:
        int slotIdx = 0;
        std::function<void (int /*from*/, int /*to*/)> onSwap;

        void mouseDrag (const juce::MouseEvent& e) override
        {
            // Only start a drag if there's actually a plugin to move and the
            // mouse has moved enough to clearly distinguish from a click.
            if (!isEnabled() || e.getDistanceFromDragStart() < 5)
            {
                juce::TextButton::mouseDrag (e);
                return;
            }
            if (auto* ddc = juce::DragAndDropContainer::findParentDragContainerFor (this))
                if (!ddc->isDragAndDropActive())
                    ddc->startDragging (juce::var (slotIdx), this);
        }

        bool isInterestedInDragSource (const SourceDetails& s) override
        {
            return dynamic_cast<DragSlotButton*> (s.sourceComponent.get()) != nullptr;
        }

        void itemDragEnter (const SourceDetails&) override
        {
            dropHover = true;
            repaint();
        }
        void itemDragExit (const SourceDetails&) override
        {
            dropHover = false;
            repaint();
        }

        void itemDropped (const SourceDetails& s) override
        {
            dropHover = false;
            repaint();
            auto* src = dynamic_cast<DragSlotButton*> (s.sourceComponent.get());
            if (src != nullptr && src != this && onSwap)
                onSwap (src->slotIdx, slotIdx);
        }

        void paintOverChildren (juce::Graphics& g) override
        {
            if (dropHover)
            {
                g.setColour (juce::Colour::fromRGB (0, 200, 220).withAlpha (0.85f));
                g.drawRect (getLocalBounds(), 2);
            }
        }

    private:
        bool dropHover = false;
    };

} // namespace dcr
