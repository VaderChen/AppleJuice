#include "x64arm/native_authentication.hpp"
#include "x64arm/unicode.hpp"
#import <OpenDirectory/OpenDirectory.h>
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
namespace x64arm::authentication {
namespace {
Result failure(NSError* error){
    if(!error)return {0xc000006dU,0,{}};
    std::uint32_t reason=0;
    switch(error.code){
    case kODErrorCredentialsAccountDisabled:case kODErrorCredentialsAccountInactive:reason=0xc0000072U;break;
    case kODErrorCredentialsAccountExpired:reason=0xc0000193U;break;
    case kODErrorCredentialsAccountLocked:case kODErrorCredentialsAccountTemporarilyLocked:reason=0xc0000234U;break;
    case kODErrorCredentialsPasswordExpired:reason=0xc0000071U;break;
    case kODErrorCredentialsPasswordChangeRequired:reason=0xc0000224U;break;
    case kODErrorCredentialsInvalidLogonHours:reason=0xc000006fU;break;
    case kODErrorCredentialsInvalidComputer:reason=0xc0000070U;break;
    case kODErrorCredentialsServerUnreachable:case kODErrorCredentialsServerNotFound:
    case kODErrorCredentialsServerError:case kODErrorCredentialsServerTimeout:
    case kODErrorCredentialsServerCommunicationError:return {0xc000005eU,0,{}};
    case kODErrorCredentialsMethodNotSupported:return {0xc00000bbU,0,{}};
    default:break;
    }
    return {reason?0xc000006eU:0xc000006dU,reason,{}};
}
NSString* string(std::u16string_view value){return [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar*>(value.data()) length:value.size()];}
std::u16string text(NSString* value){std::u16string out(value.length,0);if(!out.empty())[value getCharacters:reinterpret_cast<unichar*>(out.data()) range:NSMakeRange(0,value.length)];return out;}
std::optional<std::uint32_t> numeric(ODRecord* record,ODAttributeType attribute){
    NSError* error=nil;NSArray* values=[record valuesForAttribute:attribute error:&error];
    if(error || values.count!=1 || ![values[0] isKindOfClass:[NSString class]])return {};
    const char* first=[values[0] UTF8String];if(!first)return {};std::string_view v(first);std::uint32_t result{};
    const auto parsed=std::from_chars(v.data(),v.data()+v.size(),result);if(parsed.ec!=std::errc{} || parsed.ptr!=v.data()+v.size())return {};return result;
}
class OpenDirectoryBackend final:public Backend {
public:
    Result verify(std::u16string_view user,std::u16string_view domain,std::u16string_view password)override {
        @autoreleasepool {
            // 本機明碼驗證；不把 NTLM／Kerberos 或遠端網域偽裝成本機驗證。
            if(!domain.empty() && domain!=u"." && domain!=u"X64ARM")return {0xc000005eU,0,{}};
            if(user.empty() || user.size()>256 || password.size()>32767 || user.find_first_of(u"\\/@\0",0,4)!=std::u16string_view::npos || password.find(char16_t{})!=std::u16string_view::npos)return {0xc000000dU,0,{}};
            NSError* error=nil;ODNode* node=[[ODNode alloc] initWithSession:[ODSession defaultSession] name:@"/Local/Default" error:&error];
            if(!node)return {0xc000005eU,0,{}};
            ODRecord* record=[node recordWithRecordType:kODRecordTypeUsers name:string(user) attributes:@[kODAttributeTypeUniqueID,kODAttributeTypePrimaryGroupID,kODAttributeTypeRecordName,kODAttributeTypeFullName] error:&error];
            if(!record)return failure(error);
            NSString* secret=string(password);const BOOL valid=[record verifyPassword:secret error:&error];secret=nil;
            if(!valid)return failure(error);
            const auto uid=numeric(record,kODAttributeTypeUniqueID),gid=numeric(record,kODAttributeTypePrimaryGroupID);
            if(!uid || !gid)return {0xc000006dU,0,{}};
            NSArray* names=[record valuesForAttribute:kODAttributeTypeRecordName error:&error];
            if(error || !names.count || ![names[0] isKindOfClass:[NSString class]])return {0xc000006dU,0,{}};
            Identity identity;identity.uid=*uid;identity.gid=*gid;identity.name=text(names[0]);
            NSArray* full=[record valuesForAttribute:kODAttributeTypeFullName error:&error];if(!error && full.count && [full[0] isKindOfClass:[NSString class]])identity.full_name=text(full[0]);
            // POSIX 名稱必須對應同一個已驗證 UID，群組才可加入權杖。
            const auto native_name=utf16_to_utf8(identity.name);std::vector<char> buffer(16384);struct passwd entry{},*found=nullptr;int code=0;
            do{code=::getpwnam_r(native_name.c_str(),&entry,buffer.data(),buffer.size(),&found);if(code==ERANGE && buffer.size()<1048576)buffer.resize(buffer.size()*2);else break;}while(true);
            if(code || !found || entry.pw_uid!=*uid || entry.pw_gid!=*gid)return {0xc000006dU,0,{}};
            int count=0;::getgrouplist(native_name.c_str(),static_cast<int>(*gid),nullptr,&count);if(count<1 || count>1024)return {0xc000009aU,0,{}};
            std::vector<int> groups(static_cast<std::size_t>(count));if(::getgrouplist(native_name.c_str(),static_cast<int>(*gid),groups.data(),&count)<0)return {0xc000009aU,0,{}};
            for(int group:groups)identity.groups.push_back(static_cast<std::uint32_t>(group));
            std::sort(identity.groups.begin(),identity.groups.end());identity.groups.erase(std::unique(identity.groups.begin(),identity.groups.end()),identity.groups.end());
            return {0,0,std::move(identity)};
        }
    }
};
}
std::shared_ptr<Backend> native_backend(){return std::make_shared<OpenDirectoryBackend>();}
}
