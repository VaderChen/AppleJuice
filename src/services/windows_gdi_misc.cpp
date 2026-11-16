// gdi32 其餘 DC／物件／初始化入口：SetObjectOwner、SetMagicColors、EnableEUDC、SetVirtualResolution、GdiConvertToDevmodeW、
// GdiDllInitialize、GdiInitializeLanguagePack 與 GdiEntry13（DdQueryDisplaySettingsUniqueness）。
// 物件與 DC 驗證沿用 GDI 既有狀態；虛擬解析度存於 DC，由 GetDeviceCaps 讀取；顯示設定唯一值隨 ChangeDisplaySettings 遞增。
#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include <algorithm>
#include <vector>

namespace x64arm {
std::span<const WindowsServiceDescriptor> gdi_misc_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"gdi32.dll","SetObjectOwner",2},{"gdi32.dll","SetMagicColors",3},{"gdi32.dll","EnableEUDC",1},
        {"gdi32.dll","SetVirtualResolution",5},{"gdi32.dll","GdiConvertToDevmodeW",1},
        {"gdi32.dll","GdiDllInitialize",3},{"gdi32.dll","GdiInitializeLanguagePack",1},{"gdi32.dll","GdiEntry13",0}};
    return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_gdi_misc(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](unsigned error,std::uint64_t value=0){last_error_=error;return finish(value);};
    const auto d=[&](unsigned i){return static_cast<std::uint32_t>(arg(i));};
    const auto known_dc=[&](std::uint64_t dc){
        return dc && (gui_.contexts.contains(dc) || gui_.dc_objects.contains(dc) || (dc>=0x10000000 && gui_.windows.contains(dc-0x10000000)));
    };
    if(name=="GdiDllInitialize" || name=="GdiInitializeLanguagePack")return finish(1); // 程序附加與語言套件字型連結初始化：狀態於服務啟動時已建立。
    if(name=="GdiEntry13")return finish(gui_.display_uniqueness);
    if(name=="EnableEUDC"){gui_.eudc_enabled=d(0)!=0;return finish(1);}
    if(name=="SetObjectOwner"){
        // NT 的 GDI 物件一律屬於建立它的程序；只驗證 handle，擁有者參數沒有可轉移的對象。
        if(!arg(0) || !nested_service(state,memory,"gdi32.dll","GetObjectType",{arg(0)}))return fail(6);
        return finish(1);
    }
    if(name=="SetMagicColors"){
        // 魔術色是調色盤裝置的系統索引 8、9、246、247；原生 true-color 裝置沒有系統調色盤（與 GetSystemPaletteEntries 一致）。
        if(!known_dc(arg(0)))return fail(6);
        const auto index=d(2);if(index!=8 && index!=9 && index!=246 && index!=247)return fail(87);
        return finish(0);
    }
    if(name=="SetVirtualResolution"){
        // 全部為 0 時回到實體裝置值；其餘情況四個值都必須為正。之後 GetDeviceCaps 的 HORZRES／VERTRES／HORZSIZE／VERTSIZE 依此回報。
        const auto context=gui_.contexts.find(arg(0));if(context==gui_.contexts.end())return fail(6);
        std::array<std::int32_t,4> values{};for(unsigned i=0;i<4;++i)values[i]=static_cast<std::int32_t>(d(i+1));
        const bool reset=std::all_of(values.begin(),values.end(),[](auto v){return v==0;});
        if(!reset && std::any_of(values.begin(),values.end(),[](auto v){return v<=0;}))return fail(87);
        context->second.virtual_resolution=values;return finish(1);
    }
    if(name=="GdiConvertToDevmodeW"){
        // DEVMODEA → DEVMODEW：裝置名稱與表單名稱（各 32 個 ANSI 位元組）轉為 UTF-16，其餘欄位依位移照搬，
        // 驅動私有資料接在 dmSize 之後；結果配置於程序 heap，由呼叫端 HeapFree。
        if(!arg(0))return finish(0);
        const auto source=arg(0);const auto size_a=static_cast<unsigned>(memory.load(GuestAddress{source+36},16)),extra=static_cast<unsigned>(memory.load(GuestAddress{source+38},16));
        constexpr unsigned name_a=32,form_a=70,form_end_a=102;
        if(size_a<form_a || size_a>156)return fail(87,0);
        std::vector<std::uint8_t> input(size_a+extra);memory.read(GuestAddress{source},input);
        const auto convert=[&](std::span<const std::uint8_t> field,std::vector<std::uint8_t>& out){
            const auto end=std::find(field.begin(),field.end(),std::uint8_t{0});
            auto text=utf8_to_utf16_lossy(std::string_view(reinterpret_cast<const char*>(field.data()),static_cast<std::size_t>(end-field.begin())));
            if(text.size()>31)text.resize(31);
            const auto at=out.size();out.resize(at+64,0);
            for(std::size_t i=0;i<text.size();++i){out[at+2*i]=static_cast<std::uint8_t>(text[i]);out[at+2*i+1]=static_cast<std::uint8_t>(text[i]>>8);}
        };
        std::vector<std::uint8_t> output;output.reserve(220+extra);
        convert(std::span(input).first(name_a),output);
        output.insert(output.end(),input.begin()+name_a,input.begin()+std::min(size_a,form_a));
        if(size_a>form_a){
            convert(std::span(input).subspan(form_a,std::min(size_a,form_end_a)-form_a),output);
            if(size_a>form_end_a)output.insert(output.end(),input.begin()+form_end_a,input.begin()+size_a);
        }
        const auto size_w=static_cast<std::uint16_t>(output.size());output[68]=static_cast<std::uint8_t>(size_w);output[69]=static_cast<std::uint8_t>(size_w>>8); // DEVMODEW 的 dmSize 在 64 位元組裝置名稱之後。
        output.insert(output.end(),input.begin()+size_a,input.end());
        const auto result=allocate_heap(memory,output.size());if(!result)return fail(8);
        memory.write(GuestAddress{result},output);return finish(result);
    }
    return std::nullopt;
}
}
