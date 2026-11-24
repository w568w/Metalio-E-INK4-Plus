#include "web_novel_epub.h"
#include "web_novel_model.h"
#include <cstdio>
#include <cstdint>
#include <zlib.h>
namespace web_novel {
namespace {
class Zip {
public:
    explicit Zip(const std::string& path) : file_(std::fopen(path.c_str(), "wb")) {}
    ~Zip() {
        if (file_)
            std::fclose(file_);
    }
    bool Add(const std::string& name, const std::string& data) {
        if (!file_ || entries_.size() >= 65535 || name.size() > 65535 || data.size() > UINT32_MAX)
            return false;
        long offset = std::ftell(file_);
        if (offset < 0)
            return false;
        Entry entry{name, uint32_t(crc32(0, reinterpret_cast<const Bytef*>(data.data()), data.size())),
                    uint32_t(data.size()), uint32_t(offset)};
        LocalHeader(entry);
        Bytes(data);
        entries_.push_back(entry);
        return !std::ferror(file_);
    }
    bool AddFile(const std::string& name, const std::string& path) {
        FILE* source = std::fopen(path.c_str(), "rb");
        if (!source || !file_) {
            if (source)
                std::fclose(source);
            return false;
        }
        char buffer[4096];
        size_t n;
        uint32_t crc = crc32(0, nullptr, 0);
        uint64_t size = 0;
        while ((n = std::fread(buffer, 1, sizeof(buffer), source))) {
            size += n;
            crc = crc32(crc, reinterpret_cast<const Bytef*>(buffer), n);
        }
        long offset = std::ftell(file_);
        bool ok = !std::ferror(source) && size <= UINT32_MAX && offset >= 0;
        if (ok) {
            Entry entry{name, crc, uint32_t(size), uint32_t(offset)};
            LocalHeader(entry);
            std::rewind(source);
            while ((n = std::fread(buffer, 1, sizeof(buffer), source)))
                std::fwrite(buffer, 1, n, file_);
            ok = !std::ferror(source) && !std::ferror(file_);
            entries_.push_back(entry);
        }
        std::fclose(source);
        return ok;
    }
    bool Finish() {
        if (!file_)
            return false;
        long start = std::ftell(file_);
        if (start < 0)
            return false;
        for (const auto& entry : entries_) {
            // clang-format off
            U32(0x02014b50); U16(20); U16(20); // Central record and version fields.
            U16(0x800); U16(0); U16(0); U16(33); // UTF-8, STORE, DOS time/date.
            U32(entry.crc); U32(entry.size); U32(entry.size);
            U16(entry.name.size()); U16(0); U16(0); // Name, extra and comment lengths.
            U16(0); U16(0); U32(0); U32(entry.offset); // Disk, attributes, local offset.
            // clang-format on
            Bytes(entry.name);
        }
        long end = std::ftell(file_);
        if (end < start)
            return false;
        U32(0x06054b50);
        U16(0);
        U16(0);
        U16(entries_.size());
        U16(entries_.size());
        U32(end - start);
        U32(start);
        U16(0);
        bool ok = !std::ferror(file_);
        if (std::fclose(file_) != 0)
            ok = false;
        file_ = nullptr;
        return ok;
    }

private:
    struct Entry {
        std::string name;
        uint32_t crc, size, offset;
    };
    void LocalHeader(const Entry& entry) {
        // clang-format off
        U32(0x04034b50);                        // Local file header signature.
        U16(20); U16(0x800); U16(0);            // Version, UTF-8 flag, STORE method.
        U16(0); U16(33);                       // DOS time/date: 1980-01-01.
        U32(entry.crc); U32(entry.size); U32(entry.size);
        U16(entry.name.size()); U16(0);        // Name and extra-field lengths.
        // clang-format on
        Bytes(entry.name);
    }
    void U16(uint16_t n) {
        std::fputc(n & 255, file_);
        std::fputc(n >> 8, file_);
    }
    void U32(uint32_t n) {
        U16(n & 65535);
        U16(n >> 16);
    }
    void Bytes(const std::string& value) {
        std::fwrite(value.data(), 1, value.size(), file_);
    }
    FILE* file_;
    std::vector<Entry> entries_;
};
} // namespace
bool WriteEpub(const std::string& path, const std::string& title, const std::string& author,
               const std::string& xhtml_path, const std::vector<EpubImage>& images) {
    Zip zip(path);
    if (!zip.Add("mimetype", "application/epub+zip") ||
        !zip.Add("META-INF/container.xml",
                 "<?xml version=\"1.0\"?><container version=\"1.0\" "
                 "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\"><rootfiles><rootfile "
                 "full-path=\"content.opf\" "
                 "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"))
        return false;
    std::string manifest = "<item id=\"body\" href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>";
    for (size_t i = 0; i < images.size(); ++i) {
        const auto& image = images[i];
        if (!zip.AddFile(image.name, image.path))
            return false;
        manifest += "<item id=\"img" + std::to_string(i) + "\" href=\"" + image.name + "\" media-type=\"" +
                    image.mime + "\"/>";
    }
    const std::string opf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" "
        "version=\"2.0\" unique-identifier=\"id\"><metadata "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">" +
        EscapeXml(path) + "</dc:identifier><dc:title>" + EscapeXml(title) + "</dc:title><dc:creator>" +
        EscapeXml(author) + "</dc:creator><dc:language>zh</dc:language></metadata><manifest>" + manifest +
        "</manifest><spine><itemref idref=\"body\"/></spine></package>";
    return zip.Add("content.opf", opf) && zip.AddFile("chapter.xhtml", xhtml_path) && zip.Finish();
}
} // namespace web_novel
