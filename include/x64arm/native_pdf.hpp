#pragma once
#include "x64arm/windows_gui.hpp"
namespace x64arm {
std::vector<std::uint8_t> native_pdf_document(std::span<const GuiBitmap>,std::u16string_view title,unsigned copies=1,bool collate=false);
struct NativeSpoolDocument {std::vector<std::uint8_t> bytes;unsigned pages{};};
NativeSpoolDocument native_pdf_raw_document(std::span<const std::uint8_t>);
NativeSpoolDocument native_pdf_text_document(std::span<const std::u16string>,std::u16string_view title,unsigned width,unsigned height,unsigned copies=1,bool collate=false);
}
