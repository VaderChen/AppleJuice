#include "x64arm/windows_services.hpp"
#include "x64arm/unicode.hpp"
#include "sandbox_certificate_roots.hpp"
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/x509.h>
#include <functional>
#include <memory>

namespace x64arm {
// cryptui 憑證檢視：從客體 CERT_CONTEXT 讀取 DER，以 OpenSSL 解析主要欄位並在擁有者視窗上顯示。
namespace {
std::string bio_text(const std::function<int(BIO*)>& write){
    std::unique_ptr<BIO,decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()),BIO_free);if(!bio || write(bio.get())<0)return {};
    char* data=nullptr;const auto size=BIO_get_mem_data(bio.get(),&data);return size>0?std::string(data,static_cast<std::size_t>(size)):std::string{};
}
std::string hex(std::span<const unsigned char> bytes){
    static constexpr char digits[]="0123456789ABCDEF";std::string out;
    for(std::size_t i=0;i<bytes.size();++i){if(i)out+=' ';out+=digits[bytes[i]>>4];out+=digits[bytes[i]&15];}return out;
}
std::optional<std::u16string> describe(std::span<const std::uint8_t> der){
    const unsigned char* cursor=der.data();
    std::unique_ptr<X509,decltype(&X509_free)> certificate(d2i_X509(nullptr,&cursor,static_cast<long>(der.size())),X509_free);if(!certificate)return std::nullopt;
    const auto name=[&](X509_NAME* value){return bio_text([&](BIO* out){return X509_NAME_print_ex(out,value,0,XN_FLAG_ONELINE&~ASN1_STRFLGS_ESC_MSB);});};
    const auto time=[&](const ASN1_TIME* value){return bio_text([&](BIO* out){return ASN1_TIME_print(out,value);});};
    std::array<unsigned char,EVP_MAX_MD_SIZE> digest{};unsigned length=0;
    std::string text="簽發對象：\n  "+name(X509_get_subject_name(certificate.get()))+"\n\n簽發者：\n  "+name(X509_get_issuer_name(certificate.get()))+
        "\n\n有效期間：\n  "+time(X509_get0_notBefore(certificate.get()))+"\n  至 "+time(X509_get0_notAfter(certificate.get()));
    if(const auto* serial=X509_get0_serialNumber(certificate.get()))text+="\n\n序號：\n  "+hex({serial->data,static_cast<std::size_t>(serial->length)});
    if(const auto* key=X509_get0_pubkey(certificate.get()))text+="\n\n公開金鑰：\n  "+std::string(OBJ_nid2ln(EVP_PKEY_base_id(key)))+"（"+std::to_string(EVP_PKEY_bits(key))+" 位元）";
    if(X509_digest(certificate.get(),EVP_sha1(),digest.data(),&length))text+="\n\nSHA-1 指紋：\n  "+hex({digest.data(),length});
    if(X509_digest(certificate.get(),EVP_sha256(),digest.data(),&length))text+="\n\nSHA-256 指紋：\n  "+hex({digest.data(),length});
    return utf8_to_utf16_lossy(text);
}
}
std::span<const WindowsServiceDescriptor> certificate_ui_service_descriptors(){
    static constexpr WindowsServiceDescriptor table[]{
        {"cryptui.dll","CryptUIDlgViewCertificateW",2},{"cryptui.dll","CryptUIDlgViewCertificateA",2},{"cryptui.dll","CryptUIDlgCertMgr",1}
    };return table;
}
std::optional<std::uint32_t> WindowsServices::invoke_certificate_ui(GuestState& state,GuestMemory& memory,const WindowsServiceDescriptor& service){
    const WindowsCall arg(state,memory,service);const bool wide=service.symbol.ends_with('W');const unsigned unit=x86_?4:8;const auto bits=static_cast<std::uint8_t>(unit*8);
    const auto fail=[&](std::uint32_t error){last_error_=error;arg.return_to(state,0);return std::optional<std::uint32_t>{};};
    try{
        if(service.symbol=="CryptUIDlgCertMgr"){
            // CRYPTUI_CERT_MGR_STRUCT：dwSize、hwndParent、dwFlags、pwszTitle、pszInitUsageOID。列出受信任根憑證存放區。
            const auto info=arg(0);if(!info || memory.load(GuestAddress{info},32)<3*unit+unit)return fail(87);
            const auto owner=memory.load(GuestAddress{info+unit},bits),title_pointer=memory.load(GuestAddress{info+3*unit},bits);
            if(owner && !gui_.windows.contains(owner))return fail(1400);
            if(!native_gui_)return fail(50);
            const auto roots=sandbox_certificate_roots(filesystem_);if(roots.error)return fail(roots.error);
            GuiCertificateManager manager;std::vector<std::u16string> labels;
            {std::unique_ptr<BIO,decltype(&BIO_free)> input(BIO_new_mem_buf(roots.pem.data(),static_cast<int>(roots.pem.size())),BIO_free);
                while(auto* raw=input?PEM_read_bio_X509(input.get(),nullptr,nullptr,nullptr):nullptr){
                    std::unique_ptr<X509,decltype(&X509_free)> certificate(raw,X509_free);unsigned char* der=nullptr;const auto size=i2d_X509(certificate.get(),&der);if(size<=0)continue;
                    manager.certificates.emplace_back(der,der+size);OPENSSL_free(der);
                    std::string label;const auto* subject=X509_get_subject_name(certificate.get());
                    for(const int nid:{NID_commonName,NID_organizationalUnitName,NID_organizationName}){const auto at=X509_NAME_get_index_by_NID(subject,nid,-1);if(at<0)continue;
                        unsigned char* text=nullptr;const auto length=ASN1_STRING_to_UTF8(&text,X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject,at)));if(length>0){label.assign(reinterpret_cast<char*>(text),static_cast<std::size_t>(length));OPENSSL_free(text);break;}}
                    labels.push_back(utf8_to_utf16_lossy(label.empty()?std::string("(未命名)"):label));
                    if(manager.certificates.size()>=4096)break;
                }}
            std::u16string title=u"憑證";if(title_pointer)title=read_utf16(memory,GuestAddress{title_pointer},1024);
            GuiWindow dialog;dialog.dpi_context=effective_dpi_context();dialog.thread=threads_.current;dialog.title=title;dialog.class_name=u"#32770";dialog.dialog=true;dialog.modal=true;dialog.builtin=true;
            dialog.owner=owner;dialog.width=760;dialog.height=440;dialog.style=0x10000000U;dialog.visible=true;dialog.extra.resize(40);dialog.default_id=3;
            dialog.procedure=resolve("user32.dll","DefDlgProcW").value;dialog.certificate_manager=std::move(manager);
            const auto handle=allocate_gui_window();std::vector<GuiWindowCreation> plans{{handle,std::move(dialog),true}};
            const auto add=[&](std::u16string_view cls,std::u16string text,std::uint32_t id,std::int32_t x,std::int32_t y,std::uint32_t width,std::uint32_t height,std::uint32_t style,const std::vector<std::u16string>* items=nullptr){
                GuiWindow child;child.dpi_context=effective_dpi_context();child.class_name=std::u16string(cls);child.title=std::move(text);child.id=id;child.x=x;child.y=y;child.width=width;child.height=height;
                child.thread=threads_.current;child.parent=handle;child.style=style|0x50000000U;child.visible=true;child.builtin=true;child.control_class=child.class_name;child.procedure=stock_gui_proc(child.class_name,true);
                if(items){for(const auto& label:*items){GuiItem item;item.handle=gui_.next_item++;item.text=label;child.items.push_back(std::move(item));}child.selection=child.items.empty()?-1:0;}
                plans.push_back({allocate_gui_window(),std::move(child),true});
            };
            add(u"static",u"受信任的根憑證授權單位（"+utf8_to_utf16_lossy(std::to_string(labels.size()))+u" 個）",0xffff,20,12,320,22,0);
            add(u"listbox",u"",200,20,40,320,330,0x00a00001U,&labels); // LBS_NOTIFY、WS_VSCROLL、WS_BORDER
            add(u"static",u"選取憑證後按「檢視」顯示詳細內容。",201,360,40,380,330,0);
            add(u"button",u"檢視",3,550,385,90,28,0x00010001U);add(u"button",u"關閉",2,650,385,90,28,0x00010000U);
            return start_gui_creation(state,memory,std::move(plans));
        }
        // CRYPTUI_VIEWCERTIFICATE_STRUCT：dwSize、hwndParent、dwFlags、szTitle、pCertContext……
        const auto info=arg(0);if(!info)return fail(87);
        const std::uint64_t parent_at=unit,flags_at=2*unit,title_at=3*unit,context_at=4*unit;
        if(memory.load(GuestAddress{info},32)<context_at+unit)return fail(87);
        if(arg(1)){memory.check(GuestAddress{arg(1)},4,Access::Write);memory.store(GuestAddress{arg(1)},32,0);}
        auto owner=memory.load(GuestAddress{info+parent_at},bits);if(owner && !gui_.windows.contains(owner))return fail(1400);
        (void)flags_at;const auto title_pointer=memory.load(GuestAddress{info+title_at},bits),context=memory.load(GuestAddress{info+context_at},bits);if(!context)return fail(87);
        // CERT_CONTEXT：dwCertEncodingType、pbCertEncoded、cbCertEncoded。
        const auto encoded=memory.load(GuestAddress{context+unit},bits),size=memory.load(GuestAddress{context+2*unit},32);
        if(!encoded || !size || size>1024U*1024U)return fail(87);
        std::vector<std::uint8_t> der(static_cast<std::size_t>(size));memory.read(GuestAddress{encoded},der);
        const auto text=describe(der);if(!text)return fail(0x80092002U); // CRYPT_E_BAD_ENCODE
        std::u16string title=u"憑證";if(title_pointer)title=wide?read_utf16(memory,GuestAddress{title_pointer},1024):utf8_to_utf16_lossy(read_ansi(memory,title_pointer,2048));
        const auto body=utf16_bytes(*text),caption=utf16_bytes(title);
        const auto scratch=allocate_heap(memory,body.size()+caption.size());if(!scratch)return fail(8);
        memory.write(GuestAddress{scratch},body);memory.write(GuestAddress{scratch+body.size()},caption);
        // MessageBoxW 回傳 IDOK(1)，即 CryptUIDlgViewCertificate 的 TRUE。
        start_callbacks(state,memory,{{resolve("user32.dll","MessageBoxW").value,{owner,scratch,scratch+body.size(),0x40U}}},Continuation::ReturnCallbackFreeHeap,scratch);
        return std::nullopt;
    }catch(const MemoryFault&){return fail(998);}catch(const std::bad_alloc&){return fail(8);}
}
void WindowsServices::show_certificate_selection(std::uint64_t dialog){
    const auto& certificates=gui_.windows.at(dialog).certificate_manager->certificates;std::int32_t selected=-1;std::uint64_t detail=0;
    for(const auto& [child,window]:gui_.windows){if(window.parent!=dialog)continue;if(window.id==200)selected=window.selection;else if(window.id==201)detail=child;}
    if(!detail)return;
    auto& label=gui_.windows.at(detail);
    if(selected<0 || static_cast<std::size_t>(selected)>=certificates.size())label.title=u"請先在左側清單選取一張憑證。";
    else if(const auto text=describe(certificates[static_cast<std::size_t>(selected)]))label.title=*text;else label.title=u"這張憑證無法解碼。";
    present_gui(detail);
}
}
