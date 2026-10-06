#include "LibraryScanActivity.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryFormat.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCoverLoader.h"

namespace {
constexpr int kBarHeight = 12;
constexpr int kRowGap = 10;

void drawProgressBar(const GfxRenderer& renderer, const Rect& bar, const int percent) {
  renderer.drawRect(bar.x, bar.y, bar.width, bar.height, true);
  const int fill = std::clamp(percent, 0, 100) * (bar.width - 4) / 100;
  if (fill > 0) renderer.fillRect(bar.x + 2, bar.y + 2, fill, bar.height - 4, true);
}
}  // namespace

void LibraryScanActivity::onEnter() {
  Activity::onEnter();
  const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
  thumbHeight = themeThumbHeight > 0 ? themeThumbHeight : UITheme::getInstance().getMetrics().homeCoverHeight;
  requestUpdate();
}

void LibraryScanActivity::onExit() {
  index.close();
  Activity::onExit();
}

void LibraryScanActivity::startScan() {
  phase = Phase::Indexing;
  startedMs = millis();
  indexFailed = false;
  cancelled = false;
  processed = 0;
  coversReady = 0;
  coversCreated = 0;
  coversMissing = 0;
  // Paint "indexing" before the walk blocks the loop, or the screen would sit
  // on the intro until the card has been enumerated.
  requestUpdateAndWait();
}

void LibraryScanActivity::runIndexPass() {
  library::BuildStats stats;
  // Metadata on purpose, whatever the Library screen is configured to do: a
  // scan the user asked for is the moment to pay for real titles and authors.
  if (!library::buildLibraryIndex("/", stats, true)) {
    LOG_ERR("SCAN", "index build failed");
    indexFailed = true;
    phase = Phase::Done;
    elapsedMs = millis() - startedMs;
    requestUpdate();
    return;
  }

  LOG_INF("SCAN", "indexed %u books (%u added, %u enriched)", static_cast<unsigned>(stats.books),
          static_cast<unsigned>(stats.added), static_cast<unsigned>(stats.enriched));

  index.close();
  if (!index.open(library::libraryIndexPath())) {
    LOG_ERR("SCAN", "cannot open library index after build");
    indexFailed = true;
    phase = Phase::Done;
    elapsedMs = millis() - startedMs;
    requestUpdate();
    return;
  }

  bookCount = index.bookCount();
  phase = bookCount > 0 ? Phase::Covers : Phase::Done;
  if (phase == Phase::Done) elapsedMs = millis() - startedMs;
  requestUpdate();
}

void LibraryScanActivity::processNextBook() {
  if (processed >= bookCount) {
    phase = Phase::Done;
    elapsedMs = millis() - startedMs;
    index.close();
    requestUpdate();
    return;
  }

  bool generated = false;
  library::ClixRecord record{};
  std::string path;
  if (index.readRecord(processed, record) && index.readPath(record, path)) {
    std::string cover;
    {
      // Inflating a cover needs the framebuffer; the loan hands it over and
      // takes it back, and the repaint below restores what the screen showed.
      GfxRenderer::FrameBufferLoan loan(renderer);
      cover = BookCoverLoader::ensureThumbnail(path, thumbHeight, &generated);
    }
    if (cover.empty()) {
      ++coversMissing;
    } else if (generated) {
      ++coversCreated;
    } else {
      ++coversReady;
    }
  } else {
    LOG_ERR("SCAN", "unreadable index record %u", static_cast<unsigned>(processed));
    ++coversMissing;
  }

  ++processed;
}

void LibraryScanActivity::loop() {
  switch (phase) {
    case Phase::Ready:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        activityManager.goToApps();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) startScan();
      return;

    case Phase::Indexing:
      runIndexPass();
      return;

    case Phase::Covers:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        cancelled = true;
        phase = Phase::Done;
        elapsedMs = millis() - startedMs;
        index.close();
        requestUpdate();
      }
      // The work itself happens in render(), which holds the render lock.
      return;

    case Phase::Done:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        activityManager.goToApps();
      }
      return;
  }
}

