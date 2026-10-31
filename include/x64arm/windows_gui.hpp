#pragma once
#include "x64arm/windows_printing.hpp"
#include "x64arm/windows_gui_drag.hpp"
#include "x64arm/windows_ole_drag.hpp"
#include "x64arm/guest_callback.hpp"
#include "x64arm/gui_icon.hpp"
#include "x64arm/desktop_notifications.hpp"
#include "x64arm/native_shell.hpp"
#include "x64arm/native_winhelp.hpp"
#include <cstdint>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include "x64arm/windows_region.hpp"
#include "x64arm/windows_fonts.hpp"
#include "x64arm/windows_keyboard.hpp"

namespace x64arm {
inline constexpr std::uint64_t gui_message_parent=UINT64_MAX-2;

// 鍵集合每次改變都取得全域唯一版本；只改值不換版本。相同版本代表相同鍵集合，
// 供 GDI 資源計數快取判斷 DC 表是否變動。未包裝的變更介面刻意刪除，避免繞過版本。
template<class Key,class Value> class KeyVersionedMap : public std::map<Key,Value> {
    using Base=std::map<Key,Value>;
    static std::uint64_t next_version() noexcept {static std::atomic<std::uint64_t> counter{1};return counter.fetch_add(1,std::memory_order_relaxed);}
    std::uint64_t version_{next_version()};
public:
    KeyVersionedMap()=default;
    KeyVersionedMap(const KeyVersionedMap&)=default;
    KeyVersionedMap& operator=(const KeyVersionedMap&)=default;
    KeyVersionedMap(KeyVersionedMap&& other) noexcept:Base(std::move(other)),version_(other.version_){other.version_=next_version();}
    KeyVersionedMap& operator=(KeyVersionedMap&& other) noexcept {Base::operator=(std::move(other));version_=other.version_;other.version_=next_version();return *this;}
    std::uint64_t key_version() const noexcept {return version_;}
    Value& operator[](const Key& key){auto [at,inserted]=Base::try_emplace(key);if(inserted)version_=next_version();return at->second;}
    template<class... Args> std::pair<typename Base::iterator,bool> emplace(Args&&... args){auto result=Base::emplace(std::forward<Args>(args)...);if(result.second)version_=next_version();return result;}
    template<class... Args> std::pair<typename Base::iterator,bool> try_emplace(const Key& key,Args&&... args){auto result=Base::try_emplace(key,std::forward<Args>(args)...);if(result.second)version_=next_version();return result;}
    typename Base::size_type erase(const Key& key){const auto count=Base::erase(key);if(count)version_=next_version();return count;}
    typename Base::iterator erase(typename Base::iterator at){version_=next_version();return Base::erase(at);}
    typename Base::iterator erase(typename Base::const_iterator at){version_=next_version();return Base::erase(at);}
    void clear() noexcept {if(!Base::empty())version_=next_version();Base::clear();}
    template<class... Args> void insert(Args&&...)=delete;
    template<class... Args> void insert_or_assign(Args&&...)=delete;
    template<class... Args> void emplace_hint(Args&&...)=delete;
    template<class... Args> void extract(Args&&...)=delete;
    template<class... Args> void merge(Args&&...)=delete;
    void swap(KeyVersionedMap&)=delete;
};

class RasterSurface;
class SharedJob;
struct ConsoleBuffer;
struct GuiImeCandidateList {
    std::vector<std::u16string> text;
    std::uint32_t style{},selection{},page_start{},page_size{};
    bool operator==(const GuiImeCandidateList&) const=default;
};
struct GuiImeData {
    std::u16string text{},reading{},result{},result_reading{},original{};
    std::vector<std::uint8_t> attributes{},reading_attributes{};
    std::vector<std::uint32_t> clauses{},reading_clauses{},result_clauses{},result_reading_clauses{};
    std::vector<GuiImeCandidateList> candidate_lists;
    std::uint32_t cursor{},delta{};bool composing{},has_result{},native_edit{};
    bool operator==(const GuiImeData&) const=default;
};
struct GuiMessage {
    std::uint64_t window{}, wparam{}, lparam{};
    std::uint32_t message{};
    std::u16string text{};
    std::uint32_t modifiers{};
    std::vector<std::uint32_t> indices{};
    std::uint64_t thread{};
    std::array<std::int32_t,2> point{};bool has_point{};
    // 只供宿主佇列排序：posted、native input、generated paint、timer。
    std::uint8_t queue_priority{};bool queue_seen{};
    std::uint32_t queued_time{};std::array<std::int32_t,2> queued_cursor{};
    bool injected{};std::optional<std::uint64_t> input_extra{};
    std::optional<std::array<std::uint8_t,256>> keyboard_snapshot{};
    std::optional<GuiImeData> ime{};std::uint64_t ime_context{};
    std::uint64_t pointer_packet{};
    std::optional<desktop::Event> status_icon{};
    bool screen_point{}; // 原生／注入輸入保留事件當時的完整螢幕座標。
    std::uint64_t input_order{}; // 命中／啟用回呼完成後，仍保留輸入到達的先後。
    bool operator==(const GuiMessage&) const = default;
};
inline std::uint32_t gui_queue_category(const GuiMessage& message){
    if(message.pointer_packet)return 0x1000;
    if(message.queue_priority==1 && (message.message==0xff || message.message==0xfe))return 0x400;
    if(message.message==0x312 && message.keyboard_snapshot)return 0x80;
    if(message.queue_priority==2)return 0x20;
    if(message.queue_priority==3)return 0x10;
    if(message.queue_priority==1){if(message.message>=0x100 && message.message<=0x109)return 1;if(message.message==0x200)return 2;if(message.message>=0x201 && message.message<=0x20e)return 4;}
    return 0x108; // 明確 PostMessage 包含 QS_POSTMESSAGE 與 QS_ALLPOSTMESSAGE。
}
struct GuiFont {
    std::u16string face{};std::int32_t height{-13},width{},weight{400};
    std::uint8_t pitch_family{},charset{};bool italic{},underline{},strikeout{};
    std::shared_ptr<const void> native; // 已實現的私有字型；不依賴宿主名稱註冊。
    bool operator==(const GuiFont&) const = default;
};
struct NativeTextRun {
    GuiFont font;std::u16string text;std::int32_t x{},top{};unsigned glyphs{};
    bool operator==(const NativeTextRun&) const=default;
};
struct NativeTextShape {
    std::vector<NativeTextRun> runs;std::vector<std::int32_t> leading,trailing;
    std::vector<std::uint8_t> attributes;std::int32_t width{},height{},ascent{};bool missing{};
    bool operator==(const NativeTextShape&) const=default;
};
struct GuiScriptAnalysis {
    NativeTextShape shape;std::uint64_t dc{};std::uint32_t flags{};
    bool operator==(const GuiScriptAnalysis&) const=default;
};
struct GuiFontMetrics {
    std::int32_t height{16},ascent{13},descent{3},external_leading{},average_width{7},maximum_width{7};bool fixed_pitch{};
    bool operator==(const GuiFontMetrics&) const = default;
};
using GuiTransform=std::array<double,6>;
inline constexpr GuiTransform gui_identity{1,0,0,1,0,0};
struct GuiDraw {
    GuiTransform transform{gui_identity};bool device_coordinates{};
    enum class Kind { Fill, Text, Line, Rectangle, Icon, Ellipse } kind{};
    std::int32_t x{}, y{}, right{}, bottom{};
    std::uint32_t color{};
    std::int32_t pen_width{1},hatch{-1};
    std::uint32_t rop2{13};
    bool filled{};std::uint32_t fill_color{},pattern_background{0xffffff};bool pattern_opaque{};
    std::uint64_t pattern_brush{};std::array<std::int32_t,2> brush_origin{};std::uint32_t pattern_color{};bool pattern_monochrome{};
    std::u16string text;
    std::uint32_t cell_width{};
    bool clipped{};std::array<std::int32_t,4> clip{};
    std::vector<std::array<std::int32_t,4>> excluded{};
    std::vector<std::int32_t> advances{},advances_y{};
    bool glyph_indices{},rtl_reading{};
    std::vector<std::uint8_t> image{};std::uint32_t system_icon{};
    GuiFont font{};
    bool operator==(const GuiDraw&) const = default;
};
struct GuiClass {
    std::uint64_t procedure{}, instance{}, background{};
    std::uint16_t atom{};
    std::uint32_t extra_size{};
    std::uint64_t cursor{},icon{},small_icon{};std::uint32_t style{};
    std::u16string menu_name{};std::uint16_t menu_ordinal{};
    bool unicode{true};
    std::uint32_t class_extra_size{};
    std::vector<std::uint8_t> extra{};
    std::array<std::uint64_t,2> name_buffers{},menu_buffers{};
    std::vector<std::uint64_t> retired_buffers{};
    bool operator==(const GuiClass&) const = default;
};
struct GuiItem {
    std::uint64_t handle{}, parent{}, data{};
    std::u16string text{};
    std::uint32_t state{};
    std::vector<std::u16string> subitems{};
    std::set<std::uint32_t> callback_subitems{}, resolved_subitems{};
    std::int32_t image{-2};
    std::set<std::uint32_t> stored_subitems{};
    std::int32_t selected_image{-2},overlay{},indent{};
    // TreeView 的 callback 原值與本次顯示資料分開，未設 DI_SETITEM 時仍須再查詢。
    std::int32_t tree_children{};
    std::array<std::int32_t,3> tree_display{-2,-2,0};
    std::uint64_t revision{};
    bool operator==(const GuiItem&) const = default;
};
inline bool gui_list_text_callback(const GuiItem& item,std::uint32_t subitem){return !item.stored_subitems.contains(subitem) || item.callback_subitems.contains(subitem) || item.callback_subitems.contains(UINT32_MAX);}
struct GuiHeaderFilter {
    std::uint32_t type{0x8000U};
    std::u16string text;
    std::int32_t number{};
    std::array<std::uint16_t,8> date{};
    bool operator==(const GuiHeaderFilter&) const = default;
};
struct GuiListColumn {
    std::u16string text{};
    std::int32_t width{120},format{},subitem{};
    // Header 與 ListView 共用欄位契約；索引與顯示順序分開，重入以身分／版本核對。
    std::uint64_t identity{},revision{},data{},bitmap{};
    std::int32_t image{-1},order{-1};std::uint32_t state{};
    bool text_callback{},image_callback{};
    GuiHeaderFilter filter;
    std::u16string filter_display;
    bool operator==(const GuiListColumn&) const = default;
};
struct GuiHeaderControl {
    std::vector<std::array<std::int32_t,4>> rectangles;
    std::array<std::int32_t,4> overflow_rectangle{};
    std::int32_t overflow_item{-1};bool overflow_pressed{};
    std::vector<GuiIcon> bitmap_images;
    std::array<std::uint64_t,2> image_lists{};
    std::uint64_t inherited_font{};
    std::int32_t focus{-1},hot_divider{-1},bitmap_margin{6};
    std::uint64_t pressed{};std::int32_t origin{},original_width{},tracking_width{},drag_order{-1};
    bool tracking{},dragging{};
    std::uint64_t filter_editor{},filter_item{},filter_generation{},filter_deadline{};
    std::uint32_t filter_timeout{1000};
    GuiHeaderFilter filter_original;
    bool filter_editing{};
    bool operator==(const GuiHeaderControl&) const = default;
};
struct GuiImageList {
    std::int32_t width{},height{};
    std::uint32_t flags{};
    std::vector<GuiIcon> images{};
    std::uint32_t background{0xffffffffU};
    // ImageList_GetImageInfo 回傳的內部影像／遮罩條帶點陣圖；每次查詢依目前影像重建。
    std::uint64_t image_bitmap{},mask_bitmap{};
    bool operator==(const GuiImageList&) const = default;
};
struct GuiToolbarButton {
    std::int32_t image{-2},command{};
    std::uint8_t state{4},style{};
    std::uint64_t data{};
    std::u16string text{};
    std::uint64_t string_value{UINT64_MAX};
    std::uint32_t width{};
    std::array<std::int32_t,4> rectangle{};
    bool operator==(const GuiToolbarButton&) const = default;
};
struct GuiStatusPart {
    std::u16string text{};
    std::int32_t edge{-1};std::uint32_t flags{};
    std::uint64_t identity{},data{},icon_handle{};
    GuiIcon icon{};std::u16string tooltip{};
    bool operator==(const GuiStatusPart&) const = default;
};
struct GuiRebarBand {
    std::uint64_t child{},data{},bitmap{};
    std::uint32_t style{},id{},width{},min_width{},min_height{},height{},max_height{},integral{},ideal{},header{},foreground{},background{};
    std::int32_t image{-1},x{},y{};std::u16string text{};
    bool operator==(const GuiRebarBand&) const = default;
};
struct GuiAtom {
    std::u16string name;
    std::uint32_t references{};
    bool operator==(const GuiAtom&) const = default;
};
struct GuiProperty {
    std::uint64_t value{};
    bool owns_atom{};
    std::shared_ptr<SharedJob> atom_job;
    bool operator==(const GuiProperty&) const = default;
};
struct GuiPropertyPage {
    std::uint64_t descriptor{},instance{},resource{},procedure{},window{},handle{},callback{},ref_parent{};
    std::uint32_t flags{},width{},height{},phase{};std::u16string title{};bool dirty{},wide{true},used{};
    bool operator==(const GuiPropertyPage&)const=default;
};
struct GuiPropertySheet {
    std::vector<GuiPropertyPage> pages{};
    std::vector<std::uint64_t> scratch{};
    std::uint64_t notification{};std::size_t selected{},target{},index{};
    std::uint32_t phase{},action{},page_y{40};bool wide{true},saved{};
    bool operator==(const GuiPropertySheet&)const=default;
};
struct GuiDialogLayout {std::uint32_t width{},height{},base_x{8},base_y{16};std::u16string title;std::optional<GuiFont> font;};
// CredUIPromptForWindowsCredentials：對話框結束時寫回打包緩衝區、大小與「記住」狀態。
struct GuiCredentialPrompt {
    std::uint64_t output{},output_size{},save{};std::uint32_t flags{};
    std::optional<std::uint32_t> result;
    // 舊版 API 寫回呼叫者的 A／W 字串；新版 API 仍輸出 CoTaskMem 認證封裝。
    bool legacy{},wide{true},initial_save{};
    std::uint64_t user_output{},password_output{};
    std::uint32_t user_capacity{},password_capacity{};
    std::u16string target;
    std::string user_sid;
    bool operator==(const GuiCredentialPrompt&) const = default;
};
struct GuiCredentialConfirmation {
    std::string name,user_sid;
    std::vector<std::uint8_t> sealed;
    bool persist{};
    bool operator==(const GuiCredentialConfirmation&) const = default;
};
// CryptUIDlgCertMgr：清單項目對應的 DER；「檢視」時在同一對話框顯示詳細內容。
struct GuiCertificateManager {
    std::vector<std::vector<std::uint8_t>> certificates;
    bool operator==(const GuiCertificateManager&) const = default;
};
struct GuiShellDelete {
    std::vector<std::string> paths;
    std::uint64_t aborted_address{};
    std::uint32_t flags{};
    std::optional<std::uint32_t> result;
    bool operator==(const GuiShellDelete&) const = default;
};
struct GuiStoredVariant {
    std::uint16_t type{};
    std::array<std::uint8_t,16> scalar{};
    std::vector<std::uint8_t> payload;
    bool operator==(const GuiStoredVariant&) const = default;
};
struct GuiPropertyStore {
    std::uint64_t window{};std::uint32_t references{1};
    // PSCreateMemoryPropertyStore 的獨立記憶體屬性集；視窗屬性集仍存於 GuiWindow。
    bool memory{};std::map<std::array<std::uint8_t,20>,GuiStoredVariant> properties;
    bool operator==(const GuiPropertyStore&) const = default;
};
struct GuiShellItem {
    std::u16string path;
    std::uint32_t references{1};
    bool operator==(const GuiShellItem&) const = default;
};
struct GuiShellItemEnumerator {
    std::vector<std::u16string> paths;
    std::size_t cursor{};
    std::uint32_t references{1};
    bool operator==(const GuiShellItemEnumerator&) const = default;
};
struct GuiShellItemOperation {
    bool binding{};
    std::uint64_t item{},scratch{},output{};
    std::uint32_t hint{};
    bool operator==(const GuiShellItemOperation&) const = default;
};
struct GuiMdiClient {
    std::uint64_t window_menu{},active{};
    std::uint32_t first_child{};
    std::vector<std::uint64_t> children{};
    std::u16string frame_title{};
    bool maximized{};
    std::array<std::int32_t,2> scroll{};
    bool operator==(const GuiMdiClient&) const = default;
};
struct GuiLinkItem {
    std::u16string id,url;
    std::uint32_t start{},length{},state{2};
    bool operator==(const GuiLinkItem&) const = default;
};
struct GuiLinkRun {
    std::uint32_t start{},length{};std::int32_t link{-1};
    std::array<std::int32_t,4> rectangle{};
    bool operator==(const GuiLinkRun&) const = default;
};
struct GuiLinkLayout {
    std::vector<GuiLinkRun> runs;
    std::uint32_t width{},height{};
    bool operator==(const GuiLinkLayout&) const = default;
};
inline constexpr std::size_t gui_control_notification_bytes=4352; // 含完整 NMLINK 的 WCHAR URL 陣列。
struct GuiTabItem {
    std::uint64_t identity{};
    std::u16string text;
    std::vector<std::uint8_t> data;
    std::int32_t image{-1};
    std::uint32_t state{};
    bool rtl{};
    bool operator==(const GuiTabItem&) const = default;
};
struct GuiTabControl {
    std::vector<GuiTabItem> items;
    std::vector<std::array<std::int32_t,4>> rectangles;
    std::array<std::int32_t,4> display{};
    std::uint64_t image_list{},tooltips{},revision{};
    std::uint32_t extra_bytes{UINT32_MAX},extended_style{},rows{};
    std::int32_t focus{-1},pressed{-1},minimum_width{-1},item_width{},item_height{},padding_x{6},padding_y{3};
    std::int64_t scroll{};
    bool focused{};
    bool operator==(const GuiTabControl&) const = default;
};
struct GuiTooltipTool {
    std::uint64_t identity{},owner{},id{},instance{},text_pointer{},data{};
    std::uint32_t flags{};
    std::array<std::int32_t,4> rectangle{};
    std::u16string text;
    bool callback{},wide{true};
    bool operator==(const GuiTooltipTool&) const = default;
};
struct GuiTooltip {
    std::vector<GuiTooltipTool> tools;
    std::vector<std::u16string> lines;
    std::u16string title;
    std::uint64_t current{},shown{},timer{},revision{},icon{};
    std::array<std::int32_t,2> point{};
    std::array<std::int32_t,4> margin{};
    std::array<std::uint32_t,4> delays{0,100,5000,500};
    std::int32_t maximum_width{-1};
    std::uint32_t background{0xe1ffff},color{},line_height{16},title_height{};
    bool active{true},tracking{},suppressed{},reshow{},rtl{};
    bool operator==(const GuiTooltip&) const = default;
};
using GuiSystemTime = std::array<std::uint16_t,8>;
struct GuiDateNotice {
    std::int32_t code{};std::uint32_t flags{};GuiSystemTime first{},last{};
    std::uint32_t old_view{},new_view{};
    std::uint64_t revision{};
    bool operator==(const GuiDateNotice&) const = default;
};
struct GuiDateCell {
    GuiSystemTime date{};std::array<std::int32_t,4> rect{};
    std::u16string text;std::int32_t action{};bool selected{},trailing{},today{},disabled{},bold{};
    GuiSystemTime end{};std::int32_t calendar{},row{-1},column{-1};
    char16_t format_symbol{};std::uint32_t format_length{};
    bool operator==(const GuiDateCell&) const = default;
};
struct GuiDateCallbackField {
    std::u16string format,text;std::array<std::int32_t,2> maximum{};
    bool operator==(const GuiDateCallbackField&) const = default;
};
struct GuiCalendarLocale {
    std::u16string short_date,long_date,time;
    std::array<std::u16string,12> months,short_months,genitive_months,short_genitive_months;
    std::array<std::u16string,7> weekdays,short_weekdays,narrow_weekdays;
    std::array<std::u16string,2> designators;
    std::uint32_t first_day{6},first_week{};
    bool operator==(const GuiCalendarLocale&) const = default;
};
struct GuiCalendar {
    GuiSystemTime value{},last{},today{},minimum{},maximum{},anchor{};
    std::uint32_t range_flags{},maximum_selection{7},month_delta{},first_day{6},month_style{};
    std::uint32_t view{},border{4};bool border_explicit{};
    std::uint32_t minimum_width{196},minimum_height{190},calendar_count{1},grid_columns{1};
    std::uint64_t month_window{},month_font{},picker_owner{},edit_window{},updown_window{};
    std::uint64_t picker_revision{},edit_generation{};
    std::int32_t display_year{},display_month{},field{3},pressed{-1};
    std::int32_t input_field{-1};std::uint32_t input_value{},input_digits{};GuiSystemTime input_previous{};
    bool picker{},none{},open{},open_request{},close_request{},dragging{},first_day_explicit{};
    bool editing{},edit_request{},commit_request{},cancel_request{};
    std::u16string format,short_format,long_format,time_format;
    std::shared_ptr<const GuiCalendarLocale> locale;
    std::u16string name_input;
    std::u16string callback_schema;std::vector<GuiDateCallbackField> callback_fields;
    std::uint64_t callback_revision{};std::uint32_t callback_depth{},callback_key{};
    bool callback_query_dirty{},callback_text_dirty{};
    std::array<std::uint32_t,6> colors{0xffffff,0,0xd77800,0xffffff,0xffffff,0x808080};
    std::vector<GuiDateCell> cells;std::vector<GuiDateNotice> notices;
    std::vector<std::uint32_t> day_states;std::int32_t day_state_month{-1};std::uint64_t day_state_revision{};
    bool operator==(const GuiCalendar&) const = default;
};
struct GuiWindow {
    std::shared_ptr<ConsoleBuffer> console;
    std::map<std::array<std::uint8_t,20>,GuiStoredVariant> shell_properties;
    std::optional<GuiShellDelete> shell_delete;
    std::optional<GuiCredentialPrompt> credential_prompt;
    std::optional<GuiCertificateManager> certificate_manager;
    std::map<std::pair<std::uint64_t,std::uint16_t>,GuiProperty> properties;
    std::optional<std::u16string> theme_application,theme_classes;
    bool dark_mode_allowed{};
    std::uint32_t display_affinity{};
    std::optional<std::u16string> shutdown_reason;
    std::vector<std::uint32_t> layered_pixels; // 預乘 alpha 的客體 COLORREF；供分層視窗及 headless 讀回。
    bool layered_initialized{};std::uint8_t layered_alpha{255};std::uint32_t layered_color{};
    // SetWindowLong 的 frame style 在 SWP_FRAMECHANGED 前仍採舊 nonclient 幾何。
    std::optional<std::array<std::uint32_t,4>> frame_insets;
    std::uint64_t thread{1},desktop_object{2};
    std::uint64_t procedure{}, instance{}, user_data{}, create_data{}, class_atom{};
    bool unicode{true};
    bool shell_desktop{};
    std::optional<GuiMdiClient> mdi{};
    bool mdi_child{},mdi_ready{},mdi_active{};
    std::optional<std::uint32_t> touch_flags;
    std::optional<std::pair<std::uint32_t,std::uint32_t>> session_notification; // flags／訂閱引用次數。
    std::uint32_t width{}, height{}, style{}, ex_style{}, text_color{}, background_mode{2};
    std::u16string title;
    std::u16string link_text;
    std::vector<GuiLinkItem> links;
    GuiLinkLayout link_layout;
    bool links_initialized{};std::int32_t link_pressed{-1};
    std::vector<GuiDraw> drawing;
    bool visible{}, invalid{true}, destroying{}, initial_size_pending{};
    // ShowOwnedPopups(FALSE) 經 WM_SHOWWINDOW 隱藏的擁有者彈出視窗，等待 SW_PARENTOPENING 恢復。
    bool owned_popup_hidden{};
    bool paint_frame{},paint_erase{},internal_paint{};
    std::optional<GuiRegion> window_region;
    std::u16string class_name{};
    std::uint64_t parent{}, owner{}, id{}, dialog_proc{}, font{0xb011},last_active_popup{};
    std::int32_t x{}, y{}, selection{-1};
    std::uint32_t edit_start{},edit_end{};bool edit_modified{};
    std::optional<std::u16string> edit_undo{};
    std::uint32_t check{}, text_limit{32767};
    std::uint64_t dpi_context{UINT64_MAX};
    bool nonclient_dpi_scaling{};
    std::uint16_t password_character{u'*'};
    std::uint32_t dialog_base_x{8},dialog_base_y{16};bool owned_font{};GuiFont control_font{};
    std::uint64_t spin_buddy{};
    std::int32_t spin_min{},spin_max{100},spin_position{};std::uint32_t spin_base{10};
    std::int32_t progress_min{},progress_max{100},progress_position{},progress_step{10};
    std::int32_t track_min{},track_max{100},track_position{},track_line{1},track_page{},track_tick_frequency{1};
    std::vector<std::int32_t> track_ticks{};
    bool track_ticks_cleared{};
    std::uint32_t progress_state{1};bool progress_marquee{};
    std::uint32_t progress_bar_color{0xff000000U},progress_background_color{0xff000000U};
    bool enabled{true}, redraw{true}, builtin{}, dialog{}, modal{}, ended{};
    bool modal_owner_was_enabled{true};
    bool button_pressed{},button_armed{};
    std::uint64_t dialog_result{}, default_id{}, icon{},resource_icon{},window_icon{},window_small_icon{};
    GuiIcon application_icon{};
    std::vector<std::uint8_t> extra{};
    std::vector<GuiItem> items{};
    std::uint64_t tree_revision{};
    std::optional<GuiTabControl> tab;
    std::optional<GuiTooltip> tooltip;
    std::vector<GuiListColumn> columns{};
    std::optional<GuiHeaderControl> header;
    std::optional<GuiCalendar> calendar;
    std::uint64_t list_header{},header_list{};
    std::uint32_t list_extended_style{},list_callback_mask{};
    std::array<std::uint64_t,3> list_image_lists{};
    std::vector<GuiIcon> list_icons{};
    bool list_unicode{true};
    std::uint64_t menu{};
    std::vector<GuiToolbarButton> toolbar_buttons{};
    std::vector<std::u16string> toolbar_strings{};
    std::uint32_t toolbar_struct_size{32},toolbar_button_width{28},toolbar_button_height{26},toolbar_bitmap_width{16},toolbar_bitmap_height{16};
    std::uint32_t toolbar_requested_width{},toolbar_requested_height{},toolbar_min_width{},toolbar_max_width{4096},toolbar_text_rows{1};
    std::uint32_t toolbar_content_width{},toolbar_content_height{};
    GuiFont toolbar_font{};
    std::vector<GuiStatusPart> status_parts{};
    bool status_simple{};GuiStatusPart status_simple_part{};
    std::uint32_t status_min_height{},status_background{0xffffffffU};
    std::uint32_t show_command{1};bool restore_maximized{};
    std::optional<std::array<std::int32_t,4>> normal_rect;
    std::vector<GuiRebarBand> rebar_bands{};
    std::u16string control_class{};
    std::uint64_t combo_control{},edit_control{},combo_owner{},list_control{},combo_list_owner{};
    std::uint64_t calendar_owner{},header_owner{};
    std::uint32_t combo_drop_height{100};
    bool combo_dropped{};
    std::uint32_t combo_extended_style{};
    bool combo_edit_changed{};
    GuiItem combo_edit_item{};
    GuiMessage modal_message{};
    std::uint64_t modal_message_buffer{};
    bool operator==(const GuiWindow&) const = default;
};
struct GuiWindowCreation {
    std::uint64_t handle{};GuiWindow window{};bool wide{true};
    std::uint64_t parameter{};std::optional<std::uint64_t> class_pointer{},name_pointer{};
    std::vector<std::uint8_t> parameter_bytes{};
    bool owned_menu{};
    bool operator==(const GuiWindowCreation&) const = default;
};
struct GuiCreationOperation {
    std::vector<GuiWindowCreation> pending;
    std::vector<std::uint64_t> completed,scratch;
    std::size_t index{},abort_end{};bool skip_failed_child{};
    std::uint64_t init_parameter{},cbt_data{},nc_data{},thread{};
    bool operator==(const GuiCreationOperation&) const = default;
};
inline const std::u16string& gui_control_class(const GuiWindow& window){return window.control_class.empty()?window.class_name:window.control_class;}
inline std::array<std::int32_t,4> gui_status_rectangle(const GuiWindow& window,std::size_t part){
    auto left=part && part!=255?window.status_parts[part-1].edge:0;
    auto right=part==255 || window.status_parts[part].edge<0?static_cast<std::int32_t>(window.width):window.status_parts[part].edge;
    if(window.ex_style&0x400000U){const auto width=static_cast<std::int32_t>(window.width);const auto previous=left;left=width-right;right=width-previous;}
    return {left,0,right,static_cast<std::int32_t>(window.height)};
}
struct GuiControlNotification {
    std::uint64_t window{},item{};
    std::uint32_t subitem{};
    bool display{},wide{};
    bool operator==(const GuiControlNotification&) const = default;
};
struct GuiBitmap {
    std::uint32_t width{},height{};std::vector<std::uint32_t> pixels{};std::uint64_t resource_id{};
    // DIB bits 屬於客體配置；pixels 保存最近同步的 COLORREF/alpha，不暴露宿主指標。
    std::uint64_t dib_address{};std::uint32_t stride{};std::uint16_t bits_per_pixel{32};std::int32_t signed_height{};bool dib_upload_pending{},raw_alpha{};
    std::uint64_t dib_view{},dib_section{};std::uint32_t dib_offset{};
    std::uint32_t dib_compression{};std::array<std::uint32_t,4> dib_masks{0xff0000,0xff00,0xff,0xff000000};
    std::vector<std::uint32_t> dib_palette{}; // COLORREF；RGBQUAD 與邏輯調色盤在 API 邊界轉換。
    bool operator==(const GuiBitmap&) const = default;
};
GuiBitmap decode_dib_bitmap(std::span<const std::uint8_t> bytes);
struct GuiPathPoint {double x{},y{};std::uint8_t type{};bool operator==(const GuiPathPoint&)const=default;};
struct GuiPath {unsigned state{};bool new_figure{true};std::vector<GuiPathPoint> points;bool operator==(const GuiPath&)const=default;};
struct GuiDc {
    GuiPath path;unsigned poly_fill_mode{1},arc_direction{1};float miter_limit{10};
    std::uint32_t graphics_mode{1},map_mode{1};GuiTransform world{gui_identity};
    std::array<std::int32_t,2> window_origin{},viewport_origin{},window_extent{1,1},viewport_extent{1,1};
    std::optional<GuiRegion> application_clip,meta_clip;
    bool display{};
    std::uint64_t bitmap{},font{0xb011},pen{0xb007},brush{0xb000},palette{0xb00f},palette_generation{};std::uint32_t color{},background{0xffffff},mode{2},alignment{};
    std::int32_t x{},y{};
    std::uint32_t dc_pen_color{},dc_brush_color{0xffffff},rop2{13};
    std::uint32_t stretch_mode{1};std::array<std::int32_t,2> brush_origin{};
    bool clipped{};std::array<std::int32_t,4> clip{};
    std::vector<std::array<std::int32_t,4>> excluded{};
    // 有框視窗的 GetWindowDC：外框由原生視窗繪製，視窗座標以 viewport 原點平移到工作區表面。
    bool window_frame{};
    // SetBoundsRect／GetBoundsRect 的裝置座標累積矩形；未啟用時不追蹤。
    bool bounds_enabled{};std::array<std::int32_t,4> bounds{};
    // SetLayout／GetLayout 的版面旗標；目前只接受由左至右版面。
    std::uint32_t layout{};
    bool operator==(const GuiDc&) const = default;
};
struct GuiMouseTracking {
    std::uint64_t window{},deadline{};std::uint32_t flags{},hover_time{};std::array<std::int32_t,2> anchor{};
    bool operator==(const GuiMouseTracking&)const=default;
};
struct GuiMdiMove {
    enum class Stage { Enter, Message, Apply, Dispatch, Finish } stage{Stage::Enter};
    std::uint64_t window{},message_buffer{},capture{},thread{};
    std::array<std::int32_t,4> original{},rectangle{};
    std::array<std::int32_t,2> anchor{};
    std::uint32_t edge{};
    bool sizing{},cancel{};
    bool operator==(const GuiMdiMove&) const = default;
};
struct GuiTimer {std::uint64_t deadline{},interval{},procedure{},thread{},coalescing_tolerance{};bool operator==(const GuiTimer&) const = default;};
struct GuiThemeData {std::u16string class_name;std::uint64_t window{};std::uint32_t dpi{96},flags{};bool operator==(const GuiThemeData&)const=default;};
struct GuiThemeMetrics {
    std::array<std::int32_t,2> minimum{},intrinsic{};
    std::array<std::int32_t,4> content{};
    std::uint32_t text_color{},background_color{};bool transparent{},fixed_size{};
};
struct GuiThemeText {std::uint64_t dc{};GuiDc saved{};std::optional<std::array<std::uint32_t,2>> window_text{};bool allow_zero{};bool operator==(const GuiThemeText&)const=default;};
struct GuiGrayString {
    std::uint64_t dc{},temporary_dc{},bitmap{},brush{},thread{};
    std::int32_t count{},x{},y{},width{},height{};
    std::uint64_t source_dc{},scratch{};
    std::uint32_t draw_state_flags{};
    bool draw_state{};
    bool operator==(const GuiGrayString&) const = default;
};
struct GuiMenuItem {std::uint64_t id{};std::uint32_t flags{};std::u16string text{};std::uint64_t data{},bitmap{},checked_bitmap{},unchecked_bitmap{};std::uint32_t command{UINT32_MAX};std::uint64_t mdi_owner{};bool operator==(const GuiMenuItem&) const = default;};
struct GuiMenuInfo {std::uint32_t style{},help_id{};std::uint64_t data{};bool operator==(const GuiMenuInfo&) const = default;};
struct GuiMenuSelection {std::uint64_t command{},menu{};std::uint32_t position{};};
struct GuiMenuHighlight {std::uint64_t menu{};std::uint32_t position{};bool mouse{};};
struct GuiPopupItem {std::uint64_t id{};std::uint32_t flags{};std::u16string text{};std::vector<GuiPopupItem> children{};std::uint64_t menu{};std::uint32_t position{};GuiIcon bitmap{},checked_bitmap{},unchecked_bitmap{};bool custom_checkmarks{};bool operator==(const GuiPopupItem&) const = default;};
struct GuiMenuTrack {std::uint64_t menu{},owner{},selection{},thread{};std::uint32_t flags{};std::int32_t x{},y{};std::vector<std::uint64_t> initialized{};bool selected{},active{true},cancelled{},idle_notified{};std::uint64_t root_menu{},selected_menu{};std::uint32_t root_index{},selected_position{};bool main_bar{};void cancel(){cancelled=true;selected=false;}bool operator==(const GuiMenuTrack&) const = default;};
struct GuiSentMessage {
    GuiMessage message{};
    std::uint64_t sender{},receiver{},sender_stack{},sender_rip{},result{},resume_modal{};
    std::uint64_t deadline{UINT64_MAX},result_pointer{};std::uint32_t timeout_flags{};
    std::uint32_t error{};bool wide{},active{},done{},timed{},abandoned{};
    enum class Kind { Synchronous, Notify, Callback } kind{Kind::Synchronous};
    std::uint64_t callback{},callback_data{},power_owner{},device_owner{};bool resume_wait{};
    std::uint64_t power_notification{};
    std::optional<GuestCallback> rpc{};
    std::uint64_t rpc_error_pointer{},proxy_operation{};
    bool operator==(const GuiSentMessage&) const = default;
};
struct GuiSendCallback {
    std::uint64_t thread{},procedure{},window{},data{},result{};std::uint32_t message{};
    bool operator==(const GuiSendCallback&) const = default;
};
struct GuiAsyncSend {
    GuiMessage message{};std::vector<std::uint64_t> windows{};std::size_t index{};
    std::uint64_t thread{},callback{},data{};bool wide{};
    GuiSentMessage::Kind kind{GuiSentMessage::Kind::Notify};
    bool operator==(const GuiAsyncSend&) const = default;
};
struct GuiSocketWatch {std::uint64_t window{},event_handle{},generation{};std::uint32_t recorded{};std::array<std::uint32_t,10> errors{};std::uint32_t message{},events{},armed{};bool connecting{},connected{};bool operator==(const GuiSocketWatch&) const = default;};
struct GuiGlobalMemory {
    std::uint64_t address{},size{};std::uint32_t flags{},locks{};bool clipboard_owned{};
    bool operator==(const GuiGlobalMemory&) const = default;
};
struct GuiCommonDialog {
    enum class Kind { Font, Colour, OpenFile, SaveFile, Busy } kind{};
    std::uint64_t request{};std::uint32_t flags{};bool wide{};
    std::uint64_t hook{},blocking_task{},dialog_output{};bool owner_enabled{true};
    std::u16string directory{}, extension{}, overwrite_path{};
    std::vector<std::u16string> filters{};
    bool operator==(const GuiCommonDialog&) const = default;
};
struct GuiShellOpenWith {
    std::shared_ptr<HostFile> file;std::string uri;
    std::vector<shell::Application> applications;bool execute{},owner_enabled{};
    bool operator==(const GuiShellOpenWith&) const = default;
};
struct GuiAppBar {
    std::uint32_t callback{},edge{};std::array<std::int32_t,4> rectangle{};
    bool positioned{},autohide{},active{};
    bool operator==(const GuiAppBar&) const = default;
};
struct GuiShellBrowse {
    std::uint64_t request{},callback{},callback_data{},selection_pidl{};
    std::uint32_t flags{};bool wide{};
    std::u16string directory{},root{},status{};
    bool operator==(const GuiShellBrowse&) const = default;
};
struct GuiTooltipOperation {
    enum class Kind { Text, Show, Pop, Relay } kind{};
    GuiTooltipTool tool;
    std::optional<GuiTooltipTool> previous;
    std::vector<std::uint64_t> targets;
    std::uint64_t revision{},return_value{};
    std::array<std::int32_t,2> position{};
    bool return_fixed{},was_visible{};
    bool operator==(const GuiTooltipOperation&) const = default;
};
struct GuiControlOperation {
    GuiMessage message{};std::uint64_t thread{},buffer{},item{},callback{},parameter{};
    bool wide{},sort{},sort_indices{},pending{};std::uint32_t subitem{};
    std::vector<std::uint64_t> identities{};std::vector<std::uint32_t> order{},output{};
    std::size_t width{1},start{},left{},right{},left_end{},right_end{},out{};
    bool combo_end{},owner_draw{},button_click{},tab_select{},tree_select{},tree_delete{},edit_paste{},clipboard_opened{};
    std::vector<GuiItem> removed_items;
    std::uint32_t phase{};
    std::u16string text{};
    std::optional<GuiTooltipOperation> tooltip;
    struct Style {
        std::int32_t index{};std::uint32_t previous{};
        bool committed{};
        bool operator==(const Style&) const = default;
    };
    std::optional<Style> style;
    struct Enable {
        bool enabled{},previous{};
        bool operator==(const Enable&) const = default;
    };
    std::optional<Enable> enable;
    struct Header {
        enum class Kind { Set, Click, Track, Drag, Display, Format, Filter, Paint } kind{};
        GuiListColumn candidate{};std::uint32_t mask{};
        std::int32_t index{-1},button{},code{},order{-1};
        std::uint64_t revision{};bool committed{};
        std::uint32_t stage{};std::uint64_t generation{},return_value{},filter_buffer{};
        struct Notice {std::int32_t code{},index{-1};std::uint64_t identity{};bool operator==(const Notice&) const = default;};
        std::vector<Notice> notices;
        std::size_t next{};
        bool edit{},cancel{},close{};
        std::shared_ptr<RasterSurface> paint_surface;
        std::uint64_t paint_dc{},paint_bitmap{},previous_dc_object{};
        std::array<std::int32_t,4> paint_rectangle{};
        std::uint32_t paint_flags{},item_flags{},previous_text_color{},previous_background_mode{};
        bool previous_dc_object_present{},custom_paint{},drag_image{};
        bool operator==(const Header&) const = default;
    };
    std::optional<Header> header;
    struct Tree {
        enum class Kind { Expand, Display } kind{};
        std::uint32_t stage{},mask{},action{},previous_state{};
        std::uint64_t return_value{1};
        bool native{},pending{},notify{};
        bool operator==(const Tree&) const = default;
    };
    std::optional<Tree> tree;
    struct Calendar {
        std::vector<GuiDateNotice> notices;std::uint64_t return_value{},popup{};
        std::uint64_t resize_window{};std::uint32_t resize_width{},resize_height{};
        std::uint64_t text_buffer{},revision{},edit_generation{};
        std::uint64_t locale_buffer{},locale_revision{};
        bool edit{},commit{},cancel{};
        bool callbacks{},callback_active{},callback_query{},callback_query_done{},callback_wide{};
        std::uint64_t callback_revision{},callback_value_revision{};
        std::size_t callback_next{};std::uint32_t callback_key{},callback_field{};
        std::uint32_t stage{};std::size_t next{};bool open{},close{};
        bool operator==(const Calendar&) const = default;
    };
    std::optional<Calendar> calendar;
    bool operator==(const GuiControlOperation&) const = default;
};
struct GuiDropTarget {std::uint64_t object{},thread{},generation{};bool operator==(const GuiDropTarget&) const = default;};
struct GuiWinEventHook {
    std::uint64_t procedure{},owner{},thread{};std::uint32_t minimum{},maximum{},flags{};
    bool operator==(const GuiWinEventHook&) const=default;
};
struct GuiWinEvent {
    std::uint64_t hook{},window{},thread{};std::uint32_t event{},object{},child{},time{};
    bool operator==(const GuiWinEvent&) const=default;
};
struct GuiHook {
    std::int32_t type{};
    std::uint64_t procedure{},module{},thread{},owner{};bool wide{true};
    bool operator==(const GuiHook&) const = default;
};
struct GuiCbtAction {
    std::string symbol{};unsigned arguments{};std::uint64_t thread{},data{},veto_result{};
    bool focus_activation{};
    std::vector<std::uint64_t> scratch{};std::array<std::uint32_t,4> rectangle{};
    bool operator==(const GuiCbtAction&) const=default;
};
struct GuiImmContext {
    bool operator==(const GuiImmContext&) const = default;
    std::uint64_t thread{};bool default_context{},open{};
    std::uint32_t conversion{},sentence{};GuiImeData data{};
    std::array<std::uint8_t,28> composition{};
    std::array<std::uint8_t,92> font{};
    std::map<std::uint32_t,std::array<std::uint8_t,32>> candidates;
    std::uint64_t storage{};std::uint32_t locks{};
    std::array<std::uint64_t,5> buffers{};
    std::vector<std::uint8_t> snapshot;
};
struct GuiImmBuffer {
    bool operator==(const GuiImmBuffer&) const = default;
    std::uint64_t storage{},context{};std::uint32_t size{},locks{},kind{};
    std::vector<std::uint8_t> snapshot;
};
struct GuiMetafile {
    std::vector<std::uint8_t> bytes;
    std::vector<GuiDraw> draws;
    std::optional<GuiRect> frame,bounds;
    std::u16string description;
    std::uint32_t records{};
    bool imported{};
    bool operator==(const GuiMetafile&) const = default;
};
struct GuiClipboardFormat {
    std::uint32_t format{},aspect{1},tymed{};
    std::int32_t index{-1};
    std::vector<std::uint8_t> device;
    bool operator==(const GuiClipboardFormat&) const = default;
};
struct GuiClipboardEntry {
    GuiClipboardFormat format;
    std::uint32_t type{};
    std::vector<std::uint8_t> bytes;
    std::optional<GuiBitmap> bitmap;
    std::optional<GuiMetafile> metafile;
    bool operator==(const GuiClipboardEntry&) const = default;
};
struct GuiClipboardObject {
    std::uint32_t references{1};
    std::uint64_t provider{},thread{};
    std::vector<GuiClipboardEntry> entries;
    std::size_t cursor{};
    bool enumerator{};
    bool operator==(const GuiClipboardObject&) const = default;
};
struct GuiClipboardOperation {
    enum class Kind { Set,Get,Flush,Render,Read,Empty };
    enum class Stage { Start,Acquire,Enumerate,Next,FreeDevice,ReleaseEnumerator,Window,
        Medium,StreamStat,StreamTell,StreamSeek,StreamRead,StreamRestore,
        StorageLock,StorageCreate,StorageCopy,StorageCommit,StorageGlobal,ReleaseMedium,
        ReadStream,ReadLock,ReadStorage,ReadCopy,Cleanup,Done };
    Kind kind{};Stage stage{};
    std::uint64_t thread{},station{},generation{},source{},source_thread{},provider{},enumerator{},buffer{},output{},window{};
    std::uint64_t medium{},lock_bytes{},storage{},global{},position{},length{},offset{};
    std::uint32_t status{},requested{},type{};
    std::size_t index{};
    bool acquired{},medium_owned{},transferred{},position_valid{},here{};
    std::vector<GuiClipboardEntry> entries;
    std::vector<std::pair<std::uint32_t,std::uint64_t>> resources;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> releases;
    std::map<std::uint32_t,std::pair<std::uint32_t,std::uint64_t>> prepared;
    bool operator==(const GuiClipboardOperation&) const = default;
};
struct GuiStationNamespace {
    std::map<std::uint16_t,GuiAtom> global_atoms;
    std::map<std::u16string,std::uint16_t> global_atom_names;
    std::map<std::uint32_t,std::uint64_t> clipboard{};
    std::map<std::u16string,std::uint32_t> clipboard_formats{};
    std::map<std::uint32_t,std::u16string> clipboard_format_names;
    std::vector<std::uint32_t> clipboard_order;
    std::map<std::uint32_t,std::uint32_t> clipboard_types;
    std::uint64_t clipboard_ole_object{},clipboard_ole_thread{},clipboard_ole_generation{};
    std::vector<GuiClipboardEntry> clipboard_ole_entries;
    std::set<std::uint64_t> clipboard_listeners;
    std::uint32_t clipboard_sequence{1};std::optional<std::uint64_t> clipboard_native_sequence;
    std::uint64_t clipboard_poll_deadline{},scheduler_poll_deadline{};
    std::uint64_t network_pump_ticks{}; // 上次全量抽取 socket 監看的單調時鐘（100 ns），見 pump_gui_network。
    bool clipboard_stale{},clipboard_dirty{};
    std::uint64_t clipboard_open_thread{},clipboard_open_window{},clipboard_owner{};
    std::uint32_t clipboard_next_format{0xc000};std::optional<std::u16string> clipboard_native_text{};
    bool operator==(const GuiStationNamespace&) const = default;
};
struct GuiUserObject {
    std::u16string name;
    std::uint64_t station{}; // 0 為 window station；desktop 指向 station object。
    std::uint32_t flags{};
    bool initial{};
    std::vector<std::uint8_t> security_descriptor{};
    bool operator==(const GuiUserObject&) const = default;
};
struct GuiUserHandle {
    std::uint64_t object{};
    std::uint32_t access{};
    bool operator==(const GuiUserHandle&) const = default;
};
struct GuiHotKey {
    std::uint64_t window{},thread{},desktop{};std::uint32_t id{},modifiers{},key{};bool pressed{};
    bool operator==(const GuiHotKey&)const=default;
};
struct GuiPrintDialog {
    std::uint64_t request{},general{},callback_unknown{},callback{},site{},services{},result{},notification{};
    std::uint32_t flags{},minimum{},maximum{},range_capacity{},phase{},action{},selected{},target{};
    std::size_t index{};bool wide{true},owner_enabled{true},applied{},closing{};
    PrinterSettings settings{};
    std::vector<std::pair<std::uint32_t,std::uint32_t>> ranges{};
    std::vector<GuiPropertyPage> pages{};
    std::vector<std::uint64_t> scratch{};
    GuiMessage pending{};
    bool operator==(const GuiPrintDialog&)const=default;
};
struct GuiPrintServices {
    std::uint32_t references{1};std::uint64_t window{};bool wide{true};
    bool operator==(const GuiPrintServices&)const=default;
};
// 宿主輸入先回呼客體命中／啟用契約，再進入一般訊息佇列。
struct GuiMouseInput {
    enum class Stage { Hit, Activate, Commit } stage{Stage::Hit};
    GuiMessage message;
    std::uint64_t thread{},root{},modal{};
    std::int32_t hit{1};bool active{},wait_message{},eat{},native_activation{};
    std::vector<std::uint64_t> tested;
    bool operator==(const GuiMouseInput&)const=default;
};
struct GuiMoveSize {
    enum class Stage { Initialize, Read, Rect, Position, Dispatch, Restore, Release, Exit, Quit } stage{Stage::Initialize};
    std::uint64_t window{},thread{},buffer{},capture_generation{};
    std::array<std::int32_t,4> original{};
    std::array<std::int32_t,2> start{},point{};
    unsigned edge{};bool keyboard{},cancel{},quit{};std::uint32_t quit_code{};
    bool operator==(const GuiMoveSize&)const=default;
};
struct GuiWindowPosition {
    std::uint64_t window{},after{},data{};
    std::int32_t x{},y{};
    std::uint32_t width{},height{},flags{};
    std::optional<std::array<std::uint32_t,4>> frame;
    bool calculated{},changed{};
    bool operator==(const GuiWindowPosition&) const = default;
};
struct GuiWindowPositionBatch {
    std::uint64_t thread{},parent{};
    std::vector<GuiWindowPosition> items;
    bool operator==(const GuiWindowPositionBatch&) const = default;
};
struct GuiWindowPositionOperation {
    std::uint64_t thread{},activate{};
    std::vector<GuiWindowPosition> items;
    std::vector<std::uint64_t> scratch;
    std::size_t index{};
    unsigned phase{};
    bool operator==(const GuiWindowPositionOperation&) const = default;
};
struct GuiState : GuiStationNamespace {
    // 最多五筆待確認認證；僅保存綁定虛擬 SID 的加密資料，不保留額外明文密碼。
    std::vector<GuiCredentialConfirmation> credential_confirmations;
    std::shared_ptr<const GuiCalendarLocale> calendar_locale;
    std::uint64_t calendar_locale_revision{};
    std::map<std::uint64_t,GuiClipboardObject> clipboard_objects;
    std::map<std::uint64_t,GuiClipboardOperation> clipboard_operations;
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::uint64_t> clipboard_windows;
    std::uint64_t clipboard_data_vtable{},clipboard_enum_vtable{};
    std::map<std::uint64_t,GuiWindowPositionBatch> deferred_positions;
    std::map<std::uint64_t,GuiWindowPositionOperation> position_operations;
    desktop::Icons status_icons;
    desktop::Attention attention;
    std::set<std::uint64_t> native_attention_windows;
    std::optional<std::uint32_t> desktop_notification_state;
    std::map<std::uint64_t,GuiUserObject> user_objects;
    std::map<std::uint64_t,GuiUserHandle> user_handles;
    std::map<std::uint64_t,GuiStationNamespace> station_namespaces;
    std::uint64_t station_handle{},initial_desktop_handle{},next_user_object{3},input_desktop{2};
    bool foreground_locked{};
    std::uint32_t foreground_grant{};

