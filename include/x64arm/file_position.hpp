#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace x64arm {
struct SharedFilePosition;
struct FileTransferMetadata {
    std::uint64_t device{},inode{};
    std::uint32_t access{},sharing{},mode{};
    bool readonly{};
    std::string path;
    std::uint64_t object{};
    bool manage_volume_at_open{};
};
// 獨立 open 保存值；傳輸後同一 FILE_OBJECT 使用共用游標。
class FilePosition {
    std::uint64_t local_{};
    std::shared_ptr<SharedFilePosition> shared_;
public:
    FilePosition()=default;
    explicit FilePosition(std::uint64_t value):local_(value){}
    operator std::uint64_t() const;
    FilePosition& operator=(std::uint64_t value);
    bool operator==(const FilePosition& other)const{return static_cast<std::uint64_t>(*this)==static_cast<std::uint64_t>(other);}
    std::shared_ptr<void> lock() const;
    std::uint32_t publish(const std::string& session,std::uint64_t identity,const FileTransferMetadata& metadata);
    std::uint64_t identity() const;
    static std::pair<FilePosition,std::uint32_t> inherit(const std::string& session,std::uint64_t identity,int file_descriptor,FileTransferMetadata& metadata);
};
// CRT 也會使用一般記憶體游標；保留同一組 read／seek／write 流程。
class FilePositionReference {
    std::uint64_t* local_{};
    FilePosition* position_{};
public:
    explicit FilePositionReference(std::uint64_t& value):local_(&value){}
    explicit FilePositionReference(FilePosition& value):position_(&value){}
    operator std::uint64_t()const{return position_?static_cast<std::uint64_t>(*position_):*local_;}
    FilePositionReference& operator=(std::uint64_t value){if(position_)*position_=value;else *local_=value;return *this;}
    FilePositionReference& operator+=(std::uint64_t value){return *this=static_cast<std::uint64_t>(*this)+value;}
    FilePositionReference& operator++(){return *this+=1;}
    FilePositionReference& operator--(){return *this=static_cast<std::uint64_t>(*this)-1;}
    std::shared_ptr<void> lock()const{return position_?position_->lock():std::shared_ptr<void>{};}
};
}
