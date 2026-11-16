// 核心既有目錄的宿主表示；只保存虛擬物件身分與安全描述元，不映射宿主路徑。
namespace x64arm {
namespace {
constexpr std::uint64_t directory_signature=0x315249444f343658ULL;
struct DirectoryRecord {std::uint64_t signature{directory_signature};std::uint32_t index{},reserved{};};
std::uint32_t directory_io_error(){return errno==EACCES || errno==EPERM?5U:errno==ENOMEM || errno==ENOSPC?8U:6U;}
}
ObjectDirectory::~ObjectDirectory(){
#if !defined(_WIN32)
    if(file_>=0)::close(file_);if(directory_>=0)::close(directory_);
#endif
}
std::shared_ptr<ObjectDirectory> ObjectDirectory::open(const std::string& session,std::uint32_t index,const ObjectSecurityFactory& security){
#if !defined(_WIN32)
    if(index>=builtin_object_directories.size())throw SecurityDescriptorError{87};
    auto object=std::make_shared<ObjectDirectory>();object->index_=index;
    object->directory_=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat info{};if(object->directory_<0 || ::fstat(object->directory_,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))throw SecurityDescriptorError{5};
    SharedEventLock lock(object->directory_);const auto name="object-directory-"+std::to_string(index);
    object->file_=::openat(object->directory_,name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    const bool created=object->file_>=0;
    if(!created && errno==EEXIST)object->file_=::openat(object->directory_,name.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);
    if(object->file_<0)throw SecurityDescriptorError{directory_io_error()};
    if(created){
        try{DirectoryRecord record;record.index=index;
            if(::pwrite(object->file_,&record,sizeof(record),0)!=sizeof(record))throw SecurityDescriptorError{directory_io_error()};
            set_shared_object_security(object->file_,security());
        }catch(...){(void)::unlinkat(object->directory_,name.c_str(),0);throw;}
    }
    DirectoryRecord record;
    if(::fstat(object->file_,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1 || info.st_size!=sizeof(record) ||
       ::pread(object->file_,&record,sizeof(record),0)!=sizeof(record) || record.signature!=directory_signature || record.index!=index)throw SecurityDescriptorError{6};
    return object;
#else
    (void)session;(void)index;(void)security;throw SecurityDescriptorError{50};
#endif
}
std::shared_ptr<ObjectDirectory> ObjectDirectory::inherit(const std::string& session,int descriptor){
#if !defined(_WIN32)
    auto object=std::make_shared<ObjectDirectory>();object->file_=descriptor;
    (void)::fcntl(descriptor,F_SETFD,FD_CLOEXEC);
    object->directory_=::open(session.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
    struct stat info{};DirectoryRecord record;
    if(object->directory_<0 || ::fstat(object->directory_,&info) || info.st_uid!=::geteuid() || (info.st_mode&077))throw SecurityDescriptorError{5};
    SharedEventLock lock(object->directory_);
    if(::fstat(descriptor,&info) || !S_ISREG(info.st_mode) || info.st_uid!=::geteuid() || (info.st_mode&077) || info.st_nlink!=1 || info.st_size!=sizeof(record) ||
       ::pread(descriptor,&record,sizeof(record),0)!=sizeof(record) || record.signature!=directory_signature || record.index>=builtin_object_directories.size())throw SecurityDescriptorError{6};
    struct stat expected{};const auto name="object-directory-"+std::to_string(record.index);
    if(::fstatat(object->directory_,name.c_str(),&expected,AT_SYMLINK_NOFOLLOW) || info.st_dev!=expected.st_dev || info.st_ino!=expected.st_ino)throw SecurityDescriptorError{6};
    object->index_=record.index;return object;
#else
    (void)session;(void)descriptor;throw SecurityDescriptorError{50};
#endif
}
}
