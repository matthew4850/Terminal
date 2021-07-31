// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Terminal.hpp"
#include "unicode.hpp"

#include <winrt/Microsoft.Terminal.Core.h>

using namespace Microsoft::Terminal::Core;
using namespace winrt::Microsoft::Terminal::Core;

/* Selection Pivot Description:
 *   The pivot helps properly update the selection when a user moves a selection over itself
 *   See SelectionTest::DoubleClickDrag_Left for an example of the functionality mentioned here
 *   As an example, consider the following scenario...
 *     1. Perform a word selection (double-click) on a word
 *
 *                  |-position where we double-clicked
 *                 _|_
 *               |word|
 *                |--|
 *  start & pivot-|  |-end
 *
 *     2. Drag your mouse down a line
 *
 *
 *  start & pivot-|__________
 *             __|word_______|
 *            |______|
 *                  |
 *                  |-end & mouse position
 *
 *     3. Drag your mouse up two lines
 *
 *                  |-start & mouse position
 *                  |________
 *             ____|   ______|
 *            |___w|ord
 *                |-end & pivot
 *
 *    The pivot never moves until a new selection is created. It ensures that that cell will always be selected.
 */

// Method Description:
// - Helper to determine the selected region of the buffer. Used for rendering.
// Return Value:
// - A vector of rectangles representing the regions to select, line by line. They are absolute coordinates relative to the buffer origin.
std::vector<SMALL_RECT> Terminal::_GetSelectionRects() const noexcept
{
    std::vector<SMALL_RECT> result;

    if (!IsSelectionActive())
    {
        return result;
    }

    try
    {
        return _buffer->GetTextRects(_selection->start, _selection->end, _blockSelection, false);
    }
    CATCH_LOG();
    return result;
}

// Method Description:
// - Get the current anchor position relative to the whole text buffer
// Arguments:
// - None
// Return Value:
// - None
const COORD Terminal::GetSelectionAnchor() const noexcept
{
    return _selection->start;
}

// Method Description:
// - Get the current end anchor position relative to the whole text buffer
// Arguments:
// - None
// Return Value:
// - None
const COORD Terminal::GetSelectionEnd() const noexcept
{
    return _selection->end;
}

til::point Terminal::SelectionStartForRendering() const
{
    auto pos{ _selection->start };
    _buffer->GetSize().DecrementInBounds(pos);
    return pos;
}

til::point Terminal::SelectionEndForRendering() const
{
    auto pos{ _selection->end };
    _buffer->GetSize().IncrementInBounds(pos);
    return pos;
}

// Method Description:
// - Checks if selection is active
// Return Value:
// - bool representing if selection is active. Used to decide copy/paste on right click
const bool Terminal::IsSelectionActive() const noexcept
{
    return _selection.has_value();
}

const bool Terminal::IsBlockSelection() const noexcept
{
    return _blockSelection;
}

// Method Description:
// - Perform a multi-click selection at viewportPos expanding according to the expansionMode
// Arguments:
// - viewportPos: the (x,y) coordinate on the visible viewport
// - expansionMode: the SelectionExpansion to dictate the boundaries of the selection anchors
void Terminal::MultiClickSelection(const COORD viewportPos, SelectionExpansion expansionMode)
{
    // set the selection pivot to expand the selection using SetSelectionEnd()
    _selection = SelectionAnchors{};
    _selection->pivot = _ConvertToBufferCell(viewportPos);

    _multiClickSelectionMode = expansionMode;
    SetSelectionEnd(viewportPos);

    // we need to set the _selectionPivot again
    // for future shift+clicks
    _selection->pivot = _selection->start;
}

// Method Description:
// - Record the position of the beginning of a selection
// Arguments:
// - position: the (x,y) coordinate on the visible viewport
void Terminal::SetSelectionAnchor(const COORD viewportPos)
{
    _selection = SelectionAnchors{};
    _selection->pivot = _ConvertToBufferCell(viewportPos);

    _multiClickSelectionMode = SelectionExpansion::Cell;
    SetSelectionEnd(viewportPos);

    _selection->start = _selection->pivot;
}

