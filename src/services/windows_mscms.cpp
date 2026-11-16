// mscms.dll 設定檔與裝置管理：開啟／建立／關閉設定檔（HPROFILE）、標頭與 tag 元素讀寫、驗證，
// 安裝／解除安裝、裝置關聯、列舉、色彩目錄、標準色彩空間、CMM 登錄與 WCS 預設查詢，以及由 LOGCOLORSPACE 產生 ICC 設定檔。
// 與 gdi32 ICM 共用 IcmStore；色彩轉換（CreateColorTransform 等）另屬轉換家族。只支援本機（pMachineName 為 NULL）。
#include "windows_icm_store.hpp"
#include <algorithm>
#include <cmath>

namespace x64arm {
std::span<const WindowsServiceDescriptor> mscms_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"mscms.dll","OpenColorProfileA",4},{"mscms.dll","OpenColorProfileW",4},{"mscms.dll","WcsOpenColorProfileA",7},{"mscms.dll","WcsOpenColorProfileW",7},
        {"mscms.dll","CloseColorProfile",1},{"mscms.dll","WcsCreateIccProfile",2},{"mscms.dll","GetColorProfileHeader",2},{"mscms.dll","SetColorProfileHeader",2},
        {"mscms.dll","GetCountColorProfileElements",2},{"mscms.dll","GetColorProfileElementTag",3},{"mscms.dll","IsColorProfileTagPresent",3},
        {"mscms.dll","GetColorProfileElement",6},{"mscms.dll","SetColorProfileElement",5},{"mscms.dll","SetColorProfileElementSize",3},
        {"mscms.dll","SetColorProfileElementReference",3},{"mscms.dll","IsColorProfileValid",2},{"mscms.dll","GetColorProfileFromHandle",3},
        {"mscms.dll","GetColorDirectoryA",3},{"mscms.dll","GetColorDirectoryW",3},{"mscms.dll","InstallColorProfileA",2},{"mscms.dll","InstallColorProfileW",2},
        {"mscms.dll","UninstallColorProfileA",3},{"mscms.dll","UninstallColorProfileW",3},{"mscms.dll","AssociateColorProfileWithDeviceA",3},{"mscms.dll","AssociateColorProfileWithDeviceW",3},
        {"mscms.dll","DisassociateColorProfileFromDeviceA",3},{"mscms.dll","DisassociateColorProfileFromDeviceW",3},{"mscms.dll","EnumColorProfilesA",5},{"mscms.dll","EnumColorProfilesW",5},
        {"mscms.dll","GetStandardColorSpaceProfileA",4},{"mscms.dll","GetStandardColorSpaceProfileW",4},{"mscms.dll","SetStandardColorSpaceProfileA",3},{"mscms.dll","SetStandardColorSpaceProfileW",3},
        {"mscms.dll","RegisterCMMA",3},{"mscms.dll","RegisterCMMW",3},{"mscms.dll","UnregisterCMMA",2},{"mscms.dll","UnregisterCMMW",2},{"mscms.dll","SelectCMM",1},
        {"mscms.dll","WcsGetDefaultColorProfile",7},{"mscms.dll","WcsGetDefaultColorProfileSize",6},{"mscms.dll","WcsEnumColorProfilesSize",3},
        {"mscms.dll","WcsGetDefaultRenderingIntent",2},{"mscms.dll","WcsGetUsePerUserProfiles",3},{"mscms.dll","WcsGetCalibrationManagementState",1},
        {"mscms.dll","CreateProfileFromLogColorSpaceA",2},{"mscms.dll","CreateProfileFromLogColorSpaceW",2},{"mscms.dll","DllCanUnloadNow",0},
    };return table;
}
WindowsServices::IcmProfiles& WindowsServices::icm_profiles(){if(!icm_profiles_)icm_profiles_=std::make_shared<IcmProfiles>();return *icm_profiles_;}
std::optional<std::uint32_t> WindowsServices::invoke_mscms(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const auto name=service.symbol;const bool wide=name.ends_with('W');const unsigned unit=x86_?4U:8U;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto finish=[&](std::uint64_t value){arg.return_to(state,value);return std::optional<std::uint32_t>{};};
    const auto fail=[&](std::uint32_t error){last_error_=error;return finish(0);};
    const auto text=[&](std::uint64_t pointer,bool unicode){return unicode?read_utf16(memory,GuestAddress{pointer}):utf8_to_utf16(read_ansi(memory,pointer));};
    const auto encode=[&](std::u16string_view value,bool unicode){std::vector<std::uint8_t> bytes;if(unicode)bytes=utf16_bytes(value);else {const auto narrow=utf16_to_utf8(value);bytes.assign(narrow.begin(),narrow.end());bytes.push_back(0);}return bytes;};
    // 輸出字串：所需位元組數寫回 *size，緩衝區為 NULL 或不足時回 ERROR_INSUFFICIENT_BUFFER。
    const auto output=[&](std::uint64_t buffer,std::uint64_t size,const std::vector<std::uint8_t>& bytes){
        if(!size)return fail(87);const auto capacity=static_cast<std::uint32_t>(memory.load(GuestAddress{size},32));memory.store(GuestAddress{size},32,bytes.size());
        if(!buffer || capacity<bytes.size())return fail(122);memory.write(GuestAddress{buffer},bytes);return finish(1);
    };
    IcmStore store(*this,state,memory);auto& opened=icm_profiles();
    const auto profile=[&](std::uint64_t handle){const auto found=opened.profiles.find(handle);return found==opened.profiles.end()?nullptr:&found->second;};
    // PROFILE {dwType, pProfileData, cbDataSize}：PROFILE_FILENAME 1（只有檔名時在色彩目錄）、PROFILE_MEMBUFFER 2。
    const auto open=[&](std::uint64_t pointer,std::uint32_t access,std::uint32_t creation,bool unicode)->std::uint64_t{
        if(!pointer || (access!=1 && access!=2) || creation<1 || creation>5){last_error_=87;return 0;}
        const auto type=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer},32)),size=static_cast<std::uint32_t>(memory.load(GuestAddress{pointer+2*unit},32));const auto data=memory.load(GuestAddress{pointer+unit},bits);
        IcmProfiles::Profile entry;entry.writable=access==2;
        if(type==2){
            if(!data || size<128 || size>64*1024*1024){last_error_=87;return 0;}std::vector<std::uint8_t> bytes(size);memory.read(GuestAddress{data},bytes);
            auto model=icc_parse(bytes);if(!model){last_error_=icm_error_invalid_profile;return 0;}entry.model=std::move(*model);
        }else if(type==1){
            if(!data){last_error_=87;return 0;}entry.path=icm_profile_path(text(data,unicode));const bool present=store.exists(entry.path);
            // CREATE_NEW 1、CREATE_ALWAYS 2、OPEN_EXISTING 3、OPEN_ALWAYS 4、TRUNCATE_EXISTING 5。
            if(creation==1 && present){last_error_=80;return 0;}
            if((creation==3 || creation==5) && !present){last_error_=2;return 0;}
            if(present && (creation==3 || creation==4)){const auto bytes=store.read_profile(entry.path);if(!bytes)return 0;entry.model=*icc_parse(*bytes);}
            else {entry.model=icc_empty();entry.dirty=true;}
            if(entry.dirty && !entry.writable)entry.dirty=false;
        }else {last_error_=87;return 0;}
        const auto handle=opened.next;opened.next+=4;opened.profiles.emplace(handle,std::move(entry));return handle;
    };
    try{
        if(name.starts_with("OpenColorProfile"))return finish(open(arg(0),static_cast<std::uint32_t>(arg(1)),static_cast<std::uint32_t>(arg(3)),wide));
        // WcsOpenColorProfile：CDM 為 ICC 設定檔時以它開啟；CAMP／GMMP 為 WCS 專用設定檔，沙箱沒有 WCS 模型，提供時不採用。
        if(name.starts_with("WcsOpenColorProfile"))return finish(open(arg(0),static_cast<std::uint32_t>(arg(3)),static_cast<std::uint32_t>(arg(5)),wide));
        if(name=="DllCanUnloadNow")return finish(0);
        if(name=="CloseColorProfile"){
            const auto found=opened.profiles.find(arg(0));if(found==opened.profiles.end())return fail(6);auto entry=std::move(found->second);opened.profiles.erase(found);
            if(entry.writable && entry.dirty && !entry.path.empty() && !write_guest_file(state,memory,entry.path,icc_build(entry.model)))return finish(0);
            return finish(1);
        }
        if(name=="WcsCreateIccProfile"){
            const auto* source=profile(arg(0));if(!source)return fail(6);IcmProfiles::Profile copy;copy.model=source->model;copy.writable=true;
            const auto handle=opened.next;opened.next+=4;opened.profiles.emplace(handle,std::move(copy));return finish(handle);
        }
        if(name.ends_with("ColorProfileHeader")){
            // PROFILEHEADER：前 84 位元組的 DWORD 欄位轉成主機位元序，phReserved 原樣保留。
            auto* entry=profile(arg(0));if(!entry)return fail(6);if(!arg(1))return fail(87);
            std::array<std::uint8_t,128> header{};
            if(name.starts_with("Get")){header=entry->model.header;for(std::size_t at=0;at<84;at+=4)std::reverse(header.begin()+static_cast<std::ptrdiff_t>(at),header.begin()+static_cast<std::ptrdiff_t>(at+4));memory.write(GuestAddress{arg(1)},header);return finish(1);}
            if(!entry->writable)return fail(5);memory.read(GuestAddress{arg(1)},header);for(std::size_t at=0;at<84;at+=4)std::reverse(header.begin()+static_cast<std::ptrdiff_t>(at),header.begin()+static_cast<std::ptrdiff_t>(at+4));
            entry->model.header=header;entry->dirty=true;return finish(1);
        }
        if(name=="GetCountColorProfileElements"){const auto* entry=profile(arg(0));if(!entry)return fail(6);if(!arg(1))return fail(87);memory.store(GuestAddress{arg(1)},32,entry->model.tags.size());return finish(1);}
        if(name=="GetColorProfileElementTag"){
            const auto* entry=profile(arg(0));if(!entry)return fail(6);const auto index=static_cast<std::uint32_t>(arg(1));if(!arg(2) || !index || index>entry->model.tags.size())return fail(87);
            memory.store(GuestAddress{arg(2)},32,entry->model.tags[index-1].first);return finish(1);
        }
        if(name=="IsColorProfileTagPresent"){const auto* entry=profile(arg(0));if(!entry)return fail(6);if(!arg(2))return fail(87);memory.store(GuestAddress{arg(2)},32,entry->model.find(static_cast<std::uint32_t>(arg(1)))!=nullptr);return finish(1);}
        if(name=="GetColorProfileElement"){
            // {h, tag, dwOffset, pcbElement, pElement, pbReference}：pElement 為 NULL 時回報可讀長度；pbReference 表示有其他 tag 共用資料。
            const auto* entry=profile(arg(0));if(!entry)return fail(6);if(!arg(3))return fail(87);const auto tag=static_cast<std::uint32_t>(arg(1)),offset=static_cast<std::uint32_t>(arg(2));
            const auto found=std::find_if(entry->model.tags.begin(),entry->model.tags.end(),[&](const auto& item){return item.first==tag;});if(found==entry->model.tags.end())return fail(2012);
            const auto& data=entry->model.data[found->second];if(offset>data.size())return fail(87);const auto available=static_cast<std::uint32_t>(data.size()-offset);
            if(arg(5))memory.store(GuestAddress{arg(5)},32,entry->model.references(found->second)>1);
            if(!arg(4)){memory.store(GuestAddress{arg(3)},32,available);return finish(1);}
            const auto count=std::min(available,static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32)));memory.write(GuestAddress{arg(4)},std::span(data).subspan(offset,count));memory.store(GuestAddress{arg(3)},32,count);return finish(1);
        }
        if(name.starts_with("SetColorProfileElement")){
            auto* entry=profile(arg(0));if(!entry)return fail(6);if(!entry->writable)return fail(5);auto& model=entry->model;const auto tag=static_cast<std::uint32_t>(arg(1));
            const auto found=std::find_if(model.tags.begin(),model.tags.end(),[&](const auto& item){return item.first==tag;});
            if(name=="SetColorProfileElementReference"){
                // 新 tag 與既有 tag 共用同一份資料；新 tag 已存在時失敗。
                const auto target=std::find_if(model.tags.begin(),model.tags.end(),[&](const auto& item){return item.first==static_cast<std::uint32_t>(arg(2));});
                if(target==model.tags.end())return fail(2012);if(found!=model.tags.end())return fail(2014);model.tags.emplace_back(tag,target->second);entry->dirty=true;return finish(1);
            }
            if(name=="SetColorProfileElementSize"){
                // 大小 0 刪除 tag；其餘調整大小（不存在時以 0 填滿建立）。
                const auto size=static_cast<std::uint32_t>(arg(2));if(size>64*1024*1024)return fail(87);
                if(!size){if(found==model.tags.end())return fail(2012);model.tags.erase(found);}
                else if(found==model.tags.end()){model.tags.emplace_back(tag,model.data.size());model.data.emplace_back(size);}
                else model.data[found->second].resize(size);
                entry->dirty=true;return finish(1);
            }
            // SetColorProfileElement {h, tag, dwOffset, pcbElement, pElement}：寫入位移處，必要時擴大；不存在時建立。
            if(!arg(3) || !arg(4))return fail(87);const auto offset=static_cast<std::uint32_t>(arg(2)),count=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(3)},32));
            if(std::uint64_t{offset}+count>64*1024*1024)return fail(87);
            std::size_t index=0;if(found==model.tags.end()){index=model.data.size();model.tags.emplace_back(tag,index);model.data.emplace_back();}else index=found->second;
            auto& data=model.data[index];if(data.size()<std::size_t{offset}+count)data.resize(std::size_t{offset}+count);memory.read(GuestAddress{arg(4)},std::span(data).subspan(offset,count));entry->dirty=true;return finish(1);
        }
        if(name=="IsColorProfileValid"){
            // 結構可解析、'acsp' 且有描述、版權與白點（device link／abstract 不需白點）。
            const auto* entry=profile(arg(0));if(!entry)return fail(6);if(!arg(1))return fail(87);const auto& model=entry->model;const auto profile_class=icc_u32(model.header,12);
            const bool valid=icc_parse(icc_build(model)) && model.find(0x64657363) && model.find(0x63707274) && (profile_class==0x6c696e6b || profile_class==0x61627374 || model.find(0x77747074));
            memory.store(GuestAddress{arg(1)},32,valid);return finish(1);
        }
        if(name=="GetColorProfileFromHandle"){const auto* entry=profile(arg(0));if(!entry)return fail(6);return output(arg(1),arg(2),icc_build(entry->model));}
        if(name=="CreateProfileFromLogColorSpaceA" || name=="CreateProfileFromLogColorSpaceW"){
            // LOGCOLORSPACE {lcsSignature 'PSOC', lcsVersion, lcsSize, lcsCSType, lcsIntent, lcsEndpoints（FXPT2DOT30）, lcsGamma×3（FXPT16DOT16）, lcsFilename}：
            // LCS_CALIBRATED_RGB 以端點與 gamma 產生矩陣／TRC 設定檔，LCS_sRGB／LCS_WINDOWS_COLOR_SPACE 產生 sRGB；以 GlobalAlloc 交回，呼叫端 GlobalFree。
            if(!arg(0) || !arg(1))return fail(87);std::array<std::uint32_t,17> fields{};for(unsigned i=0;i<17;++i)fields[i]=static_cast<std::uint32_t>(memory.load(GuestAddress{arg(0)+i*4},32));
            if(fields[0]!=0x50534f43)return fail(87);
            IccRgbParameters parameters;
            if(fields[3]==lcs_srgb || fields[3]==lcs_windows_color_space)parameters=icc_srgb_parameters();
            else if(fields[3]==lcs_calibrated_rgb){
                for(unsigned c=0;c<3;++c)for(unsigned i=0;i<3;++i)parameters.colorants[c][i]=static_cast<std::int32_t>(fields[5+c*3+i])/1073741824.0;
                for(unsigned i=0;i<3;++i){parameters.gamma[i]=fields[14+i]/65536.0;if(parameters.gamma[i]<=0)return fail(87);}
                for(unsigned i=0;i<3;++i)parameters.white[i]=parameters.colorants[0][i]+parameters.colorants[1][i]+parameters.colorants[2][i];
                parameters.description="Calibrated RGB";
            }else return fail(87);
            // LCS_GM_BUSINESS→飽和度、GRAPHICS→相對色度、IMAGES→感知、ABS_COLORIMETRIC→絕對色度。
            parameters.intent=fields[4]==1?2U:fields[4]==2?1U:fields[4]==8?3U:0U;
            const auto bytes=icc_rgb_profile(parameters);const auto block=nested_service(state,memory,"kernel32.dll","GlobalAlloc",{0,bytes.size()});if(!block)return fail(8);
            memory.write(GuestAddress{block},bytes);memory.store(GuestAddress{arg(1)},bits,block);return finish(1);
        }
        // 以下為裝置與系統層級的管理；第一個參數多為 pMachineName，只支援本機。
        const bool machine=name.starts_with("GetColorDirectory") || name.starts_with("InstallColorProfile") || name.starts_with("UninstallColorProfile") || name.starts_with("AssociateColorProfile") ||
            name.starts_with("DisassociateColorProfile") || name.starts_with("EnumColorProfiles") || name.starts_with("GetStandardColorSpaceProfile") || name.starts_with("SetStandardColorSpaceProfile") ||
            name.starts_with("RegisterCMM") || name.starts_with("UnregisterCMM");
        if(machine && arg(0))return fail(50);
        if(name.starts_with("GetColorDirectory"))return output(arg(1),arg(2),encode(icm_color_directory,wide));
        if(name.starts_with("InstallColorProfile")){if(!arg(1))return fail(87);return finish(store.install(text(arg(1),wide)));}
        if(name.starts_with("UninstallColorProfile")){if(!arg(1))return fail(87);return finish(store.uninstall(text(arg(1),wide),static_cast<std::uint32_t>(arg(2))!=0));}
        if(name.starts_with("AssociateColorProfile")){if(!arg(1) || !arg(2))return fail(87);return finish(store.associate(text(arg(1),wide),text(arg(2),wide),false));}
        if(name.starts_with("DisassociateColorProfile")){if(!arg(1) || !arg(2))return fail(87);return finish(store.disassociate(text(arg(1),wide),text(arg(2),wide)));}
        if(name.starts_with("GetStandardColorSpaceProfile")){const auto path=store.standard_profile(static_cast<std::uint32_t>(arg(1)));if(!path)return finish(0);return output(arg(2),arg(3),encode(*path,wide));}
        if(name.starts_with("SetStandardColorSpaceProfile")){if(!arg(2))return fail(87);return finish(store.set_standard_profile(static_cast<std::uint32_t>(arg(1)),text(arg(2),wide)));}
        if(name.starts_with("RegisterCMM")){if(!arg(2))return fail(87);return finish(store.register_cmm(static_cast<std::uint32_t>(arg(1)),text(arg(2),wide)));}
        if(name.starts_with("UnregisterCMM"))return finish(store.unregister_cmm(static_cast<std::uint32_t>(arg(1))));
        if(name=="SelectCMM"){
            // 內建 Windows CMM（'Win '）或已登錄的 CMM 才可選取。
            const auto id=static_cast<std::uint32_t>(arg(0));if(id!=0x57696e20 && !store.cmm(id))return fail(icm_error_invalid_cmm);
            char16_t value[5]{};for(unsigned i=0;i<4;++i)value[i]=static_cast<char16_t>((id>>(24-8*i))&0xffU);return finish(store.write_string(u"Settings",u"SelectedCMM",value));
        }
        if(name.starts_with("EnumColorProfiles") || name=="WcsEnumColorProfilesSize"){
            // ENUMTYPE {dwSize, dwVersion, dwFields, pDeviceName, dwMediaType, dwDitheringMode, dwResolution[2], dwCMMType, dwClass, dwDataColorSpace, dwConnectionSpace,
            // dwSignature, dwPlatform, dwProfileFlags, dwManufacturer, dwModel, dwAttributes[2], dwRenderingIntent, dwCreator, dwDeviceClass}：輸出為檔名的 multi-sz。
            const bool size_only=name=="WcsEnumColorProfilesSize",unicode=wide || size_only;const auto record=arg(1);if(!record)return fail(87);
            const auto fields=static_cast<std::uint32_t>(memory.load(GuestAddress{record+8},32));const auto device=memory.load(GuestAddress{record+(x86_?12U:16U)},bits);const auto base=x86_?16U:24U;
            const auto field=[&](unsigned at){return static_cast<std::uint32_t>(memory.load(GuestAddress{record+base+at},32));};
            std::vector<std::u16string> candidates;if((fields&1U) && device)candidates=store.associations(text(device,unicode));else candidates=store.registered();
            // 媒體、混色與解析度需要印表機 DEVMODE，設定檔標頭沒有對應欄位，不參與比對。
            static constexpr std::array<std::tuple<std::uint32_t,unsigned,unsigned>,12> header_fields{{{0x10,16,4},{0x20,20,12},{0x40,24,16},{0x80,28,20},{0x100,32,36},{0x200,36,40},{0x400,40,44},{0x800,44,48},{0x1000,48,52},{0x4000,60,64},{0x8000,64,80},{0x10000,68,12}}};
            std::vector<std::u16string> matched;
            for(const auto& file:candidates){
                const auto bytes=store.read_profile(icm_profile_path(file));if(!bytes)continue;bool ok=true;
                for(const auto& [bit,at,offset]:header_fields)if((fields&bit) && field(at)!=icc_u32(*bytes,offset))ok=false;
                if((fields&0x2000U) && (field(52)!=icc_u32(*bytes,56) || field(56)!=icc_u32(*bytes,60)))ok=false;
                if(ok)matched.push_back(file);
            }
            std::vector<std::uint8_t> bytes;for(const auto& file:matched){const auto item=encode(file,unicode);bytes.insert(bytes.end(),item.begin(),item.end());}bytes.resize(bytes.size()+(unicode?2:1));
            if(size_only){if(!arg(2))return fail(87);memory.store(GuestAddress{arg(2)},32,bytes.size());return finish(1);}
            if(arg(4))memory.store(GuestAddress{arg(4)},32,matched.size());return output(arg(2),arg(3),bytes);
        }
        if(name.starts_with("WcsGetDefaultColorProfile")){
            // {scope, pDeviceName, cptColorProfileType, cpstColorProfileSubType, dwProfileID, cbProfileName／pcbProfileName[, pProfileName]}：只有 ICC 設定檔（CPT_ICC 0）；
            // 沒有裝置名稱時為系統預設 sRGB。回傳檔名（UTF-16）。
            if(static_cast<std::uint32_t>(arg(2))!=0)return fail(2);const auto device=arg(1)?read_utf16(memory,GuestAddress{arg(1)}):std::u16string{};
            const auto path=device.empty()?store.standard_profile(lcs_srgb):store.device_default(device);if(!path)return fail(2);const auto bytes=utf16_bytes(icm_file_part(*path));
            if(name.ends_with("Size")){if(!arg(5))return fail(87);memory.store(GuestAddress{arg(5)},32,bytes.size());return finish(1);}
            if(!arg(6))return fail(87);if(static_cast<std::uint32_t>(arg(5))<bytes.size())return fail(122);memory.write(GuestAddress{arg(6)},bytes);return finish(1);
        }
        if(name=="WcsGetDefaultRenderingIntent"){if(!arg(1))return fail(87);memory.store(GuestAddress{arg(1)},32,store.read_number(u"Settings",u"DefaultRenderingIntent").value_or(0));return finish(1);}
        if(name=="WcsGetCalibrationManagementState"){if(!arg(0))return fail(87);memory.store(GuestAddress{arg(0)},32,store.read_number(u"Settings",u"CalibrationManagement").value_or(0)!=0);return finish(1);}
        if(name=="WcsGetUsePerUserProfiles"){
            // 依裝置保存於 ICM\UsePerUserProfiles；沙箱的使用者與系統設定檔共用同一份儲存，預設為 FALSE。
            if(!arg(0) || !arg(2))return fail(87);memory.store(GuestAddress{arg(2)},32,store.read_number(u"UsePerUserProfiles",read_utf16(memory,GuestAddress{arg(0)})).value_or(0)!=0);return finish(1);
        }
        return fail(50);
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}catch(const std::invalid_argument&){return fail(icm_error_invalid_profile);}
}
}
