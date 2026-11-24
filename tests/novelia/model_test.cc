#include "novalpie_chapter.h"
#include "novelia_site.h"
#include "web_novel_model.h"
#include "web_novel_epub.h"
#include <cassert>
#include <fstream>
using namespace web_novel;
namespace Lang::Strings {
const char* NOVELIA_FAVORITES_INVALID = "Unexpected favorites data";
const char* NOVELIA_FOLDER_ID_MISSING = "Favorite folder has no id";
const char* NOVELIA_FOLDER_UNNAMED = "Favorites";
const char* NOVELIA_NOVELS_INVALID = "Unexpected novel list data";
const char* NOVELIA_NOVEL_ID_MISSING = "Novel has no id";
const char* NOVELIA_TOC_INVALID = "Unexpected chapter list data";
const char* NOVELIA_TRANSLATION_SOURCE = "Original";
}
int main(int argc, char** argv) {
    assert(web_novel::SignNovalpieSession("1795291822", "a1b2c3d4") == "DFZQKSJdAn4IDK7lOdC8Xw9E2sH=");
    // 由 Node crypto 独立生成，验证会话密钥派生及 GCM 验证标签。
    const std::string session = "UVFRUVFRUVFRUVFRUVFRUVFRUVFRUVFRUVFRUVFRUVE=";
    auto encrypted = web_novel::Parse(R"({
        "encrypted": true,
        "content": "kcE0uPJYet8QEFsFlqBq2FBtUQ==",
        "iv": "IyMjIyMjIyMjIyMj",
        "tag": "Wwz6iP5dCDqccLxannF3sg=="
    })");
    assert(web_novel::DecodeNovalpieChapter(encrypted.get(), session) == "第一段\n第二段");
    web_novel::Set(encrypted.get(), "tag", "Awz6iP5dCDqccLxannF3sg==");
    assert(web_novel::DecodeNovalpieChapter(encrypted.get(), session).empty());

    assert(argc == 2);
    assert(Format("%d", -104) == "-104");
    assert(Format("%s %.1f MB / %d", "cache", 12.3, 84) == "cache 12.3 MB / 84");
    auto folders = Parse(R"({"favoredWeb":[{"id":"a","title":"收藏 & 中文"}]})");
    assert(NoveliaFolders(folders.get()).at(0).title == "收藏 & 中文");
    auto favorites = Parse(R"({"items":[{"providerId":"syosetu","novelId":"n123","titleZh":"小说"}]})");
    assert(NoveliaNovels(favorites.get()).at(0).id == "syosetu/n123");
    auto toc = Parse(R"({"toc":[{"titleZh":"第一卷"},{"chapterId":"01","titleJp":"one"},{"chapterId":2,"titleZh":"二"}]})");
    auto items = NoveliaToc(toc.get());
    assert(items.size() == 3);
    assert(items[0].volume);
    assert(items[1].id == "01");
    assert(items[2].id == "2");
    Set(toc.get(), "lastReadChapterId", "2");
    const auto remote = Text(toc.get(), "lastReadChapterId");
    assert(ResumeChapter(items, "", remote) == "2");
    assert(ResumeChapter(items, "01", remote) == "01");
    assert(ResumeChapter(items, "removed", remote) == "2");
    assert(ResumeChapter(items, "01", "") == "01");
    assert(ResumeChapter(items, "", "").empty());
    assert(ResumeChapter(items, "removed", "removed").empty());
    assert(ResumeChapter({}, "01", remote).empty());
    auto volume_toc = Parse(R"({"toc":[{"chapterId":"01"},{"titleJp":"卷二"},{"chapterId":"02"}]})");
    assert(ResumeChapter(NoveliaToc(volume_toc.get()), "", "02") == "02");
    auto snapshot = Parse(Encode(toc.get()));
    assert(ResumeChapter(NoveliaToc(snapshot.get()), "", Text(snapshot.get(), "lastReadChapterId")) == "2");
    std::string error;
    auto changed = Parse(R"({"books":[]})");
    assert(NoveliaNovels(changed.get(), &error).empty() && !error.empty());
    assert(TranslationField("gpt") && !TranslationField("unknown"));
    assert(Quote("a/b 中") == "a%2Fb%20%E4%B8%AD");
    assert(EscapeXml("<a&\n") == "&lt;a&amp;<br/>");
    std::string image = std::string(argv[1]) + ".image";
    { std::ofstream file(image); file << "image-bytes"; }
    const auto xhtml = image + ".xhtml";
    { std::ofstream file(xhtml); file << "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>测试</title></head><body><p>文字</p><img src=\"img.png\"/></body></html>"; }
    assert(WriteEpub(argv[1], "书 & 名", "作者", xhtml, {{"img.png", "image/png", image}}));
    std::remove(xhtml.c_str());
    std::remove(image.c_str());
}