// Method Description:
// - Update selection anchors when dragging to a position
// - based on the selection expansion mode
// Arguments:
// - viewportPos: the (x,y) coordinate on the visible viewport
// - newExpansionMode: overwrites the _multiClickSelectionMode for this function call. Used for ShiftClick
void Terminal::SetSelectionEnd(const COORD viewportPos, std::optional<SelectionExpansion> newExpansionMode)
{
    if (!_selection.has_value())
    {
        // capture a log for spurious endpoint sets without an active selection
        LOG_HR(E_ILLEGAL_STATE_CHANGE);
        return;
    }

    const auto textBufferPos = _ConvertToBufferCell(viewportPos);

    // if this is a shiftClick action, we need to overwrite the _multiClickSelectionMode value (even if it's the same)
    // Otherwise, we may accidentally expand during other selection-based actions
    _multiClickSelectionMode = newExpansionMode.has_value() ? *newExpansionMode : _multiClickSelectionMode;

    std::pair<COORD, COORD> anchors;
    bool targetStart;
    std::tie(anchors.first, anchors.second, targetStart) = _PivotSelection(textBufferPos);
    const auto expandedAnchors = _ExpandSelectionAnchors(anchors);

    if (newExpansionMode.has_value())
    {
        // shift-click operations only expand the target side
        auto& anchorToExpand = targetStart ? _selection->start : _selection->end;
        anchorToExpand = targetStart ? expandedAnchors.first : expandedAnchors.second;

        // the other anchor should then become the pivot (we don't expand it)
        auto& anchorToPivot = targetStart ? _selection->end : _selection->start;
        anchorToPivot = _selection->pivot;
    }
    else
    {
        // expand both anchors
        std::tie(_selection->start, _selection->end) = expandedAnchors;
    }
}

// Method Description:
// - returns a new pair of selection anchors for selecting around the pivot
// - This ensures start < end when compared
// Arguments:
// - targetPos: the (x,y) coordinate we are moving to on the text buffer
// Return Value:
// - COORD: the new start for a selection
// - COORD: the new end for a selection
// - bool: if true, target will be the new start. Otherwise, target will be the new end.
std::tuple<COORD, COORD, bool> Terminal::_PivotSelection(const COORD targetPos) const
{
    const bool targetStart{ _buffer->GetSize().CompareInBounds(targetPos, _selection->pivot) <= 0 };
    if (targetStart)
    {
        // target is before pivot
        // treat target as start
        return { targetPos, _selection->pivot, targetStart };
    }
    else
    {
        // target is after pivot
        // treat pivot as start
        return { _selection->pivot, targetPos, targetStart };
    }
}

// Method Description:
// - Update the selection anchors to expand according to the expansion mode
// Arguments:
// - anchors: a pair of selection anchors representing a desired selection
// Return Value:
// - the new start/end for a selection
std::pair<COORD, COORD> Terminal::_ExpandSelectionAnchors(std::pair<COORD, COORD> anchors) const
{
    COORD start = anchors.first;
    COORD end = anchors.second;

    const auto bufferSize = _buffer->GetSize();
    switch (_multiClickSelectionMode)
    {
    case SelectionExpansion::Line:
        start = { bufferSize.Left(), start.Y };
        end = { bufferSize.RightInclusive(), end.Y };
        break;
    case SelectionExpansion::Word:
        start = _buffer->GetWordStart(start, _wordDelimiters);
        end = _buffer->GetWordEnd(end, _wordDelimiters);
        break;
    case SelectionExpansion::Cell:
    default:
        // no expansion is necessary
        break;
    }
    return std::make_pair(start, end);
}

// Method Description:
// - enable/disable block selection (ALT + selection)
// Arguments:
// - isEnabled: new value for _blockSelection
void Terminal::SetBlockSelection(const bool isEnabled) noexcept
{
    _blockSelection = isEnabled;
}

bool Terminal::MovingStart() const noexcept
{
    // true --> we're moving start endpoint ("higher")
    // false --> we're moving end endpoint ("lower")
    return _selection->start == _selection->pivot ? false : true;
}

