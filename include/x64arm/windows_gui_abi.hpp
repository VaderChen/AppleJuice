#pragma once
#include <cstdint>
#include <string_view>
namespace x64arm {
class GuestMemory;
struct GuiMessage;
// 對話框與訊息派送共用 MSG 客體佈局及記憶體範圍檢查。
GuiMessage gui_read_message(const GuestMemory&,std::uint64_t);
void gui_write_message(GuestMemory&,std::uint64_t,const GuiMessage&);
// 與 RegisterWindowMessageA/W 共用穩定名稱對應，供系統對話框接收公開註冊訊息。
constexpr std::uint32_t gui_registered_message(std::u16string_view text) {
    std::uint32_t hash=2166136261;for(auto c:text)hash=(hash^c)*16777619;
    return 0xc000+hash%0x4000;
}
// Windows 視窗類別的客體 ABI；不可使用宿主指標寬度。
struct GuiClassAbi {
    unsigned size, style, procedure, class_extra, window_extra, instance, icon, cursor, background, menu, name, small_icon;
    std::uint8_t pointer_bits;
    constexpr GuiClassAbi(bool x86, bool extended)
        : size(x86?(extended?48U:40U):(extended?80U:72U)), style(extended?4U:0U),
          procedure(x86?(extended?8U:4U):8U), class_extra(procedure+(x86?4U:8U)),
          window_extra(class_extra+4), instance(window_extra+4), icon(instance+(x86?4U:8U)),
          cursor(icon+(x86?4U:8U)), background(cursor+(x86?4U:8U)), menu(background+(x86?4U:8U)),
          name(menu+(x86?4U:8U)), small_icon(name+(x86?4U:8U)), pointer_bits(x86?32:64) {}
};
// 共用控制項訊息的結構不依宿主或呼叫 SendMessageA/W 的字元寬度改變。
struct GuiControlAbi {
    unsigned unit;std::uint8_t bits;
    unsigned notify_size,notify_code;
    unsigned combo_size,combo_index,combo_text,combo_capacity,combo_image,combo_selected,combo_overlay,combo_indent,combo_data;
    unsigned toolbar_size,toolbar_data,toolbar_string,toolbar_info_size,toolbar_info_data,toolbar_info_text,toolbar_info_capacity;
    unsigned list_size,list_text,list_capacity,list_image,list_data;
    unsigned column_size,column_text,column_capacity,column_subitem;
    unsigned tree_size,tree_handle,tree_state,tree_mask,tree_text,tree_capacity,tree_image,tree_selected,tree_children,tree_data;
    unsigned rebar_text,rebar_capacity,rebar_image,rebar_child,rebar_min_width,rebar_min_height,rebar_width,rebar_bitmap,rebar_id,rebar_height,rebar_max_height,rebar_integral,rebar_ideal,rebar_data,rebar_header;
    constexpr explicit GuiControlAbi(bool small)
      : unit(small?4U:8U),bits(small?32:64),notify_size(small?12U:24U),notify_code(unit*2),
        combo_size(small?36U:56U),combo_index(unit),combo_text(unit*2),combo_capacity(unit*3),
        combo_image(combo_capacity+4),combo_selected(combo_capacity+8),combo_overlay(combo_capacity+12),combo_indent(combo_capacity+16),combo_data(small?32U:48U),
        toolbar_size(small?20U:32U),toolbar_data(small?12U:16U),toolbar_string(toolbar_data+unit),
        toolbar_info_size(small?32U:48U),toolbar_info_data(small?20U:24U),toolbar_info_text(toolbar_info_data+unit),toolbar_info_capacity(toolbar_info_text+unit),
        list_size(small?36U:48U),list_text(small?20U:24U),list_capacity(list_text+unit),list_image(list_capacity+4),list_data(list_image+4),
        column_size(small?24U:32U),column_text(small?12U:16U),column_capacity(column_text+unit),column_subitem(column_capacity+4),
        tree_size(small?40U:56U),tree_handle(unit),tree_state(unit*2),tree_mask(tree_state+4),tree_text(tree_mask+4),tree_capacity(tree_text+unit),tree_image(tree_capacity+4),tree_selected(tree_image+4),tree_children(tree_selected+4),tree_data(tree_children+4),
        rebar_text(small?20U:24U),rebar_capacity(rebar_text+unit),rebar_image(rebar_capacity+4),rebar_child(rebar_image+4),rebar_min_width(rebar_child+unit),rebar_min_height(rebar_min_width+4),rebar_width(rebar_min_height+4),rebar_bitmap(small?48U:64U),rebar_id(rebar_bitmap+unit),rebar_height(rebar_id+4),rebar_max_height(rebar_height+4),rebar_integral(rebar_max_height+4),rebar_ideal(rebar_integral+4),rebar_data(small?72U:96U),rebar_header(rebar_data+unit) {}
};
}
