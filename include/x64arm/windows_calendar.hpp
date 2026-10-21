#pragma once
#include "x64arm/windows_gui.hpp"
#include "x64arm/memory.hpp"
#include <functional>
namespace x64arm {
bool gui_calendar_class(std::u16string_view name);
bool gui_calendar_dispatch(std::uint32_t message);
bool gui_date_valid(const GuiSystemTime& value,bool time=true);
std::int64_t gui_date_ordinal(const GuiSystemTime& value);
GuiSystemTime gui_date_from_ordinal(std::int64_t value);
void gui_calendar_set_view(GuiCalendar& calendar,std::uint32_t view);
void gui_calendar_grid(GuiWindow& window,const GuiFontMetrics& metrics,const std::function<std::int32_t(std::u16string_view)>& measure={});
std::optional<std::uint64_t> gui_calendar_grid_message(GuestMemory& memory,GuiWindow& window,const GuiMessage& message,bool x86);
GuiSystemTime gui_date_normalize(GuiSystemTime value,bool time=true);
void gui_calendar_initialize(GuiWindow& window,const GuiSystemTime& value);
void gui_calendar_project(GuiWindow& window,NativeGui* native);
std::u16string gui_calendar_today_text(const GuiCalendar& calendar);
std::uint32_t gui_calendar_today_width(const GuiWindow& window,NativeGui* native);
void gui_calendar_layout(GuiWindow& window,const GuiFontMetrics& metrics,const std::function<std::int32_t(std::u16string_view)>& measure={});
std::optional<std::uint64_t> gui_calendar_message(GuestMemory& memory,GuiWindow& window,const GuiMessage& message,bool wide,bool x86);
void gui_date_write(GuestMemory& memory,std::uint64_t address,const GuiSystemTime& value);
std::array<std::int32_t,3> gui_date_field_spin(const GuiCalendar& calendar);
bool gui_date_field_step(GuiCalendar& calendar,std::int64_t delta);
void gui_date_capture_callbacks(GuiWindow& window,GuiControlOperation::Calendar& request,std::uint32_t message,std::uint64_t previous_value_revision);
inline constexpr std::size_t gui_calendar_locale_chars=128;
inline constexpr auto gui_calendar_locale_types=[](){std::array<std::uint32_t,76> types{};types[0]=0x1f;types[1]=0x20;types[2]=0x1003;types[3]=0x100c;types[4]=0x100d;for(unsigned n=0;n<12;++n){types[5+n]=0x38+n;types[17+n]=0x44+n;types[29+n]=0x10000038+n;types[41+n]=0x10000044+n;}for(unsigned n=0;n<7;++n){types[53+n]=0x2a+n;types[60+n]=0x31+n;types[67+n]=0x60+n;}types[74]=0x28;types[75]=0x29;return types;}();
std::shared_ptr<const GuiCalendarLocale> gui_calendar_read_locale(const GuestMemory& memory,std::uint64_t buffer);
void gui_calendar_apply_locale(GuiCalendar& calendar,std::shared_ptr<const GuiCalendarLocale> locale);
std::u16string_view gui_calendar_month_name(const GuiCalendar& calendar,unsigned month,bool abbreviated=false,bool genitive=false);
std::u16string_view gui_calendar_weekday_name(const GuiCalendar& calendar,unsigned weekday,unsigned style=0);
std::u16string gui_date_format_text(const GuiCalendar& calendar,const GuiSystemTime& date,std::u16string_view format);
}
