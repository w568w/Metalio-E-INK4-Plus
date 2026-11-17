#include "book_external.h"
#include "book_screen/book_nav.h"
#include "book_screen/book_screen_priv.h"
#include "book_screen/reader/book_reader_body.h"
#include "screen_common.h"
#include "vk_key_handler.h"
#include <utility>

namespace {
lv_obj_t* Create() {
    if (auto* waiting = BookWaitForCleanup("external_book_loading", VkKeyScreenDesc{Create}))
        return waiting;
    return CreateReaderScreen(Book_State().reader.launch.book);
}
lv_obj_t* CommitRequestedBook() {
    auto& reader = Book_State().reader;
    reader.launch = std::move(reader.pending_launch);
    reader.rebuild_host_reader = Create;
    return Create();
}
} // namespace
void BookExternalOpen(ReaderLaunch next) {
    auto& reader = Book_State().reader;
    const bool replacing = reader.IsHostSession();
    reader.pending_launch = std::move(next);
    if (replacing)
        ScreenNavigateReplace(CommitRequestedBook);
    else
        ScreenNavigateTo(CommitRequestedBook);
}
