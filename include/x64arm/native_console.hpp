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
};
std::unique_ptr<ConsoleWindow> create_console_window();
struct ConsoleBuffer : ConsoleSnapshot {
    struct ConioCharacter {std::uint16_t value{};bool echo{};};
    ConsoleBuffer();
    std::deque<ConsoleRecord> records,ansi_records;
    std::deque<ConioCharacter> conio_bytes,conio_units;
    char16_t conio_output_high{};
    std::unique_ptr<ConsoleWindow> window;
    std::uint64_t window_handle{};
    std::string terminal_input,output_utf8,escape;
    std::uint32_t input_mode{7},output_mode{7};
    bool eof{};std::u16string edited_line,echo_input;
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