void LibraryScanActivity::drawReady(const Rect& content) {
  const auto lines = renderer.wrappedText(UI_12_FONT_ID, tr(STR_LIBRARY_SCAN_INTRO), content.width, 4);
  int y = content.y;
  for (const auto& line : lines) {
    renderer.drawText(UI_12_FONT_ID, content.x, y, line.c_str(), true);
    y += renderer.getLineHeight(UI_12_FONT_ID);
  }
}

void LibraryScanActivity::drawProgress(const Rect& content) {
  const bool indexing = phase == Phase::Indexing;
  renderer.drawText(
      UI_12_FONT_ID, content.x, content.y,
      I18n::getInstance().get(indexing ? StrId::STR_LIBRARY_SCAN_INDEXING : StrId::STR_LIBRARY_SCAN_COVERS), true);
  if (indexing) return;

  const int percent = bookCount > 0 ? processed * 100 / bookCount : 0;
  const int barY = content.y + renderer.getLineHeight(UI_12_FONT_ID) + kRowGap;
  drawProgressBar(renderer, Rect{content.x, barY, content.width, kBarHeight}, percent);

  char counter[32];
  snprintf(counter, sizeof(counter), "%u / %u", static_cast<unsigned>(processed), static_cast<unsigned>(bookCount));
  renderer.drawText(SMALL_FONT_ID, content.x, barY + kBarHeight + kRowGap, counter, true);
}

void LibraryScanActivity::drawSummary(const Rect& content) {
  const StrId headline = indexFailed ? StrId::STR_LIBRARY_SCAN_FAILED
                         : cancelled ? StrId::STR_LIBRARY_SCAN_STOPPED
                                     : StrId::STR_LIBRARY_SCAN_DONE;
  renderer.drawText(UI_12_FONT_ID, content.x, content.y, I18n::getInstance().get(headline), true);
  if (indexFailed) return;

  int y = content.y + renderer.getLineHeight(UI_12_FONT_ID) + kRowGap;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  const struct {
    StrId label;
    unsigned value;
  } rows[] = {
      {StrId::STR_LIBRARY_SCAN_BOOKS, static_cast<unsigned>(processed)},
      {StrId::STR_LIBRARY_SCAN_COVERS_NEW, static_cast<unsigned>(coversCreated)},
      {StrId::STR_LIBRARY_SCAN_COVERS_CACHED, static_cast<unsigned>(coversReady)},
      {StrId::STR_LIBRARY_SCAN_COVERS_NONE, static_cast<unsigned>(coversMissing)},
  };
  for (const auto& row : rows) {
    char value[12];
    snprintf(value, sizeof(value), "%u", row.value);
    renderer.drawText(SMALL_FONT_ID, content.x, y, I18n::getInstance().get(row.label), true);
    renderer.drawText(SMALL_FONT_ID, content.x + content.width - renderer.getTextWidth(SMALL_FONT_ID, value), y, value,
                      true);
    y += lineHeight;
  }

  char elapsed[32];
  snprintf(elapsed, sizeof(elapsed), "%lus", static_cast<unsigned long>(elapsedMs / 1000));
  renderer.drawText(SMALL_FONT_ID, content.x, y + kRowGap, elapsed, true);
}

void LibraryScanActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentTop = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const Rect content{safe.x + metrics.contentSidePadding, contentTop,
                     std::max(1, safe.width - metrics.contentSidePadding * 2),
                     std::max(1, safe.y + safe.height - contentTop - metrics.verticalSpacing)};

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight},
                 tr(STR_LIBRARY_SCAN_TITLE));

  switch (phase) {
    case Phase::Ready:
      drawReady(content);
      break;
    case Phase::Indexing:
    case Phase::Covers:
      drawProgress(content);
      break;
    case Phase::Done:
      drawSummary(content);
      break;
  }

  const char* confirmLabel = phase == Phase::Ready ? tr(STR_LIBRARY_SCAN_START) : nullptr;
  const char* backLabel = phase == Phase::Covers ? tr(STR_CANCEL) : tr(STR_BACK);
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, confirmLabel, confirmLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // One book per painted frame. This runs while the caller's RenderLock is
  // still held, which is what makes borrowing the framebuffer for the cover
  // decoder safe; doing it from loop() races the render task.
  if (phase == Phase::Covers) {
    processNextBook();
    requestUpdate();
  }
}