    std::map<std::uint64_t,GuiScriptAnalysis> script_analyses;
    std::map<std::uint64_t,GuiFont> script_caches;
    std::map<std::uint64_t,GuiMetafile> emf_recorders,metafiles;

    std::array<std::uint64_t,2> resource_peaks{};
    // 只計算目前虛擬程序真正持有的 handle；共用 stock 物件沒有配置於這些容器。
    // 兩張 DC 表的鍵集合未變時沿用上次的聯集大小；每次 API 前後都會觀察峰值。
    // 快取只是計算捷徑，不屬於 GUI 狀態；狀態比較時一律視為相等。
    struct DcUnionCache {std::uint64_t versions[2]{};std::size_t count{};bool operator==(const DcUnionCache&) const noexcept {return true;}};
    mutable DcUnionCache dc_union_cache_{};
    std::array<std::uint64_t,2> resource_counts() const noexcept {
        auto dc_count=contexts.size()+dc_objects.size();
        if(dc_union_cache_.versions[0]==contexts.key_version() && dc_union_cache_.versions[1]==dc_objects.key_version())dc_count=dc_union_cache_.count;
        else if(!contexts.empty() && !dc_objects.empty()) {
            // 兩表同為排序樹；大小接近時合併走訪，避免每個 DC 都從樹根搜尋。
            // 大小懸殊則搜尋較小集合，避免少量 DC 導致掃過整張大表。
            const auto small=std::min(contexts.size(),dc_objects.size()),large=std::max(contexts.size(),dc_objects.size());
            if(small<large/static_cast<std::size_t>(std::bit_width(large))) {
                if(contexts.size()<dc_objects.size()) {
                    for(const auto& [handle,dc]:contexts){(void)dc;if(dc_objects.contains(handle))--dc_count;}
                } else {
                    for(const auto& [handle,font]:dc_objects){(void)font;if(contexts.contains(handle))--dc_count;}
                }
            } else {
                auto dc=contexts.begin();auto selected=dc_objects.begin();
                while(dc!=contexts.end() && selected!=dc_objects.end()) {
                    if(dc->first<selected->first)++dc;
                    else if(selected->first<dc->first)++selected;
                    else {--dc_count;++dc;++selected;}
                }
            }
        }
        dc_union_cache_.versions[0]=contexts.key_version();dc_union_cache_.versions[1]=dc_objects.key_version();dc_union_cache_.count=dc_count;
        return {dc_count+metafiles.size()+regions.size()+brushes.size()+pens.size()+bitmaps.size()+fonts.size()+palettes.size(),
            windows.size()+icons.size()+accelerators.size()+menus.size()+hooks.size()+event_hooks.size()+input_contexts.size()};
    }
    void observe_resources() noexcept {
        // 未去重的總數是嚴格上界；不可能創新高時，不必走訪任何 DC。
        const auto gdi_upper=contexts.size()+dc_objects.size()+metafiles.size()+regions.size()+brushes.size()+pens.size()+bitmaps.size()+fonts.size()+palettes.size();
        const auto users=windows.size()+icons.size()+accelerators.size()+menus.size()+hooks.size()+event_hooks.size()+input_contexts.size();
        if(gdi_upper>resource_peaks[0])resource_peaks[0]=std::max(resource_peaks[0],resource_counts()[0]);
        resource_peaks[1]=std::max(resource_peaks[1],static_cast<std::uint64_t>(users));
    }

