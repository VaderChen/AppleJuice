#pragma once
#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace x64arm::winhelp {
struct WindowSpec {
    std::uint16_t flags{};
    std::string type,name,caption;
    std::int16_t x{},y{},width{},height{},maximize{};
    std::uint32_t scroll_color{},non_scroll_color{};
};
struct System {
    std::uint16_t version{},flags{};
    std::string title,copyright;
    std::optional<std::uint32_t> contents;
    std::vector<std::string> macros;
    std::vector<WindowSpec> windows;
};
struct File {
    std::map<std::string,std::vector<std::uint8_t>> streams;
    System system;
};
// 只解析資料；巨集保留為文字，不能在解析階段執行。
// 損毀或尚不支援的容器會拋出 std::invalid_argument。
File read(std::span<const std::uint8_t>);
std::vector<std::uint8_t> decompress(std::span<const std::uint8_t>,std::size_t limit,bool block_padding=false);
struct TopicBlock {
    std::uint32_t previous{},first{},last_header{};
    std::vector<std::uint8_t> data;
};
struct TopicRecord {
    std::uint32_t position{},previous{},next{},text_size{};
    std::uint8_t kind{};
    std::vector<std::uint8_t> formatting,encoded_text;
};
std::vector<TopicBlock> topic_blocks(const File&);
enum class PhraseEncoding { none, classic, hall };
struct PhraseTable {
    PhraseEncoding encoding{PhraseEncoding::none};
    std::vector<std::vector<std::uint8_t>> entries;
};
PhraseTable phrase_table(const File&);
// 還原位元組及內嵌 NUL；碼頁轉換與格式呈現由上層處理。
std::vector<std::uint8_t> topic_text(const PhraseTable&,const TopicRecord&);
std::vector<TopicRecord> topic_records(const File&);
struct TopicIndex {
    // 值是 TOPICOFFSET，不能直接當作 TOPICPOS 使用。
    std::map<std::uint32_t,std::uint32_t> context_ids,context_hashes;
    std::map<std::uint32_t,std::string> titles;
};
TopicIndex topic_index(const File&);
std::uint32_t context_hash(std::string_view);
struct TopicAddress {
    std::uint32_t offset{},length{};
    std::size_t record{},header{};
};
struct TopicLocation { std::size_t record{},header{}; std::uint32_t character{}; };
std::vector<TopicAddress> topic_addresses(const File&,std::span<const TopicRecord>);
std::optional<TopicLocation> locate_topic(std::span<const TopicAddress>,std::uint32_t offset);
struct TopicHeader {
    std::uint32_t number{},browse_previous{},browse_next{},non_scroll{},scroll{},next{};
    std::string title;
    std::vector<std::string> macros;
};
TopicHeader topic_header(const PhraseTable&,const TopicRecord&);
struct Font {
    std::string face;
    std::uint16_t height_twips{},weight{};
    std::uint8_t attributes{},family{};
    std::uint32_t foreground{},background{}; // Windows COLORREF：0x00BBGGRR
};
std::vector<Font> fonts(const File&);
struct LinkTarget {
    std::uint8_t type{};
    std::optional<std::uint8_t> window_number;
    std::string file,window;
};
struct TextRun {
    std::uint8_t opcode{}; // 0 為文字；其餘保留 HLP 格式命令。
    std::uint32_t argument{},character{};
    std::string text;
    std::optional<LinkTarget> link{};
};
struct Paragraph {
    std::int16_t column{-1};
    std::uint16_t flags{},id{};
    std::array<std::int32_t,6> metrics{}; // 上／下／行距、左／右／首行縮排
    std::uint8_t border{};
    std::int16_t border_width{};
    std::vector<std::pair<std::uint16_t,std::uint16_t>> tabs;
    std::vector<TextRun> runs;
};
struct TextLayout {
    bool table{};
    std::uint8_t table_type{};
    std::int16_t minimum_width{};
    std::vector<std::pair<std::int16_t,std::int16_t>> columns; // 間距、欄寬
    std::vector<Paragraph> paragraphs;
};
TextLayout text_layout(const PhraseTable&,const TopicRecord&);
Paragraph paragraph(const PhraseTable&,const TopicRecord&);
using KeywordIndex=std::map<std::string,std::vector<std::uint32_t>>;
// prefix 為 footnote 索引字元，或多媒體的 xK；值為 TOPICOFFSET。
KeywordIndex keyword_index(const File&,std::string_view prefix="K");
}
