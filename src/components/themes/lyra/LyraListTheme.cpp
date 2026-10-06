#include "LyraListTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "ReadingStatsStore.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"
#include "fontIds.h"
#include "util/ReadingStatsAnalytics.h"

namespace {
constexpr int kCoverTextGap = 16;
constexpr int kSelectionPadding = 8;
constexpr int kCornerRadius = 6;
constexpr int kTitleTopOffset = 4;
constexpr int kLineGap = 6;
constexpr int kPercentGap = 12;
constexpr int kPlaceholderIconSize = 32;

int rowTop(const Rect& tile, const int index) { return tile.y + LyraListMetrics::rowHeight * index; }

// Time already spent, time still to go, and how far in. Matching by path alone
// misses books that moved on the card, which is what the title/author fallback
// inside the store is for.
//
// The estimate extrapolates from this reader's own pace on this book: the
// elapsed time covers `percent`, so the whole book costs elapsed/percent. It
// needs a few percent on the clock before it says anything, or an early
// session would promise a forty-hour novel.
std::string readingSummaryOf(const RecentBook& book) {
  const ReadingBookStats* stats = READING_STATS.findMatchingBookForPath(book.path, book.title, book.author);
  if (stats == nullptr) return {};

  const uint8_t percent = std::min<uint8_t>(stats->lastProgressPercent, 100);
  std::string summary = ReadingStatsAnalytics::formatDurationHm(stats->totalReadingMs);

  constexpr uint8_t kMinPercentForEstimate = 3;
  if (percent >= kMinPercentForEstimate && percent < 100 && stats->totalReadingMs > 0) {
    const uint64_t wholeBookMs = stats->totalReadingMs * 100ULL / percent;
    summary += " / Est. " + ReadingStatsAnalytics::formatDurationHm(wholeBookMs - stats->totalReadingMs);
  }

  char tail[8];
  snprintf(tail, sizeof(tail), " / %u%%", static_cast<unsigned>(percent));
  return summary + tail;
}
}  // namespace

int LyraListTheme::homeCoverThumbHeight(const GfxRenderer&) const { return LyraListMetrics::coverHeight; }

int LyraListTheme::recentBookIndexAtPoint(const int x, const int y, const Rect tile) const {
  (void)x;
  const int index = (y - tile.y) / LyraListMetrics::rowHeight;
  return std::clamp(index, 0, LyraListMetrics::rowCount - 1);
}

void LyraListTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                        const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                        bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)bufferRestored;
  if (recentBooks.empty()) {
    drawEmptyRecents(renderer, rect);
    return;
  }

  const int count = std::min(static_cast<int>(recentBooks.size()), LyraListMetrics::rowCount);
  const int sidePadding = LyraListMetrics::values.contentSidePadding;
  const int textX = sidePadding + LyraListMetrics::coverWidth + kCoverTextGap;
  const int textWidth = std::max(0, rect.width - textX - sidePadding);

  // Covers come off the SD card once per screen; every later render paints the
  // text over the restored snapshot of this band.
  if (!coverRendered) {
    for (int i = 0; i < count; i++) {
      const Rect slot{sidePadding, rowTop(rect, i) + LyraListMetrics::rowVPadding, LyraListMetrics::coverWidth,
                      LyraListMetrics::coverHeight};
      bool hasCover = false;
      if (!recentBooks[i].coverBmpPath.empty()) {
        const std::string coverBmpPath =
            UITheme::getCoverThumbPath(recentBooks[i].coverBmpPath, homeCoverThumbHeight(renderer));
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            hasCover = drawCoverThumbFill(renderer, bitmap, slot);
          }
          file.close();
        }
      }

      renderer.drawRect(slot.x, slot.y, slot.width, slot.height, true);
      if (!hasCover) {
        renderer.drawIcon(CoverIcon, slot.x + (slot.width - kPlaceholderIconSize) / 2,
                          slot.y + (slot.height - kPlaceholderIconSize) / 2, kPlaceholderIconSize);
      }
    }

    coverBufferStored = storeCoverBuffer();
    coverRendered = coverBufferStored;
  }

  for (int i = 0; i < count; i++) {
    const int top = rowTop(rect, i) + LyraListMetrics::rowVPadding;

    // Selection wraps the text column only: boxing the cover as well would put
    // a second frame around the one the cover already draws.
    if (selectorIndex == i) {
      renderer.fillRoundedRect(textX - kSelectionPadding, top, textWidth + 2 * kSelectionPadding,
                               LyraListMetrics::coverHeight, kCornerRadius, Color::LightGray);
    }

    const int titleY = top + kTitleTopOffset;
    const std::string title = renderer.truncatedText(UI_12_FONT_ID, recentBooks[i].title.c_str(), textWidth);
    renderer.drawText(UI_12_FONT_ID, textX, titleY, title.c_str(), true);

    // The numbers are the point of this line, so the author gives up width
    // first: it is the one part the reader already knows by heart.
    const std::string summary = readingSummaryOf(recentBooks[i]);
    const std::string separator = summary.empty() ? "" : " - ";
    const int summaryWidth = summary.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, summary.c_str());
    const int separatorWidth = summary.empty() ? 0 : renderer.getTextWidth(SMALL_FONT_ID, separator.c_str());

    const int authorY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + kLineGap;
    const int authorWidth = std::max(0, textWidth - summaryWidth - separatorWidth);
    const std::string author = renderer.truncatedText(SMALL_FONT_ID, recentBooks[i].author.c_str(), authorWidth);

    std::string secondLine = author;
    if (!summary.empty()) {
      if (!secondLine.empty()) secondLine += separator;
      secondLine += summary;
    }
    if (!secondLine.empty()) {
      renderer.drawText(SMALL_FONT_ID, textX, authorY, secondLine.c_str(), true);
    }
  }
}