void Terminal::UpdateSelection(SelectionDirection direction, SelectionExpansion mode)
{
    // 1. Figure out which endpoint to update
    // One of the endpoints is the pivot, signifying that the other endpoint is the one we want to move.
    auto targetPos{ _selection->start == _selection->pivot ? _selection->end : _selection->start };

    // 2. Perform the movement
    switch (mode)
    {
    case SelectionExpansion::Cell:
        _MoveByChar(direction, targetPos);
        break;
    case SelectionExpansion::Word:
        _MoveByWord(direction, targetPos);
        break;
    case SelectionExpansion::Viewport:
        _MoveByViewport(direction, targetPos);
        break;
    case SelectionExpansion::Buffer:
        _MoveByBuffer(direction, targetPos);
        break;
    }

    // 3. Actually modify the selection
    std::tie(_selection->start, _selection->end, std::ignore) = _PivotSelection(targetPos);

    // 4. Scroll (if necessary)
    if (const auto viewport = _GetVisibleViewport(); !viewport.IsInBounds(targetPos))
    {
        if (const auto amtAboveView = viewport.Top() - targetPos.Y; amtAboveView > 0)
        {
            // anchor is above visible viewport, scroll by that amount
            _scrollOffset += amtAboveView;
        }
        else
        {
            // anchor is below visible viewport, scroll by that amount
            const auto amtBelowView = targetPos.Y - viewport.BottomInclusive();
            _scrollOffset -= amtBelowView;
        }
        _NotifyScrollEvent();
    }
}

void Terminal::_MoveByChar(SelectionDirection direction, COORD& pos)
{
    switch (direction)
    {
    case SelectionDirection::Left:
        _buffer->GetSize().DecrementInBounds(pos);
        pos = _buffer->GetGlyphStart(pos);
        break;
    case SelectionDirection::Right:
        _buffer->GetSize().IncrementInBounds(pos);
        pos = _buffer->GetGlyphEnd(pos);
        break;
    case SelectionDirection::Up:
    {
        const auto bufferSize{ _buffer->GetSize() };
        pos = { pos.X, std::clamp(base::ClampSub<short, short>(pos.Y, 1).RawValue(), bufferSize.Top(), bufferSize.BottomInclusive()) };
        break;
    }
    case SelectionDirection::Down:
    {
        const auto bufferSize{ _buffer->GetSize() };
        pos = { pos.X, std::clamp(base::ClampAdd<short, short>(pos.Y, 1).RawValue(), bufferSize.Top(), bufferSize.BottomInclusive()) };
        break;
    }
    }
}

void Terminal::_MoveByWord(SelectionDirection direction, COORD& pos)
{
    switch (direction)
    {
    case SelectionDirection::Left:
        const auto wordStartPos{ _buffer->GetWordStart(pos, _wordDelimiters) };
        if (_buffer->GetSize().CompareInBounds(_selection->pivot, pos) < 0)
        {
            // If we're moving towards the pivot, move one more cell
            pos = wordStartPos;
            _buffer->GetSize().DecrementInBounds(pos);
        }
        else if (wordStartPos == pos)
        {
            // already at the beginning of the current word,
            // move to the beginning of the previous word
            _buffer->GetSize().DecrementInBounds(pos);
            pos = _buffer->GetWordStart(pos, _wordDelimiters);
        }
        else
        {
            // move to the beginning of the current word
            pos = wordStartPos;
        }
        break;
    case SelectionDirection::Right:
        const auto wordEndPos{ _buffer->GetWordEnd(pos, _wordDelimiters) };
        if (_buffer->GetSize().CompareInBounds(pos, _selection->pivot) < 0)
        {
            // If we're moving towards the pivot, move one more cell
            pos = _buffer->GetWordEnd(pos, _wordDelimiters);
            _buffer->GetSize().IncrementInBounds(pos);
        }
        else if (wordEndPos == pos)
        {
            // already at the end of the current word,
            // move to the end of the next word
            _buffer->GetSize().IncrementInBounds(pos);
            pos = _buffer->GetWordEnd(pos, _wordDelimiters);
        }
        else
        {
            // move to the end of the current word
            pos = wordEndPos;
        }
        break;
    case SelectionDirection::Up:
        _MoveByChar(direction, pos);
        pos = _buffer->GetWordStart(pos, _wordDelimiters);
        break;
    case SelectionDirection::Down:
        _MoveByChar(direction, pos);
        pos = _buffer->GetWordEnd(pos, _wordDelimiters);
        break;
    }
}

