#pragma once
#include "x64arm/windows_tokens.hpp"
#include <memory>
#include <mutex>
#include <utility>

namespace x64arm {
// 未命名的私有檔案持有真正權杖物件；SCM_RIGHTS 傳遞引用而非快照。
class SharedToken {
public:
    class Transaction {
    public:
        explicit Transaction(SharedToken&);
        ~Transaction();
        Transaction(const Transaction&)=delete;
        Transaction& operator=(const Transaction&)=delete;
        VirtualToken read() const;
        void write(const VirtualToken&);
    private:
        SharedToken& token_;
        std::unique_lock<std::recursive_mutex> lock_;
    };
    ~SharedToken();
    static std::shared_ptr<SharedToken> create(const VirtualToken&);
    static std::shared_ptr<SharedToken> inherit(int);
    VirtualToken read();
    // revision 屬於呼叫端快照；跨程序提交後，下次檢查立即取得新內容。
    bool refresh(VirtualToken&,std::uint64_t& revision);
    int descriptor()const{return descriptor_;}
    const std::pair<std::uint64_t,std::uint64_t>& identity()const{return identity_;}
private:
    int descriptor_{-1};
    void* mapped_{};
    void map_storage();
    void validate_storage() const;
    unsigned lock_depth_{};
    std::recursive_mutex mutex_;
    std::pair<std::uint64_t,std::uint64_t> identity_{};
};
}
