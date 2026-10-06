#pragma once

#include "components/themes/lyra/LyraTheme.h"

class GfxRenderer;

// Lyra List: the recent books as a vertical list instead of side-by-side
// covers. One row per book — cover on the left, title and author stacked next
// to it, read percentage flush right on the author's line.
//
// The row grid is derived from the tile so the draw and the thumbnail
// generator cannot disagree on cover height: homeCoverTileHeight is split
// evenly between homeRecentBooksCount rows, and the cover is that row minus
// its vertical padding.
namespace LyraListMetrics {
inline constexpr int rowCount = 4;
// What is left of the 800 px panel once the home header (56), the gap above
// the menu (16) and the menu itself (6 rows x 72) are paid for. Four rows is
// what the reader asked for, so the rows give way, not the count.
inline constexpr int tileHeight = 296;
inline constexpr int rowHeight = tileHeight / rowCount;
inline constexpr int rowVPadding = 6;
inline constexpr int coverHeight = rowHeight - 2 * rowVPadding;
// Covers are taller than wide; 2:3 is the common trade paperback ratio and the
// slot crops rather than stretches whatever the book actually ships.
inline constexpr int coverWidth = coverHeight * 2 / 3;

constexpr int statusBandHeight = 30;

constexpr ThemeMetrics values = [] {
  ThemeMetrics v = LyraMetrics::values;
  // Room for two bands: clock and battery on top, the greeting under it.
  v.homeTopPadding = LyraMetrics::values.homeTopPadding + statusBandHeight;
  // The clock lives in the status band this theme draws itself.
  v.headerShowsClock = false;
  v.homeCoverTileHeight = tileHeight;
  v.homeCoverHeight = coverHeight;
  v.homeRecentBooksCount = rowCount;
  return v;
}();
}  // namespace LyraListMetrics

class LyraListTheme : public LyraTheme {
 public:
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           const int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           std::function<bool()> storeCoverBuffer) const override;
  int recentBookIndexAtPoint(int x, int y, Rect tile) const override;
  int homeCoverThumbHeight(const GfxRenderer& renderer) const override;
};
