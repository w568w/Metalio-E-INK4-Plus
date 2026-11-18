#include "display/screen/book_screen/reader/book_reader_hooks.h"
#include "reader/txt_chapter.h"
#include <cassert>

static std::weak_ptr<int> callback_lifetime;

int main() {
    auto toc = std::make_shared<ReaderToc>(
        ReaderToc{"远程小说", {{"卷一", false}, {"第一章"}, {"卷二", false}, {"第二章"}}});
    HostToc host;
    host.toc = toc;
    host.open = [](int) { return true; };
    assert(!host.Valid()); // 没有当前项不算可用目录。
    host.index = 2;
    assert(!host.Valid()); // 当前项是卷标题，不可阅读。
    host.index = 3;
    assert(host.Valid());
    assert(host.Adjacent(1) == -1);
    assert(host.Adjacent(-1) == 1);
    host.index = 1;
    assert(host.Adjacent(1) == 3);

    ReaderLaunch launch;
    launch.host_toc = host;
    int selected = -1;
    launch.host_toc->open = [&](int index) { selected = index; return true; };
    assert(!launch.host_toc->Select(0)); // 卷标题不可跳转。
    assert(!launch.host_toc->Select(2));
    assert(!launch.host_toc->Select(-1));
    assert(!launch.host_toc->Select(4));
    assert(selected == -1);
    assert(launch.host_toc->Select(3));
    assert(selected == 3);
    launch.host_toc->open = [](int) { return false; };
    assert(!launch.host_toc->Select(1)); // 宿主忙时拒绝请求，调用方据此不收起目录。

    // 宿主同步替换阅读请求时，正在执行的回调仍须存活到返回。
    {
        auto owned = std::make_shared<int>(1);
        callback_lifetime = owned;
        launch.host_toc->open = [&, owned](int index) {
            launch = {};
            assert(!callback_lifetime.expired());
            selected = index;
            return true;
        };
    }
    assert(launch.host_toc->Select(1));
    assert(selected == 1 && !launch.host_toc);
    assert(callback_lifetime.expired());

    // 解析借用的标题片段，不能读入片段之外的尾随内容。
    std::string ordinal, name;
    const std::string_view title("第十二章 正文--尾随", sizeof("第十二章 正文") - 1);
    assert(reader::SplitChapterHeading(title, ordinal, name));
    assert(ordinal == "第十二章" && name == "正文");
    assert(!reader::SplitChapterHeading({}, ordinal, name));
    assert(ordinal.empty() && name.empty());
}