    std::map<std::uint64_t,GuiImmContext> input_contexts;
    std::map<std::uint64_t,GuiImmBuffer> input_buffers;
    std::map<std::uint64_t,std::uint64_t> input_defaults,input_associations;
    std::map<std::uint64_t,std::uint64_t> input_windows,input_window_targets;
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::uint32_t> input_borrows;
    std::map<std::uint64_t,GuiWinEventHook> event_hooks;
    std::vector<GuiWinEvent> event_queue;
    std::map<std::uint64_t,GuiPropertyStore> shell_property_stores;
    // LockWindowUpdate 全桌面只鎖定一個視窗樹；鎖定期間不提交原生畫面，解除時整樹失效重繪。
    std::uint64_t update_lock{};
    // ActivateKeyboardLayout 為各執行緒選定的 HKL；未選定時沿用宿主目前輸入法。
    std::map<std::uint64_t,std::uint64_t> thread_keyboard_layouts;
    std::uint64_t process_keyboard_layout{};
    std::map<std::uint64_t,GuiShellItem> shell_items;
    std::map<std::uint64_t,GuiShellItemEnumerator> shell_item_enumerators;
    std::map<std::uint64_t,GuiShellItemOperation> shell_item_operations;
    std::uint64_t shell_item_vtable{},shell_item_enum_vtable{};
    std::uint64_t shell_property_vtable{},task_allocator{};
    std::uint32_t task_allocator_references{};
    std::set<std::uint64_t> task_allocations;
    bool dpi_aware{},dpi_context_set{};
    std::uint64_t dpi_context{UINT64_MAX};
    std::u16string explicit_app_user_model_id;
    // 虛擬 desktop 的 global atoms；跨宿主 process 的桌面命名空間尚未共享。
    std::uint32_t theme_app_properties{7},preferred_theme_mode{};
    bool theme_enabled{true};
    std::map<std::uint64_t,GuiThemeData> themes{};
    std::map<std::uint64_t,std::uint64_t> window_themes{};
    std::map<std::uint64_t,GuiThemeText> theme_text_operations{};
    std::map<std::uint64_t,GuiCbtAction> cbt_actions;
    std::map<std::uint64_t,std::pair<std::uint64_t,std::string>> cbt_replay;
    std::map<std::uint64_t,GuiCreationOperation> creation_operations;
    std::map<std::uint64_t,GuiPropertySheet> property_sheets;
    std::map<std::uint64_t,GuiPropertyPage> property_pages;
    std::map<std::uint64_t,GuiPrintDialog> print_dialogs;
    std::map<std::uint64_t,GuiPrintServices> print_services;
    std::uint64_t print_services_vtable{};
    std::vector<std::uint64_t> z_order;
    std::map<std::uint64_t,GuiHook> hooks;
    std::map<std::u16string, GuiClass> classes;
    std::map<std::u16string, GuiClass> system_classes;
    std::map<std::uint64_t, GuiWindow> windows;
    std::map<std::uint64_t,GuiRegion> regions;
    // Windows 3.x 相容 metafile（HMETAFILE）；內容為 METAHEADER 開頭的完整記錄。
    std::map<std::uint64_t,std::vector<std::uint8_t>> windows_metafiles;
    std::map<std::uint64_t, std::uint32_t> brushes;
    struct Palette {std::vector<std::uint32_t> entries;std::uint64_t generation{1};bool operator==(const Palette&)const=default;};
    std::map<std::uint64_t,Palette> palettes;
    std::map<std::uint64_t,GuiBitmap> brush_patterns;
    std::map<std::uint64_t, std::uint32_t> pens;
    struct ExtendedPen {std::uint32_t style{},width{},brush_style{};bool operator==(const ExtendedPen&)const=default;};
    std::map<std::uint64_t,ExtendedPen> extended_pens;
    std::map<std::uint64_t,std::int32_t> pen_widths,brush_hatches;
    std::vector<GuiMessage> queue;
    std::set<std::uint64_t> message_threads;
    std::uint64_t next_window{0x10000}, next_object{0xc000}, next_atom{0xc000}, next_input_order{1};
    std::uint64_t dispatched{}, paints{}, destroyed{};
    std::map<std::uint64_t,GuiSocketWatch> socket_watches{};
    std::map<std::uint64_t,GuiMouseTracking> mouse_tracking;
    std::map<std::uint64_t,GuiDragDetection> drag_detections;
    std::map<std::uint64_t,GuiMouseInput> mouse_inputs;
    std::map<std::uint64_t,GuiMoveSize> move_sizes;
    std::uint64_t capture_generation{},escape_presses{};
    std::map<std::uint64_t,OleDragOperation> ole_drags;
    std::map<std::uint32_t,std::uint64_t> ole_drag_cursors;
    std::int32_t mouse_drag_width{4},mouse_drag_height{4};
    std::uint32_t mouse_hover_width{4},mouse_hover_height{4},mouse_hover_time{400};
    bool font_smoothing{true};
    bool ghosting_disabled{};
    std::uint32_t font_smoothing_type{2},font_smoothing_contrast{1400},font_smoothing_orientation{1};
    std::array<std::uint8_t,256> keyboard{},async_keyboard{};
    std::array<std::uint8_t,256> input_keyboard{},injected_keys{},injected_di_keys{};
    std::array<std::uint8_t,8> injected_di_buttons{};
    std::vector<GuiHotKey> hotkeys;
    std::map<std::uint64_t,GuiKeyboardTranslation> keyboard_translation;
    std::uint64_t last_input_ticks{};
    std::uint64_t caret_window{},caret_bitmap{},cursor{0xd000+32512};
    std::uint64_t cursor_animation_start{};std::uint32_t cursor_animation_step{};
    std::int32_t caret_x{},caret_y{},cursor_count{};bool caret_visible{};
    std::uint64_t focus{}, active{}, capture{}, next_item{0x200000}, desktop{0x100};
    KeyVersionedMap<std::uint64_t,std::uint64_t> dc_objects{};
    std::map<std::uint64_t,std::vector<GuiDc>> saved_contexts;
    KeyVersionedMap<std::uint64_t,GuiDc> contexts{};
    std::map<std::uint64_t,GuiGrayString> gray_strings;
    std::map<std::uint64_t,GuiBitmap> bitmaps{};
    std::map<std::uint64_t,GuiIcon> icons{};
    GuiIcon default_application_icon{};bool application_icon_loaded{};
    std::map<std::uint64_t,GuiImageList> image_lists{};
    std::uint64_t shell_large_image_list{},shell_small_image_list{};
    std::map<std::uint64_t,std::vector<std::array<std::uint16_t,3>>> accelerators{};
    std::map<std::pair<std::uint64_t,std::uint64_t>,GuiTimer> timers{};
    std::uint64_t timer_deadline(std::uint64_t earliest,std::uint64_t tolerance,std::pair<std::uint64_t,std::uint64_t> ignored)const {
        if(!tolerance)return earliest;
        const auto latest=tolerance>UINT64_MAX-earliest?UINT64_MAX:earliest+tolerance;
        auto result=latest;bool found=false;
        for(const auto& [key,timer]:timers)if(key!=ignored && timer.deadline>=earliest && timer.deadline<=result){result=timer.deadline;found=true;}
        return found?result:earliest;
    }
    std::map<std::uint64_t,std::vector<GuiMenuItem>> menus{};
    std::map<std::uint64_t,GuiMenuInfo> menu_info{};
    std::map<std::uint64_t,std::uint64_t> system_menus{};
    std::map<std::uint64_t,GuiMenuTrack> menu_tracks{};
    std::map<std::uint64_t,GuiSentMessage> sent_messages{};
    std::vector<GuiSendCallback> send_callbacks{};
    std::map<std::uint64_t,GuiAsyncSend> async_sends{};
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::uint64_t> message_wait_deadlines{};
    std::map<std::pair<std::uint64_t,std::uint32_t>,std::array<std::uint8_t,28>> scroll_info{};
    // EnableScrollBar 的 ESB_* 箭頭停用狀態，鍵與 scroll_info 相同（視窗、SB_HORZ／SB_VERT／SB_CTL）。
    std::map<std::pair<std::uint64_t,std::uint32_t>,std::uint32_t> scroll_arrows{};
    // msctls_netaddress 視窗的 NCM_SETALLOWTYPE 類型遮罩（預設 NET_STRING_ANY_ADDRESS）。
    std::map<std::uint64_t,std::uint32_t> net_address_types{};
    std::map<std::uint64_t,GuiMdiMove> mdi_moves{};
    std::optional<std::array<std::int32_t,4>> cursor_clip;
    std::set<std::pair<std::uint64_t,std::uint64_t>> input_attachments;
    std::map<std::uint64_t,std::vector<std::uint8_t>> fonts{};
    std::map<std::uint64_t,std::vector<NativeFontFace>> memory_fonts;
    std::map<std::pair<std::string,std::uint32_t>,std::vector<std::uint64_t>> file_fonts;
    std::map<std::uint64_t,std::shared_ptr<const void>> font_natives;
    std::map<std::uint64_t,std::uint32_t> com_initializations{};
    // CoLockObjectExternal 的外部強鎖計數；第一次鎖定 AddRef，最後解鎖 Release。
    std::map<std::uint64_t,std::uint32_t> com_external_locks{};
    // uxtheme 緩衝繪製：記憶體 DC 承接繪製，EndBufferedPaint 依 BLENDFUNCTION 或 SRCCOPY 寫回目標。
    struct BufferedPaint {std::uint64_t target{},dc{},bitmap{},previous{},bits{};std::array<std::int32_t,4> rect{},exclude{};std::uint32_t format{},blend{};bool excluded{},blended{};bool operator==(const BufferedPaint&)const=default;};
    std::map<std::uint64_t,BufferedPaint> buffered_paints{};std::uint32_t buffered_paint_inits{};
    // D3DKMT 配接器 handle（同一個虛擬顯示配接器可被開啟多次）。
    std::set<std::uint32_t> kmt_adapters{};std::uint32_t next_kmt_adapter{0x40000040U};
    std::uint64_t com_main_sta{}; // 依首次 STA 初始化順序識別，非依 thread ID 排序。
    std::map<std::uint64_t,std::uint64_t> com_message_filters{};
    std::map<std::uint64_t,GuiCommonDialog> common_dialogs{};
    std::map<std::uint64_t,GuiShellBrowse> shell_browsers{};
    std::map<std::uint64_t,GuiShellOpenWith> shell_open_with;
    std::map<std::uint64_t,GuiAppBar> appbars;
    std::optional<std::array<std::int32_t,4>> appbar_native_area;
    std::optional<shell::Dock> appbar_native_dock;
    bool appbar_fullscreen{};
    std::map<std::uint64_t,std::u16string> shell_pidls{};
    struct ShellRegistration {
        std::uint64_t window{};std::uint32_t sources{},events{},message{};
        std::u16string root;bool recursive{};
        bool operator==(const ShellRegistration&)const=default;
    };
    struct ShellDelivery {
        std::uint32_t registration{},event{},message{};
        std::uint64_t window{},thread{},array{};
        std::vector<std::uint64_t> allocations;
        bool shared{},consumed{};unsigned active{},locks{};
        bool operator==(const ShellDelivery&)const=default;
    };
    std::uint32_t next_shell_registration{1};
    std::map<std::uint32_t,ShellRegistration> shell_registrations;
    std::map<std::uint64_t,ShellDelivery> shell_deliveries;
    std::map<std::uint64_t,std::uint64_t> shell_locks;
    std::uint32_t common_dialog_error{};
    std::map<std::uint64_t,GuiGlobalMemory> global_memory{};
    std::set<std::uint64_t> drag_lists{};std::map<std::uint64_t,std::int32_t> drag_insertions{};
    std::array<std::int32_t,2> pointer_position{};
    std::map<std::uint64_t,GuiControlNotification> control_notifications{};
    std::vector<std::uint64_t> control_notification_buffers{};
    struct PowerRegistration {
        std::uint64_t window{};std::array<std::uint8_t,16> guid{};
        std::optional<std::uint32_t> last;
        bool operator==(const PowerRegistration&)const=default;
    };
    struct PowerDelivery {
        std::uint64_t registration{},window{};
        bool operator==(const PowerDelivery&)const=default;
    };
    std::map<std::uint64_t,PowerRegistration> power_registrations;
    std::map<std::uint64_t,PowerDelivery> power_deliveries;
    std::vector<std::uint64_t> power_notification_buffers;
    std::map<std::uint64_t,std::uint32_t> ole_initializations{};
    std::map<std::uint64_t,GuiDropTarget> drop_targets{};
    std::map<std::uint64_t,GuiControlOperation> control_operations{};
    bool operator==(const GuiState&) const = default;
};
struct GuiDisplayMode {
    std::uint32_t width{},height{},refresh_numerator{},refresh_denominator{1};
    std::uint32_t scanline{1},scaling{1};
    std::uint64_t native_id{};
    bool operator==(const GuiDisplayMode&) const = default;
};
class NativeGui {
public:
    virtual ~NativeGui() = default;
    virtual void create(std::uint64_t handle, const GuiWindow& window) = 0;
    virtual void show(std::uint64_t handle, bool visible) = 0;
    virtual void reorder(std::uint64_t,std::uint64_t) {}
    virtual void activate(std::uint64_t) {}
    virtual void focus(std::uint64_t) {}
    // UIA provider 事件對應原生輔助使用通知；未提供的 backend 明確回報 false。
    virtual bool accessibility_event(std::uint64_t,std::uint32_t,std::uint32_t) {return false;}
    // UIA notification 對應的朗讀公告；important=true 代表 NotificationProcessing_Important*。
    virtual bool accessibility_announcement(std::uint64_t,std::u16string_view,bool) {return false;}
    virtual std::array<std::int32_t,4> work_area(){return {0,0,1440,900};}
    virtual std::array<std::int32_t,4> screen_area(){return {0,0,1440,900};}
    virtual std::vector<GuiDisplayMode> display_modes(){return {};}
    virtual std::optional<GuiDisplayMode> current_display_mode(){return {};}
    // 模式變更限定目前程序；離開全螢幕、銷毀視窗或 backend 時恢復原模式。
    virtual bool display_fullscreen(std::uint64_t,bool,const std::optional<GuiDisplayMode>& = {}){return false;}
    virtual std::optional<shell::Dock> shell_dock(){return {};}
    virtual std::uint32_t shell_reveal(std::span<const std::shared_ptr<HostFile>>,std::uint32_t){return 50;}
    virtual std::vector<shell::Application> shell_applications(const std::shared_ptr<HostFile>&,std::string_view){return {};}
    virtual std::uint32_t shell_open(const std::shared_ptr<HostFile>&,std::string_view,const shell::Application&){return 50;}
    virtual std::optional<shell::Application> shell_default_application(const std::shared_ptr<HostFile>&,std::string_view){return {};}
    virtual bool set_opacity(std::uint64_t,std::uint8_t) {return false;}
    virtual bool set_region(std::uint64_t,const std::optional<GuiRegion>&) {return false;}
    virtual void present(std::uint64_t handle, const GuiWindow& window) = 0;
    virtual void destroy(std::uint64_t handle) = 0;
    virtual std::vector<GuiMessage> poll(bool wait) = 0;
    virtual bool input_active() const {return true;}
    virtual bool status_icons(const std::map<std::uint64_t,desktop::Icon>&) {return false;}
    virtual std::uint32_t help(const NativeHelpRequest&) {return 50;}
    virtual bool status_focus(std::uint64_t) {return false;}
    virtual bool attention(std::uint64_t,bool,bool) {return false;}
    virtual std::optional<std::uint32_t> notification_state() const {return {};}
    virtual bool ime_mark(std::uint64_t,const GuiImeData&,std::array<std::int32_t,2>) {return false;}
    virtual bool ime_convert(std::uint64_t) {return false;}
    virtual void raw_input_policy(bool,bool,bool) {}
    virtual std::vector<GuiMessage> poll_for(std::uint64_t nanoseconds) {return poll(nanoseconds!=0);}
    virtual std::optional<GuiFontMetrics> font_metrics(const GuiFont&) {return std::nullopt;}
    virtual std::optional<std::int32_t> text_width(const GuiFont&,const std::u16string&) {return std::nullopt;}
    virtual std::optional<std::vector<std::int32_t>> text_advances(const GuiFont&,const std::u16string&) {return std::nullopt;}
    virtual std::vector<std::u16string> font_families(bool) {return {};}
    virtual std::optional<std::u16string> clipboard_text() {return std::nullopt;}
    virtual bool set_clipboard_text(const std::u16string&) {return false;}
    virtual std::optional<std::uint64_t> clipboard_sequence() {return std::nullopt;}
    virtual bool clear_clipboard() {return false;}
    virtual std::optional<std::array<std::int32_t,2>> cursor_position() {return std::nullopt;}
    virtual bool set_cursor_position(std::int32_t,std::int32_t) {return false;}
    virtual bool set_cursor(const GuiIcon*,bool) {return false;}
    virtual std::optional<bool> physical_key_state(unsigned) {return std::nullopt;}
    virtual std::optional<std::uint64_t> input_idle_milliseconds() {return std::nullopt;}
    virtual std::optional<GuiMenuSelection> popup_menu(std::uint64_t,const std::vector<GuiPopupItem>&,std::int32_t,std::int32_t,std::uint32_t) {return std::nullopt;}
    // 主執行緒的原生追蹤迴圈每次閒置借用客體訊息泵；false 立即取消追蹤。
    virtual std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle) {
        (void)idle;return popup_menu(owner,items,x,y,flags);
    }
    // 子選單即將顯示／已隱藏時同步通知客體；false 取消整個追蹤。
    virtual std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle,const std::function<bool(std::uint64_t,std::uint32_t,bool)>& popup) {
        (void)popup;return popup_menu(owner,items,x,y,flags,idle);
    }
    // 高亮通知可同步改變選單狀態；false 表示客體取消或項目已失效。
    virtual std::optional<GuiMenuSelection> popup_menu(std::uint64_t owner,const std::vector<GuiPopupItem>& items,std::int32_t x,std::int32_t y,std::uint32_t flags,const std::function<bool()>& idle,const std::function<bool(std::uint64_t,std::uint32_t,bool)>& popup,const std::function<bool(const GuiMenuHighlight&)>& highlight) {
        (void)highlight;return popup_menu(owner,items,x,y,flags,idle,popup);
    }
    virtual void graphics_text(std::uint64_t,const GuiDraw&) {}
    virtual std::optional<GuiThemeMetrics> theme_metrics(std::u16string_view,std::int32_t,std::int32_t,std::uint32_t,std::array<std::int32_t,2>) {return {};}
    virtual std::optional<GuiBitmap> theme_background(std::u16string_view,std::int32_t,std::int32_t,std::uint32_t,std::array<std::int32_t,2>) {return {};}
    virtual std::optional<GuiBitmap> capture_client(std::uint64_t) {return {}; }
    virtual void graphics_surface(std::uint64_t,std::shared_ptr<RasterSurface>) {}
    virtual void* graphics_view(std::uint64_t) {return nullptr;}
    virtual std::optional<std::array<std::uint16_t,768>> gamma_ramp(){return {};}
    virtual bool set_gamma_ramp(std::span<const std::uint16_t,768>){return false;}
    virtual void beep() {}
    virtual void set_menu(std::uint64_t,const std::vector<GuiPopupItem>&) {}
    // 以客體 client 原點為基準的原生選單列／項目矩形；index=-1 表示整列。
    virtual std::optional<std::array<std::int32_t,4>> menu_rectangle(std::uint64_t,std::int32_t) {return std::nullopt;}
    virtual std::optional<std::uint32_t> text_line_count(std::uint64_t) { return std::nullopt; }
    // 嵌入式網頁檢視（WebView2 對應原生網頁引擎）：id 由服務層配置，矩形為父視窗 client 座標。
    // 無原生後端時 create 回傳 false，其餘呼叫不產生作用。command：0 重新載入、1 停止、2 上一頁、3 下一頁。
    virtual bool webview_create(std::uint64_t,std::uint64_t) {return false;}
    virtual void webview_destroy(std::uint64_t) {}
    virtual void webview_visible(std::uint64_t,bool) {}
    virtual void webview_bounds(std::uint64_t,const std::array<std::int32_t,4>&) {}
    virtual void webview_zoom(std::uint64_t,double) {}
    virtual void webview_focus(std::uint64_t) {}
    virtual void webview_navigate(std::uint64_t,const std::u16string&) {}
    virtual void webview_load_html(std::uint64_t,const std::u16string&) {}
    virtual void webview_command(std::uint64_t,unsigned) {}
    virtual void webview_script(std::uint64_t,const std::u16string&) {}
    virtual void webview_user_script(std::uint64_t,const std::u16string&) {}
    virtual void webview_post_message(std::uint64_t,const std::u16string&,bool) {}
    // UTF-16 的行首／行末索引，不含 CRLF；軟換行與實際原生文字排版共用。
    virtual std::optional<std::vector<std::array<std::uint32_t,2>>> text_line_ranges(std::uint64_t) {return std::nullopt;}
};
GuiFont gui_font(const GuiState&,std::uint64_t font_handle);
GuiFontMetrics fallback_font_metrics(const GuiFont&);
std::shared_ptr<NativeGui> make_native_gui(const std::filesystem::path& snapshot);
}
