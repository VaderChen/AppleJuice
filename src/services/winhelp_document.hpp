#pragma once
#include "winhelp_file.hpp"
namespace x64arm::winhelp {
struct PagePart {
    std::size_t record{};
    std::uint32_t offset{};
    TextLayout layout;
};
struct Page {
    std::size_t record{};
    TopicHeader header;
    std::vector<PagePart> parts;
};
struct DocumentLocation {
    std::size_t page{},part{};
    std::uint32_t character{};
    bool heading{};
};
class Document {
public:
    explicit Document(const File&);
    const System& system()const{return system_;}
    const std::vector<Font>& fonts()const{return fonts_;}
    const std::vector<Page>& pages()const{return pages_;}
    const KeywordIndex& keywords()const{return keywords_;}
    std::optional<DocumentLocation> locate(std::uint32_t offset)const;
    std::optional<DocumentLocation> context(std::uint32_t id)const;
    std::optional<DocumentLocation> hashed_context(std::uint32_t hash)const;
    std::optional<DocumentLocation> named_context(std::string_view name)const;
    std::optional<DocumentLocation> contents()const;
private:
    System system_;
    std::vector<Font> fonts_;
    TopicIndex index_;
    KeywordIndex keywords_;
    std::vector<TopicAddress> addresses_;
    std::vector<Page> pages_;
    std::map<std::size_t,DocumentLocation> records_;
};
}
