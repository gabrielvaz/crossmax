#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/plugins/PluginCatalogActivity.h"  // anyPluginInstalled()
#include "components/UITheme.h"
#include "components/themes/lyra/LyraListTheme.h"
#include "fontIds.h"
#include "util/BookCoverLoader.h"
#include "util/TimeUtils.h"

namespace {
struct HomeMenuEntry {
  HomeMenuItem item;
  StrId label;
  UIIcon icon;
};

// Four rows, so the menu still fits under the recent-books tile on the 800px
// panel. File transfer and the plugin catalog moved into Apps, which already
// held the OPDS browser: they are destinations you visit occasionally, not
// every time you pick up the reader.
constexpr HomeMenuEntry kDefaultMenuOrder[] = {
    {HomeMenuItem::LIBRARY, StrId::STR_LIBRARY, Library},
    {HomeMenuItem::FILE_BROWSER, StrId::STR_BROWSE_FILES, Folder},
    {HomeMenuItem::APPS, StrId::STR_APPS_TITLE, Apps},
    {HomeMenuItem::SETTINGS_MENU, StrId::STR_SETTINGS_TITLE, Settings},
};
constexpr HomeMenuEntry kCarouselMenuOrder[] = {
    {HomeMenuItem::FILE_BROWSER, StrId::STR_BROWSE_FILES, Folder},
    {HomeMenuItem::RECENTS, StrId::STR_MENU_RECENT_BOOKS, Recent},
    {HomeMenuItem::OPDS_BROWSER, StrId::STR_OPDS_BROWSER, Library},
    {HomeMenuItem::APPS, StrId::STR_APPS_TITLE, Apps},
    {HomeMenuItem::FILE_TRANSFER, StrId::STR_FILE_TRANSFER, Transfer},
    {HomeMenuItem::SETTINGS_MENU, StrId::STR_SETTINGS_TITLE, Settings},
};
constexpr int kDefaultMenuItemCount = static_cast<int>(sizeof(kDefaultMenuOrder) / sizeof(kDefaultMenuOrder[0]));
constexpr int kCarouselMenuItemCount = static_cast<int>(sizeof(kCarouselMenuOrder) / sizeof(kCarouselMenuOrder[0]));

// Rows the home menu draws. Only the carousel still carries the shared
// OPDS/plugins slot, which collapses when the device has neither.
constexpr int homeMenuRowCount(bool librarySlot, bool carousel) {
  if (!carousel) return kDefaultMenuItemCount;
  return librarySlot ? kCarouselMenuItemCount : kCarouselMenuItemCount - 1;
}

constexpr const HomeMenuEntry* menuEntryAtIndex(int index, bool librarySlot, bool carousel) {
  if (index < 0) return nullptr;
  if (!carousel) {
    return index < kDefaultMenuItemCount ? &kDefaultMenuOrder[index] : nullptr;
  }
  if (!librarySlot && index >= 2) ++index;
  return index < kCarouselMenuItemCount ? &kCarouselMenuOrder[index] : nullptr;
}

constexpr HomeMenuItem indexToMenuItem(int index, bool librarySlot, bool carousel) {
  const HomeMenuEntry* entry = menuEntryAtIndex(index, librarySlot, carousel);
  return entry == nullptr ? HomeMenuItem::NONE : entry->item;
}

constexpr int menuItemToIndex(HomeMenuItem item, bool librarySlot, bool carousel) {
  for (int i = 0; i < homeMenuRowCount(librarySlot, carousel); ++i) {
    if (indexToMenuItem(i, librarySlot, carousel) == item) return i;
  }
  return 0;
}

static_assert(indexToMenuItem(0, false, false) == HomeMenuItem::LIBRARY);
static_assert(indexToMenuItem(1, false, false) == HomeMenuItem::FILE_BROWSER);
static_assert(indexToMenuItem(2, false, false) == HomeMenuItem::APPS);
static_assert(indexToMenuItem(3, false, false) == HomeMenuItem::SETTINGS_MENU);
// The row count no longer moves with the library slot outside the carousel.
static_assert(homeMenuRowCount(true, false) == homeMenuRowCount(false, false));
static_assert(menuItemToIndex(HomeMenuItem::APPS, true, false) == 2);
static_assert(indexToMenuItem(2, false, true) == HomeMenuItem::APPS);
static_assert(indexToMenuItem(4, false, true) == HomeMenuItem::SETTINGS_MENU);
static_assert(indexToMenuItem(3, true, true) == HomeMenuItem::APPS);
static_assert(indexToMenuItem(5, true, true) == HomeMenuItem::SETTINGS_MENU);
static_assert(menuItemToIndex(HomeMenuItem::APPS, false, true) == 2);
static_assert(menuItemToIndex(HomeMenuItem::SETTINGS_MENU, true, true) == 5);
}  // namespace

