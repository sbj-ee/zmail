#pragma once

#include <QList>

#include <algorithm>

namespace zmail::ui {

// Where the message-list selection goes when rows leave the view (Delete /
// move to Trash): the row below the removed one(s); at the bottom of the
// list, the closest row above; nothing when the list empties. Rows are view
// (proxy) rows, so a sorted list moves in the order the user sees.
struct SelectionAfterRemoval
{
    int rowBefore = -1; // row to select, counted before the removal; -1 = none
    int rowAfter = -1;  // that row's index once the removed rows are gone
};

// removed: view rows being removed (any order; duplicates and out-of-range
// rows are ignored). For a multi-row removal, "below" means below the
// bottom-most removed row.
inline SelectionAfterRemoval selectionAfterRemoval(int rowCount, QList<int> removed)
{
    removed.erase(std::remove_if(removed.begin(), removed.end(),
                                 [rowCount](int r) { return r < 0 || r >= rowCount; }),
                  removed.end());
    std::sort(removed.begin(), removed.end());
    removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
    if (removed.isEmpty()) {
        return {};
    }
    int target = removed.last() + 1; // rows below the bottom-most removed row all survive
    if (target >= rowCount) {
        target = removed.last() - 1;
        while (target >= 0 && std::binary_search(removed.cbegin(), removed.cend(), target)) {
            --target;
        }
    }
    if (target < 0) {
        return {};
    }
    const auto above = std::lower_bound(removed.cbegin(), removed.cend(), target) - removed.cbegin();
    return {target, target - int(above)};
}

} // namespace zmail::ui
