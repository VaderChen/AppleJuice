#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace x64arm {
class SharedWake;
using PipeAccessCheck=std::function<std::uint32_t(const std::vector<std::uint8_t>&)>;
struct PipeResult { std::uint32_t bytes{}, error{}; bool pending{}; };
struct PipePeekInfo {std::uint32_t state{},available{},messages{},message_length{};};
struct PipeInfo { std::uint32_t mode{}, maximum{}, output_size{}, input_size{}, timeout{}, instances{}, integrity{}, policy{}; };
// session 內的本機 IPC；訊息與位元組管道共用有界佇列，不接觸宿主具名管道。
class NamedPipe {
public:
    struct Opened;
    struct Pair;
    struct Transaction;
    // 超過初始配額的寫入已排入資料；Windows I/O 等讀取方消耗完畢後才完成。
    struct WriteCompletion { std::uint64_t end{}; std::uint32_t generation{},bytes{}; };
    static Opened create(const std::string& session, std::u16string name, std::uint32_t access,
                         std::uint32_t mode, std::uint32_t maximum, std::uint32_t output,
                         std::uint32_t input, std::uint32_t timeout, bool first,
                         std::uint32_t integrity = 0x2000, std::uint32_t policy = 1, std::uint32_t access_mask = UINT32_MAX,
                         std::span<const std::uint8_t> security = {},const PipeAccessCheck& authorize = {});
    static Opened open(const std::string& session, std::u16string name, std::uint32_t access, std::span<const std::uint8_t> context = {}, std::uint32_t integrity = 0x2000,
                       const PipeAccessCheck& authorize = {});
    static Pair anonymous(const std::string& session,std::uint32_t size,std::span<const std::uint8_t> security);
    static std::uint32_t available(const std::string& session, std::u16string name, std::uint32_t& timeout);
    static std::uint32_t security(const std::string& session,std::u16string name,const std::function<void(int)>& operation);
    ~NamedPipe();
    PipeResult connect();
    std::uint32_t disconnect();
    PipeResult read(std::span<std::uint8_t> output, bool message, bool peek = false,
                    std::uint32_t* available = nullptr, std::uint32_t* remaining = nullptr);
    PipeResult peek(std::span<std::uint8_t>,PipePeekInfo&);
    PipeResult write(std::span<const std::uint8_t> input,bool nonblocking=false,WriteCompletion* completion=nullptr);
    PipeResult flush();
    // 要求與等待回覆在同一個 session 鎖內登錄；回覆完成後不受 Disconnect 影響。
    PipeResult transact(std::span<const std::uint8_t>,std::span<std::uint8_t>,std::shared_ptr<Transaction>&);

    PipeInfo information() const;
    // FILE_NAME_INFO／FILE_NAME_INFORMATION 的磁碟區相對 UTF-16 名稱。
    std::u16string file_name() const;
    std::uint32_t client_context(std::vector<std::uint8_t>&) const;
    std::uint32_t process_id(bool client,std::uint32_t&) const;
    bool server() const;
    // 不取鎖的提示：為 true 時，同條件的 read（非 peek）必然仍回 pending。false 不代表有資料。
    bool read_would_block() const;
    // 只有同一喚醒物件才可省略排程器定期輪詢；指標不同時保守退回原路徑。
    bool uses_wake(const SharedWake*) const noexcept;
    int descriptor() const;
    int lock_descriptor() const;
    std::uint64_t identity() const;
    static Opened inherit(const std::string& session,int descriptor,std::uint64_t identity,bool server);
private:
    PipeResult read_impl(std::span<std::uint8_t>,bool,bool,std::uint32_t*,std::uint32_t*,PipePeekInfo*);
    struct Impl;
    explicit NamedPipe(std::shared_ptr<Impl> value);
    std::shared_ptr<Impl> impl_;
};
namespace named_pipe_detail {
// 管道擁有者的存活判定：pid 存在、不是 zombie，且出生識別相符；birth 為 0 時只比對 pid。公開供測試。
bool owner_alive(std::uint32_t pid, std::uint64_t birth);
}
struct NamedPipe::Opened { std::shared_ptr<NamedPipe> pipe; std::uint32_t error{},access{}; };
struct NamedPipe::Pair { std::shared_ptr<NamedPipe> read,write;std::uint32_t error{}; };
}