int HomeActivity::getMenuItemCount() const {
  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  int count = homeMenuRowCount(hasLibrarySlot(), isCarousel);
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  return count;
}

void HomeActivity::requestCarouselUpdate(CarouselUpdateScope scope) {
  if (scope == CarouselUpdateScope::Full) {
    carouselUpdateScope = scope;
  } else {
    // A queued full redraw must not be downgraded by a later menu event.
    CarouselUpdateScope expected = CarouselUpdateScope::None;
    carouselUpdateScope.compare_exchange_strong(expected, CarouselUpdateScope::MenuOnly);
  }
  requestUpdate();
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::fillCoverGridFromLibrary() {
  if (recentBooks.size() >= CoverGridHomeUi::MAX_BOOKS) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < CoverGridHomeUi::MAX_BOOKS; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && Storage.exists(UITheme::getCoverThumbPath(book.coverBmpPath, height).c_str()))
    return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (Storage.exists(epub->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (Storage.exists(xtc->getThumbBmpPath(height).c_str())) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;
  const bool useFullCover =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    const int currentProgress = progress++;
    const bool isEpub = FsHelpers::hasReflowableBookExtension(book.path);
    const bool isXtc = FsHelpers::hasXtcExtension(book.path);

    // Keep the persisted path theme-neutral; Carousel redirects only this activity's copy.
    if (isEpub) {
      const Epub epub(book.path, "/.crosspoint");
      if (useFullCover) {
        const std::string thumbPath = epub.getThumbBmpPath();
        if (book.coverBmpPath != thumbPath) {
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, thumbPath);
        }
        book.coverBmpPath = epub.getCoverBmpPath();
      } else if (book.coverBmpPath.empty()) {
        book.coverBmpPath = epub.getThumbBmpPath();
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
      }
    } else if (isXtc) {
      const Xtc xtc(book.path, "/.crosspoint");
      if (useFullCover) {
        const std::string thumbPath = xtc.getThumbBmpPath();
        if (book.coverBmpPath != thumbPath) {
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, thumbPath);
        }
        book.coverBmpPath = xtc.getCoverBmpPath();
      } else if (book.coverBmpPath.empty()) {
        book.coverBmpPath = xtc.getThumbBmpPath();
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath);
      }
    } else {
      continue;
    }
    if (book.coverBmpPath.empty()) continue;

    const std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
    if (Storage.exists(coverPath.c_str())) {
      bool invalidCache = false;
      {
        HalFile cachedCover;
        if (!Storage.openFileForRead("HOME", coverPath, cachedCover)) {
          LOG_ERR("HOME", "Failed to open cached cover: %s", coverPath.c_str());
          continue;
        }
        if (cachedCover.fileSize() == 0) {
          // EPUB uses an empty thumbnail as a persistent "no supported cover" marker.
          if (isEpub && !useFullCover) continue;
          invalidCache = true;
        } else {
          Bitmap bitmap(cachedCover);
          invalidCache =
              bitmap.parseHeaders() != BmpReaderError::Ok || bitmap.getWidth() <= 0 || bitmap.getHeight() <= 0;
        }
      }
      if (!invalidCache) continue;
      LOG_ERR("HOME", "Removing invalid cached cover: %s", coverPath.c_str());
      if (!Storage.remove(coverPath.c_str())) {
        LOG_ERR("HOME", "Failed to remove invalid cached cover: %s", coverPath.c_str());
        continue;
      }
    }

    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }
    GUI.fillPopupProgress(renderer, popupRect, 10 + currentProgress * (90 / recentBooks.size()));
    std::string generatedPath;
    if (isEpub) {
      {
        GfxRenderer::FrameBufferLoan loan(renderer);
        generatedPath = useFullCover ? BookCoverLoader::ensureFullCover(book.path)
                                     : BookCoverLoader::ensureThumbnail(book.path, coverHeight);
      }
      // Inflate used the old framebuffer bytes. Rebuild a complete, known
      // loading frame before the next progress refresh can reach the panel.
      renderer.clearScreen();
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 10 + currentProgress * (90 / recentBooks.size()));
    } else {
      generatedPath = useFullCover ? BookCoverLoader::ensureFullCover(book.path)
                                   : BookCoverLoader::ensureThumbnail(book.path, coverHeight);
    }
    if (generatedPath.empty() && isXtc) LOG_ERR("HOME", "Failed to generate XTC cover: %s", book.path.c_str());
  }

  recentsLoaded = true;
  recentsLoading = false;
  coverRendered = false;
  coverBufferStored = false;
  requestCarouselUpdate(CarouselUpdateScope::Full);
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  hasPlugins = anyPluginInstalled();

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  loadRecentBooks(coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount);
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary();
    resolveGridCoverPaths();
    coverGridUi->begin(recentBooks, hasLibrarySlot(), hasContinueReading);
  }

  const auto base = static_cast<int>(recentBooks.size());
  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  selectorIndex =
      initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasLibrarySlot(), isCarousel);
  lastCarouselBookIndex = 0;

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  if (coverBufferUnavailable) return false;

  // Thumbnail generation may borrow the framebuffer; cache only the final render.
  if (!recentsLoaded) return false;

  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;

  if (!coverBuffer || coverBufferSize < needed) {
    // The carousel region is up to ~44 KB, too large for the task stack. Allocate
    // once and reuse it for every selection during this HomeActivity lifetime.
    auto replacement = makeUniqueNoThrow<uint8_t[]>(needed);
    if (!replacement) {
      LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
      // ponytail: the theme/region is fixed for this Activity lifetime; retry
      // only after re-entering Home, when heap fragmentation may have changed.
      coverBufferUnavailable = true;
      return false;
    }
    coverBuffer = std::move(replacement);
    coverBufferSize = needed;
  }

  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(),
                                   coverBufferSize)) {
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer.get(),
                                     coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  coverBuffer.reset();
  coverBufferSize = 0;
  coverBufferStored = false;
  coverBufferUnavailable = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int bookCount = static_cast<int>(recentBooks.size());
  const int renderedMenuCount = menuCount - (metrics.homeContinueReadingInMenu ? 0 : bookCount);
  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  auto activateSelection = [this, isCarousel] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasLibrarySlot(), isCarousel)) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::RECENTS:
        onRecentsOpen();
        break;
      case HomeMenuItem::LIBRARY:
        onLibraryOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:  // the library slot
        hasPlugins ? onPluginsOpen() : onOpdsBrowserOpen();
        break;
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      case HomeMenuItem::APPS:
        onAppsOpen();
        break;
      default:
        break;
    }
  };

  if (isCarousel) {
    const bool coversFocused = selectorIndex < bookCount;
    const int rowIndex = coversFocused ? selectorIndex : selectorIndex - bookCount;

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (coversFocused) {
        selectorIndex = ButtonNavigator::nextIndex(rowIndex, bookCount);
        lastCarouselBookIndex = selectorIndex;
      } else {
        selectorIndex = bookCount + ButtonNavigator::nextIndex(rowIndex, renderedMenuCount);
      }
      requestCarouselUpdate(coversFocused ? CarouselUpdateScope::Full : CarouselUpdateScope::MenuOnly);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (coversFocused) {
        selectorIndex = ButtonNavigator::previousIndex(rowIndex, bookCount);
        lastCarouselBookIndex = selectorIndex;
      } else {
        selectorIndex = bookCount + ButtonNavigator::previousIndex(rowIndex, renderedMenuCount);
      }
      requestCarouselUpdate(coversFocused ? CarouselUpdateScope::Full : CarouselUpdateScope::MenuOnly);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (bookCount > 0) {
        if (coversFocused) {
          lastCarouselBookIndex = selectorIndex;
          selectorIndex = bookCount;
        } else {
          selectorIndex = std::clamp(lastCarouselBookIndex, 0, bookCount - 1);
        }
        requestCarouselUpdate(CarouselUpdateScope::Full);
      }
      return;
    }
  } else if (!coverGridUi) {
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int coverCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up},
                                         [&cycleBand, coverCount] { cycleBand(0, coverCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down},
                                         [&cycleBand, coverCount] { cycleBand(0, coverCount, +1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [&cycleBand, coverCount, menuCount] {
      cycleBand(coverCount, menuCount - coverCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [&cycleBand, coverCount, menuCount] {
      cycleBand(coverCount, menuCount - coverCount, +1);
    });
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (isCarousel) {
    const bool coversFocused = selectorIndex < bookCount;
    const int rowIndex = coversFocused ? selectorIndex : selectorIndex - bookCount;
    switch (swipe) {
      case MappedInputManager::SwipeDir::Left:
        if (coversFocused) {
          selectorIndex = ButtonNavigator::nextIndex(rowIndex, bookCount);
          lastCarouselBookIndex = selectorIndex;
        } else {
          selectorIndex = bookCount + ButtonNavigator::nextIndex(rowIndex, renderedMenuCount);
        }
        requestCarouselUpdate(coversFocused ? CarouselUpdateScope::Full : CarouselUpdateScope::MenuOnly);
        return;
      case MappedInputManager::SwipeDir::Right:
        if (coversFocused) {
          selectorIndex = ButtonNavigator::previousIndex(rowIndex, bookCount);
          lastCarouselBookIndex = selectorIndex;
        } else {
          selectorIndex = bookCount + ButtonNavigator::previousIndex(rowIndex, renderedMenuCount);
        }
        requestCarouselUpdate(coversFocused ? CarouselUpdateScope::Full : CarouselUpdateScope::MenuOnly);
        return;
      case MappedInputManager::SwipeDir::Up:
      case MappedInputManager::SwipeDir::Down:
        if (bookCount > 0) {
          if (coversFocused) {
            lastCarouselBookIndex = selectorIndex;
            selectorIndex = bookCount;
          } else {
            selectorIndex = std::clamp(lastCarouselBookIndex, 0, bookCount - 1);
          }
          requestCarouselUpdate(CarouselUpdateScope::Full);
        }
        return;
      case MappedInputManager::SwipeDir::None:
        break;
    }
  } else {
    if (swipe == MappedInputManager::SwipeDir::Up) {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
      return;
    }
  }

  int tx = 0;
  int ty = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTouchDown(tx, ty) && tx >= 0 && tx < renderer.getScreenWidth() &&
      ty >= metrics.homeTopPadding && ty < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    int touchedBook = 0;
    if (isCarousel) {
      const int centerBook =
          selectorIndex < bookCount ? selectorIndex : std::clamp(lastCarouselBookIndex, 0, bookCount - 1);
      if (tx < renderer.getScreenWidth() / 3) {
        touchedBook = ButtonNavigator::previousIndex(centerBook, bookCount);
      } else if (tx >= renderer.getScreenWidth() * 2 / 3) {
        touchedBook = ButtonNavigator::nextIndex(centerBook, bookCount);
      } else {
        touchedBook = centerBook;
      }
      lastCarouselBookIndex = touchedBook;
    } else {
      // Multi-cover themes (Lyra3Covers and the like) render several recent
      // books side by side: map the finger to the cover it is on instead of
      // always settling on the first book.
      touchedBook = GUI.recentBookIndexAtPoint(
          tx, ty, Rect{0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight});
      touchedBook = std::clamp(touchedBook, 0, static_cast<int>(recentBooks.size()) - 1);
    }
    if (selectorIndex != touchedBook) {
      selectorIndex = touchedBook;
      requestCarouselUpdate(CarouselUpdateScope::Full);
    }
    return;
  }

  int tapX = 0;
  int tapY = 0;
  if (!recentBooks.empty() && mappedInput.wasScreenTapped(tapX, tapY) && tapX >= 0 &&
      tapX < renderer.getScreenWidth() && tapY >= metrics.homeTopPadding &&
      tapY < metrics.homeTopPadding + metrics.homeCoverTileHeight) {
    if (!isCarousel) {
      selectorIndex = GUI.recentBookIndexAtPoint(
          tapX, tapY, Rect{0, metrics.homeTopPadding, renderer.getScreenWidth(), metrics.homeCoverTileHeight});
      selectorIndex = std::clamp(selectorIndex, 0, static_cast<int>(recentBooks.size()) - 1);
    }
    activateSelection();
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuSelection =
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size();
  int menuRow = -1;
  MappedInputManager::RowTouch menuTouch;
  if (isCarousel) {
    const int menuBottom = renderer.getScreenHeight();
    const int columnWidth = renderer.getScreenWidth() / renderedMenuCount;
    menuTouch = mappedInput.colTouch(menuRow, 0, columnWidth, renderedMenuCount, menuBottom - metrics.menuRowHeight,
                                     menuBottom, columnWidth);
  } else {
    menuTouch = mappedInput.rowTouch(menuRow, menuTop, metrics.menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                     0, INT32_MAX, metrics.menuRowHeight);
  }
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        if (isCarousel) {
          requestCarouselUpdate(CarouselUpdateScope::MenuOnly);
        } else {
          requestUpdate();
        }
      }
    } else {
      selectorIndex = touchedIndex;
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::drawStatusAndGreeting(const int pageWidth, const ThemeMetrics& metrics) {
  const int side = metrics.contentSidePadding;
  const int statusY = metrics.topPadding;

  // Status band: time and date on the left, battery on the right.
  char clock[16] = {};
  std::tm now{};
  if (TimeUtils::formatCurrentTime(clock, sizeof(clock), SETTINGS.clockFormat == 1) &&
      TimeUtils::getLocalDateTime(TimeUtils::getCurrentValidTimestamp(), now)) {
    char status[32];
    snprintf(status, sizeof(status), "%s  %02u/%02u", clock, static_cast<unsigned>(now.tm_mday),
             static_cast<unsigned>(now.tm_mon + 1));
    renderer.drawText(SMALL_FONT_ID, side, statusY, status);
  }
  GUI.drawBatteryRight(
      renderer, Rect{pageWidth - side - metrics.batteryWidth, statusY, metrics.batteryWidth, metrics.batteryHeight},
      SETTINGS.hideBatteryPercentage == 0);

  // Greeting band, under the status line.
  if (SETTINGS.ownerName[0] != '\0') {
    char greeting[64];
    snprintf(greeting, sizeof(greeting), I18N.get(StrId::STR_HOME_GREETING), SETTINGS.ownerName);
    const std::string shown = renderer.truncatedText(UI_12_FONT_ID, greeting, pageWidth - 2 * side);
    renderer.drawText(UI_12_FONT_ID, side, statusY + LyraListMetrics::statusBandHeight, shown.c_str(), true,
                      EpdFontFamily::BOLD);
  }
}

void HomeActivity::render(RenderLock&&) {
  static_assert(canRenderCarouselMenuOnly(true, true, CarouselUpdateScope::MenuOnly));
  static_assert(!canRenderCarouselMenuOnly(false, true, CarouselUpdateScope::MenuOnly));
  static_assert(!canRenderCarouselMenuOnly(true, false, CarouselUpdateScope::MenuOnly));
  static_assert(!canRenderCarouselMenuOnly(true, true, CarouselUpdateScope::Full));

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  // Must match what activateSelection() resolves the tapped row to: the third
  // slot is shared by the OPDS browser and the plugin catalog, so it exists
  // whenever either does. Counting it with hasOpdsServers alone while the
  // action counts it with hasLibrarySlot() shifts every row below it by one
  // (plugins installed, no OPDS server: Apps opened Settings).
  const int homeMenuItemCount = homeMenuRowCount(hasLibrarySlot(), isCarousel);
  const bool showContinueReading = metrics.homeContinueReadingInMenu && !recentBooks.empty();
  std::vector<const char*> menuItems;
  std::vector<UIIcon> menuIcons;
  menuItems.reserve(homeMenuItemCount + (showContinueReading ? 1 : 0));
  menuIcons.reserve(homeMenuItemCount + (showContinueReading ? 1 : 0));
  for (int i = 0; i < homeMenuItemCount; ++i) {
    const HomeMenuEntry* entry = menuEntryAtIndex(i, hasLibrarySlot(), isCarousel);
    // The shared slot takes the plugin name as well as the plugin icon;
    // labelling it "OPDS browser" while it opens the catalog reads as a bug.
    const bool pluginSlot = entry->item == HomeMenuItem::OPDS_BROWSER && hasPlugins;
    menuItems.push_back(I18N.get(pluginSlot ? StrId::STR_PLUGINS : entry->label));
    menuIcons.push_back(pluginSlot ? Plugins : entry->icon);
  }

  if (showContinueReading) {
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  const Rect headerRect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding};
  const Rect menuRect{0, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, pageWidth,
                      pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                                    metrics.homeMenuTopOffset + metrics.buttonHintsHeight)};
  auto drawHeader = [&] {
    GUI.drawHeader(renderer, headerRect,
                   metrics.homeShowRecentBookTitle && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr);
  };
  auto drawMenu = [&] {
    GUI.drawHomeMenu(
        renderer, menuRect, static_cast<int>(menuItems.size()),
        metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
        [&menuItems](int index) { return std::string(menuItems[index]); },
        [&menuIcons](int index) { return menuIcons[index]; });
  };

  const CarouselUpdateScope updateScope = carouselUpdateScope.exchange(CarouselUpdateScope::None);
  const bool menuOnlyUpdate = canRenderCarouselMenuOnly(isCarousel, recentsLoaded, updateScope);
  if (menuOnlyUpdate) {
    renderer.fillRect(headerRect.x, headerRect.y, headerRect.width, headerRect.height, false);
    drawHeader();
    renderer.fillRect(0, pageHeight - metrics.menuRowHeight, pageWidth, metrics.menuRowHeight, false);
    drawMenu();
    renderer.displayBuffer();
    return;
  }

  if (isCarousel) {
    coverRendered = false;
    coverBufferStored = false;
  }

  renderer.clearScreen();
  if (coverGridUi) {
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    // Front Left/Right walk the tabs, so their hints read Left/Right; the
    // side page buttons (unhinted) walk the covers.
    const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(!firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Refreshing the paths right away
    // lets the next pass draw already-cached thumbs before generation runs.
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
    return;
  }
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  if (isCarousel) {
    drawHeader();
  } else {
    // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
    // homeTopPadding, so the height must shrink by topPadding or the band (and a
    // centered title, e.g. RoundedRaff's book title) sinks into the tile.
    // Home is the stack root: no back button in its header.
    // Lyra List owns its header: a status band over a greeting line.
    if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_LIST) {
      drawStatusAndGreeting(pageWidth, metrics);
    } else {
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                     metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                     nullptr, false);
    }
  }

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = 0;
  coverRectY = metrics.homeTopPadding;
  coverRectW = pageWidth;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  drawMenu();

  if (!isCarousel) {
    const auto labels = mappedInput.mapLabels(SETTINGS.standbyShortcutEnabled ? tr(STR_STANDBY_TITLE) : "",
                                              tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onAppsOpen() { activityManager.goToApps(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(hasOpdsServers); }
