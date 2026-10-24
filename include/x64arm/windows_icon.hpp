#pragma once
#include "x64arm/windows_abi.hpp"
#include "x64arm/windows_gui.hpp"
#include <optional>

namespace x64arm {
struct IconChoice {std::size_t offset{};std::uint32_t width{},height{},depth{},id{};};
IconChoice choose_icon(std::span<const std::uint8_t>,bool cursor,bool file,
                       std::uint32_t width,std::uint32_t height,bool monochrome=false,bool prefer_larger=false);
GuiIcon decode_icon_resource(std::span<const std::uint8_t>,bool icon=true,
                             std::uint32_t width=0,std::uint32_t height=0,std::uint32_t flags=0);
GuiIcon decode_icon_file(std::span<const std::uint8_t>,bool cursor,
                        std::uint32_t width=0,std::uint32_t height=0,std::uint32_t flags=0,bool prefer_larger=false);
std::uint32_t icon_animation_step(const GuiIcon&,std::uint64_t elapsed_ticks);
void finish_icon(GuiIcon&);
void resize_icon(GuiIcon&,std::uint32_t,std::uint32_t,bool monochrome=false);
std::optional<GuiIcon> native_png_icon(std::span<const std::uint8_t>);
std::optional<GuiIcon> native_stock_icon(std::uint32_t,std::uint32_t);
std::optional<GuiIcon> native_system_cursor(std::uint32_t);
std::span<const std::uint32_t> stock_icon_ids();
bool stock_icon_valid(std::uint32_t);
inline constexpr std::u16string_view stock_icon_path=u"C:\\Windows\\System32\\MacWinStockIcons.dll";
bool is_stock_icon_path(std::u16string_view);

// ICO/CUR、PE、NE、BMP、ANI 共用的唯讀來源；不載入或執行來源程式。
class IconSource {
    struct Resource {std::uint32_t id{},offset{},size{};};
    std::span<const std::uint8_t> bytes_;
    std::vector<Resource> groups_,icons_;
    std::uint32_t resource_rva_{},resource_size_{};
    std::size_t sections_{};
    unsigned section_count_{};
    bool file_{},cursor_{},ne_{},bitmap_{};
    std::uint32_t get(std::size_t,unsigned) const;
    std::span<const std::uint8_t> slice(std::size_t,std::size_t) const;
    std::size_t offset(std::uint32_t,std::size_t) const;
    std::size_t directory(std::uint32_t,std::size_t) const;
    std::vector<Resource> entries(std::uint32_t) const;
    std::span<const std::uint8_t> data(const Resource&,unsigned depth=0) const;
public:
    explicit IconSource(std::span<const std::uint8_t>);
    std::size_t count() const {return groups_.size();}
    std::size_t index(std::int32_t) const;
    GuiIcon extract(std::size_t,std::uint32_t width,std::uint32_t height=0,
                    std::uint32_t flags=0,std::uint32_t* id=nullptr) const;
};
std::span<const WindowsServiceDescriptor> icon_service_descriptors();
}
