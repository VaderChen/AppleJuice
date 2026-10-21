#pragma once
#include "x64arm/readonly_files.hpp"
#include "pe_signature_roots.hpp"
#include "x64arm/public_certificate_roots.hpp"
#include <climits>
#include <memory>
#include <string_view>
#include <openssl/err.h>
#include <openssl/pem.h>

namespace x64arm {
struct SandboxCertificateRoots {std::string pem;std::uint32_t error{};};
// 預設只讀取編譯進引擎的 Microsoft／Mozilla 公開 CA，不碰磁碟或宿主信任來源。
template<class Files>
inline SandboxCertificateRoots sandbox_certificate_roots(Files& files) {
    SandboxCertificateRoots result{std::string(pe_signature_detail::roots_pem),0};
    result.pem.append(public_certificate_detail::roots_pem);
    if(files.certificate_roots_file().empty())return result;
    const auto opened=files.open_reader("C:/"+files.certificate_roots_file());
    if(opened.error){result.error=opened.error;return result;}
    if(opened.metadata.size>16U*1024U*1024U){result.error=8;return result;}
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(opened.metadata.size));const auto read=opened.file->read(bytes,0);
    if(read.error || read.bytes!=bytes.size()){result.error=read.error?read.error:38U;return result;}
    if(bytes.empty()){result.error=13;return result;}
    std::unique_ptr<BIO,decltype(&BIO_free)> input(BIO_new_mem_buf(bytes.data(),static_cast<int>(bytes.size())),BIO_free);
    if(!input){result.error=8;return result;}unsigned count=0;ERR_clear_error();
    while(auto* certificate=PEM_read_bio_X509(input.get(),nullptr,nullptr,nullptr)){X509_free(certificate);if(++count>8192){result.error=8;return result;}}
    const auto error=ERR_peek_last_error();ERR_clear_error();
    if(!count || (error && ERR_GET_REASON(error)!=PEM_R_NO_START_LINE)){result.error=13;return result;}
    result.pem.push_back('\n');result.pem.append(reinterpret_cast<const char*>(bytes.data()),bytes.size());return result;
}
inline bool add_certificate_root_pem(X509_STORE* store,std::string_view pem) {
    if(pem.size()>INT_MAX)return false;
    std::unique_ptr<BIO,decltype(&BIO_free)> input(BIO_new_mem_buf(pem.data(),static_cast<int>(pem.size())),BIO_free);if(!input)return false;
    unsigned count=0;ERR_clear_error();
    while(auto* raw=PEM_read_bio_X509(input.get(),nullptr,nullptr,nullptr)){
        std::unique_ptr<X509,decltype(&X509_free)> certificate(raw,X509_free);
        if(!X509_STORE_add_cert(store,certificate.get())){
            const auto error=ERR_peek_last_error();ERR_clear_error();if(ERR_GET_REASON(error)!=X509_R_CERT_ALREADY_IN_HASH_TABLE)return false;
        }
        ++count;
    }
    const auto error=ERR_peek_last_error();ERR_clear_error();return count && (!error || ERR_GET_REASON(error)==PEM_R_NO_START_LINE);
}
}
