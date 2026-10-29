#include "x64arm/readonly_files.hpp"
#include "x64arm/windows_paths.hpp"
#include <algorithm>
#include <set>
#if !defined(_WIN32)
#include <unistd.h>
#endif
namespace x64arm {
namespace {
char drive_root(std::string path){
    std::replace(path.begin(),path.end(),'\\','/');if(path.starts_with("//?/"))path.erase(0,4);
    if(path.size()!=3 || path[1]!=':' || path[2]!='/')return 0;char letter=path[0];if(letter>='a' && letter<='z')letter=static_cast<char>(letter-32);return letter>='A' && letter<='Z'?letter:0;
}
}
std::vector<FileVolume> ReadOnlyFiles::volumes() const{return volumes_->volumes();}
FileVolumeState ReadOnlyFiles::volume_state() const{return volumes_->snapshot();}
FileVolumeProperties ReadOnlyFiles::volume_properties(char volume) const{return volumes_->properties(volume);}
std::uint32_t ReadOnlyFiles::set_volume_label(char volume,std::u16string label){return volumes_->set_label(volume,std::move(label),writable_);}
std::uint32_t ReadOnlyFiles::set_volume_object_id(char volume,const std::array<std::uint8_t,64>& value){return volumes_->set_object_id(volume,value,writable_);}
FileVolumeLocation ReadOnlyFiles::volume_location(std::string path,bool follow) const {
    std::string leaf;auto resolved=resolve(std::move(path),true,&leaf,true,follow);
#if !defined(_WIN32)
    if(resolved.descriptor>=0)::close(resolved.descriptor);
#endif
    return {resolved.volume,std::move(resolved.volume_path),std::move(resolved.mount_path),resolved.error?resolved.error:resolved.readonly_file?5U:resolved.volume?0U:50U};
}
FileVolumePaths ReadOnlyFiles::volume_mount_paths(char volume,bool relative) const try {
    const auto state=volume_state();if(state.error)return {{},state.error};const auto known=volumes();if(std::none_of(known.begin(),known.end(),[&](const auto& item){return item.id==volume;}))return {{},3};
    for(const auto& point:state.folders){const auto entry=reparse(windows_volume_path(point.source)+point.path);if(entry.error || entry.kind!=3 || entry.device!=point.device || entry.id!=point.inode)return {{},entry.error && entry.error!=4390?entry.error:13U};}
    FileVolumePaths result;if(relative){for(const auto& point:state.folders)if(point.source==volume)result.paths.push_back(point.path+'/');return result;}
    std::set<std::string> paths;std::size_t bytes=0;
    const auto visit=[&](auto&& self,char current,std::string path,std::set<char> visited)->bool {
        if(current==volume && paths.insert(path).second){bytes+=path.size()+1;if(paths.size()>4096 || bytes>65536){result.error=8;return false;}}
        if(!visited.insert(current).second)return true;
        for(const auto& point:state.folders)if(point.source==current)if(!self(self,point.target,path+point.path+'/',visited))return false;return true;
    };
    for(const auto& [drive,target]:state.drives)if(!visit(visit,target,std::string(1,drive)+":/",{}))return result;
    result.paths.assign(paths.begin(),paths.end());return result;
}catch(const std::bad_alloc&){return {{},8};}
std::uint32_t ReadOnlyFiles::set_volume_mount(std::string path,char target) try {
    if(!writable_)return 5;auto cache=std::make_shared<Cache>();std::uint32_t result=0;
    if(const auto drive=drive_root(path))result=volumes_->set_drive(drive,target,true);
    else {const auto location=volume_location(std::move(path));if(location.error)return location.error;result=volumes_->set_folder(location.volume,location.path,target,true);}
    if(!result)cache_=std::move(cache);return result;
}catch(const std::bad_alloc&){return 8;}
std::uint32_t ReadOnlyFiles::delete_volume_mount(std::string path) try {
    if(!writable_)return 5;auto cache=std::make_shared<Cache>();std::uint32_t result=0;
    if(const auto drive=drive_root(path))result=volumes_->delete_drive(drive,true);
    else {const auto location=volume_location(std::move(path));if(location.error)return location.error;result=volumes_->delete_folder(location.volume,location.path,true);}
    if(!result)cache_=std::move(cache);return result;
}catch(const std::bad_alloc&){return 8;}
}
