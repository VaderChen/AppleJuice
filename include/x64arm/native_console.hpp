#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <termios.h>
#endif

namespace x64arm {
// INPUT_RECORD 的固定 Windows ABI；不使用宿主結構或 wchar_t。
using ConsoleRecord = std::array<std::uint8_t,20>;
std::uint32_t console_field(const ConsoleRecord&,unsigned,unsigned=4);
void set_console_field(ConsoleRecord&,unsigned,std::uint32_t,unsigned=4);
ConsoleRecord console_key(char16_t,bool=true,std::uint16_t=0,std::uint16_t=0,std::uint32_t=0);
struct ConsoleCell {
    char16_t character{u' '};std::uint16_t attributes{7};
    bool operator==(const ConsoleCell&) const=default;
};
struct ConsoleSnapshot {
    std::vector<ConsoleCell> cells;
    std::u16string title{u"MacWin Console"};
    std::uint16_t columns{80},rows{25},x{},y{},attributes{7};
    std::uint16_t left{},top{},view_columns{80},view_rows{25};
    std::uint32_t cursor_size{25};bool cursor_visible{true},fullscreen{};
    // CONSOLE_SCREEN_BUFFER_INFOEX 的色表（COLORREF 0x00BBGGRR）與彈出視窗屬性；原生視窗依此繪製。
    std::array<std::uint32_t,16> colors{0x000000,0x800000,0x008000,0x808000,0x000080,0x800080,0x008080,0xc0c0c0,
                                        0x404040,0xff4040,0x40ff40,0xffff40,0x4040ff,0xff40ff,0x40ffff,0xffffff};
    std::uint16_t popup_attributes{0xf5};
    // 目前字型：儲存格像素大小與 CONSOLE_FONT_INFOEX 的字族、粗細、字面名稱；原生視窗依此排版。
    std::uint16_t font_width{9},font_height{18};std::uint32_t font_family{0x36},font_weight{400};
    std::u16string font_face{u"SF Mono"};
};
class ConsoleWindow {
public:
    virtual ~ConsoleWindow()=default;
    virtual bool update(const ConsoleSnapshot&)=0;
    virtual std::vector<ConsoleRecord> poll(unsigned milliseconds)=0;
    virtual bool closed() const=0;
    virtual bool fullscreen() const=0;
    virtual void* native_view() {return nullptr;}
    virtual bool bind_native_window(void*) {return false;}
    virtual std::array<std::uint16_t,2> maximum_size() const {return {32767,32767};}
    virtual bool set_fullscreen(bool) {return false;}
};
std::unique_ptr<ConsoleWindow> create_console_window();
struct ConsoleBuffer : ConsoleSnapshot {
    struct ConioCharacter {std::uint16_t value{};bool echo{};};
    ConsoleBuffer();
    std::deque<ConsoleRecord> records,ansi_records,ansi_input_records;
    std::deque<ConioCharacter> conio_bytes,conio_units;
    char16_t conio_output_high{};
    std::unique_ptr<ConsoleWindow> window;
    std::uint64_t window_handle{};
    std::string terminal_input,output_utf8,escape;
    std::uint32_t input_mode{7},output_mode{7};
    std::uint32_t input_codepage{65001},output_codepage{65001};
    std::string output_codepage_pending;
    bool eof{};std::u16string edited_line,echo_input;
    std::size_t edit_tail{}; // 游標右側的 UTF-16 單位數
    std::u16string original_title{title};
    // 別名與命令歷程依執行檔名稱分組（名稱不分大小寫）；歷程只記錄行輸入模式送出的非空命令。
    struct Alias {std::u16string source,target;};
    struct AliasTable {std::u16string exe;std::vector<Alias> entries;};
    struct History {
        std::u16string exe;
        std::deque<std::u16string> commands;
        std::uint32_t capacity;
        void resize(std::uint32_t size){
            if(commands.size()>size)commands.resize(size);
            capacity=size;
        }
    };
    std::vector<AliasTable> aliases;std::vector<History> histories;
    std::uint32_t history_size{50},history_buffers{4},history_flags{};
    void resize(std::uint16_t,std::uint16_t,bool preserve_viewport=false);
    void resize_viewport(std::uint16_t,std::uint16_t);
    void reveal_cursor();
    void write(std::string_view);
    void ingest(std::string_view,bool=false);
    void push(ConsoleRecord);
private:
    void character(char16_t);
    void control_sequence();
};
struct NativeConsole {
    std::shared_ptr<ConsoleBuffer> buffer;
    int descriptor{-1};bool changed{};
#if !defined(_WIN32)
    termios original{};
#endif
    ~NativeConsole();
};
}