void Terminal::_MoveByViewport(SelectionDirection direction, COORD& pos)
{
    const auto bufferSize{ _buffer->GetSize() };
    switch (direction)
    {
    case SelectionDirection::Left:
        pos = { bufferSize.Left(), pos.Y };
        break;
    case SelectionDirection::Right:
        pos = { bufferSize.RightInclusive(), pos.Y };
        break;
    case SelectionDirection::Up:
    {
        const auto viewportHeight{ _mutableViewport.Height() };
        const auto newY{ base::ClampSub<short, short>(pos.Y, viewportHeight) };
        pos = newY < bufferSize.Top() ? bufferSize.Origin() : COORD{ pos.X, newY };
        break;
    }
    case SelectionDirection::Down:
    {
        const auto viewportHeight{ _mutableViewport.Height() };
        const auto newY{ base::ClampAdd<short, short>(pos.Y, viewportHeight) };
        pos = newY > bufferSize.BottomInclusive() ? COORD{ bufferSize.RightInclusive(), bufferSize.BottomInclusive() } : COORD{ pos.X, newY };
        break;
    }
    }
}

void Terminal::_MoveByBuffer(SelectionDirection direction, COORD& pos)
{
    const auto bufferSize{ _buffer->GetSize() };
    switch (direction)
    {
    case SelectionDirection::Left:
    case SelectionDirection::Up:
        pos = bufferSize.Origin();
        break;
    case SelectionDirection::Right:
    case SelectionDirection::Down:
        pos = { bufferSize.RightInclusive(), bufferSize.BottomInclusive() };
        break;
    }
}

// Method Description:
// - clear selection data and disable rendering it
#pragma warning(disable : 26440) // changing this to noexcept would require a change to ConHost's selection model
void Terminal::ClearSelection()
{
    _selection = std::nullopt;
}

// Method Description:
// - get wstring text from highlighted portion of text buffer
// Arguments:
// - singleLine: collapse all of the text to one line
// Return Value:
// - wstring text from buffer. If extended to multiple lines, each line is separated by \r\n
const TextBuffer::TextAndColor Terminal::RetrieveSelectedTextFromBuffer(bool singleLine)
{
    auto lock = LockForReading();

    const auto selectionRects = _GetSelectionRects();

    const auto GetAttributeColors = std::bind(&Terminal::GetAttributeColors, this, std::placeholders::_1);

    // GH#6740: Block selection should preserve the visual structure:
    // - CRLFs need to be added - so the lines structure is preserved
    // - We should apply formatting above to wrapped rows as well (newline should be added).
    // GH#9706: Trimming of trailing white-spaces in block selection is configurable.
    const auto includeCRLF = !singleLine || _blockSelection;
    const auto trimTrailingWhitespace = !singleLine && (!_blockSelection || _trimBlockSelection);
    const auto formatWrappedRows = _blockSelection;
    return _buffer->GetText(includeCRLF, trimTrailingWhitespace, selectionRects, GetAttributeColors, formatWrappedRows);
}

// Method Description:
// - convert viewport position to the corresponding location on the buffer
// Arguments:
// - viewportPos: a coordinate on the viewport
// Return Value:
// - the corresponding location on the buffer
COORD Terminal::_ConvertToBufferCell(const COORD viewportPos) const
{
    const auto yPos = base::ClampedNumeric<short>(_VisibleStartIndex()) + viewportPos.Y;
    COORD bufferPos = { viewportPos.X, yPos };
    _buffer->GetSize().Clamp(bufferPos);
    return bufferPos;
}

// Method Description:
// - This method won't be used. We just throw and do nothing. For now we
//   need this method to implement UiaData interface
// Arguments:
// - coordSelectionStart - Not used
// - coordSelectionEnd - Not used
// - attr - Not used.
void Terminal::ColorSelection(const COORD, const COORD, const TextAttribute)
{
    THROW_HR(E_NOTIMPL);
}
