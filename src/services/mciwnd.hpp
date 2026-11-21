#pragma once

#include "mci_avi.hpp"

namespace x64arm::vfw {
inline constexpr std::u16string_view mci_window_class=u"MCIWndClass";
inline constexpr unsigned wnd_device=1124, wnd_alias=1161, wnd_position_a=1126, wnd_start=1127,
    wnd_length=1128,wnd_end=1129, wnd_mode_a=1130, wnd_eject=1131,
    wnd_zoom_set=1132,wnd_zoom_get=1133, wnd_volume_set=1134,wnd_volume_get=1135,
    wnd_speed_set=1136,wnd_speed_get=1137,wnd_repeat_set=1138,wnd_repeat_get=1139,
    wnd_realize=1142,wnd_validate=1145,wnd_play_from=1146,wnd_play_to=1147,
    wnd_filename_a=1148,wnd_name_a=1149,wnd_palette_get=1150,wnd_palette_set=1151,
    wnd_error_a=1152,wnd_timers_set=1153,wnd_active_set=1154,wnd_inactive_set=1155,
    wnd_active_get=1156,wnd_inactive_get=1157,wnd_styles_get=1160,wnd_styles_set=1159,
    wnd_play_reverse=1163,wnd_open_a=1177,wnd_open_w=1276,wnd_position_w=1226,wnd_mode_w=1230,
    wnd_filename_w=1248,wnd_name_w=1249,wnd_error_w=1252,
    wnd_time_set_a=1143,wnd_time_get_a=1144,wnd_time_set_w=1243,wnd_time_get_w=1244,
    wnd_source_get=1164,wnd_source_put=1165,wnd_dest_get=1166,wnd_dest_put=1167,
    wnd_can_play=1168,wnd_can_window=1169,wnd_can_record=1170,wnd_can_save=1171,wnd_can_eject=1172,
    wnd_can_configure=1173,wnd_palette_kick=1174,wnd_interface_open=1175,wnd_owner_set=1176,
    wnd_command_a=1125,wnd_command_w=1225,wnd_reply_a=1162,wnd_reply_w=1262,wnd_new_a=1158,wnd_new_w=1258;
inline constexpr unsigned clip_timer=0x4d43, play_button=0x3200,menu_button=0x3201,track_bar=0x3202;
struct MciWindow {
    std::uint64_t owner{},palette{};
    std::uint32_t device{},styles{},error{},zoom{100},active_timer{50},inactive_timer{500};
    std::uint32_t last_mode{524},last_position{UINT32_MAX};
    bool repeat{},busy{},active{true};
    std::u16string file,alias,reply;
};
}
