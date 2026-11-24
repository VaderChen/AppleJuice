#pragma once
#include "x64arm/process_io.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>
namespace x64arm {
using HttpHeaders=std::vector<std::pair<std::string,std::string>>;
struct HttpResult {
    std::uint32_t error{},status{};
    std::string status_line;
    std::string effective_url;
    HttpHeaders headers;
    std::vector<std::uint8_t> body;
    bool operator==(const HttpResult&) const = default;
};
struct HttpInput {
    std::string url,method,proxy,bypass,username,password,proxy_username,proxy_password;
    // WinHTTP 的伺服器與代理認證各自選擇方案；0 沿用 WinINet／SetOption 的自動協商。
    std::uint32_t auth_scheme{},proxy_auth_scheme{};
    // 每筆請求保留 SECURITY_FLAG_IGNORE_*，與父 handle／其他請求隔離。
    std::uint32_t certificate_exceptions{};
    // 信任根：客體請求帶入沙箱信任根（sandbox_certificate_roots 的 PEM，與 Schannel／WinTrust 相同來源），
    // 不使用宿主的系統憑證；certificate_authorities 是原生測試工具指定的 CA 檔案路徑，優先於前者。
    std::string certificate_roots;
    std::string certificate_authorities;
    bool credentials_set{},proxy_credentials_set{};
    bool follow_redirects{true};
    std::uint32_t redirect_policy{1},max_redirects{10},disabled_features{},decompression{},secure_protocols{};
    HttpHeaders headers;
    std::vector<std::uint8_t> body;
    std::array<std::int32_t,4> timeouts{0,60000,30000,30000};
    // WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET：送出 RFC 6455 升級握手，101 之後連線交給 NativeWebSocket。
    bool websocket{};
    // WebSocket 選項（session／request 設定後由子 handle 繼承）：關閉等待秒數、保活間隔毫秒、收／送緩衝大小。
    std::uint32_t websocket_close_timeout{10},websocket_keepalive{30000},websocket_receive_buffer{32768},websocket_send_buffer{32768};
    bool operator==(const HttpInput&) const = default;
};
// RFC 6455 用戶端連線：宿主執行緒持有升級後的連線，處理遮罩、分段、ping／pong 與 close；
// 客體依 WinHTTP 的緩衝區類型（0 二進位訊息、1 二進位片段、2 UTF-8 訊息、3 UTF-8 片段、4 close）收送。
class NativeWebSocket {
public:
    virtual ~NativeWebSocket()=default;
    // 有可交付的資料、close 或連線錯誤。
    virtual bool readable() const=0;
    // 取出至多 capacity 位元組；回傳 0 或 WinHTTP 錯誤碼，type 為緩衝區類型。
    virtual std::uint32_t receive(std::size_t capacity,std::vector<std::uint8_t>& data,std::uint32_t& type)=0;
    // 排入一個訊框；回傳 {錯誤碼, 送出序號}。序號由 written() 達到即表示已寫入連線。
    virtual std::pair<std::uint32_t,std::uint64_t> send(std::uint32_t type,std::span<const std::uint8_t> payload)=0;
    virtual std::pair<std::uint32_t,std::uint64_t> send_close(std::uint16_t status,std::span<const std::uint8_t> reason)=0;
    virtual std::uint64_t written() const=0;
    virtual std::uint32_t error() const=0;
    virtual bool close_sent() const=0;
    // 已收到伺服器 close 時回傳 true 與狀態碼、原因（空 close 為 1005）。
    virtual bool close_status(std::uint16_t& status,std::vector<std::uint8_t>& reason) const=0;
    // WinHttpWebSocketClose 之後丟棄尚未交付與後續到達的資料訊框，只等伺服器 close。
    virtual void discard_data()=0;
    // WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL：閒置時送出未請求的 pong，0 停用。
    virtual void set_keepalive(std::uint32_t interval)=0;
    virtual void cancel() noexcept=0;
    virtual process_io::Counters io_counters() const {return {};}
};
class NativeHttp {
public:
    virtual ~NativeHttp()=default;
    virtual bool done() const=0;
    virtual HttpResult take()=0;
    // 記憶體快取沒有網路傳輸；真正的宿主傳輸覆寫快照與取消。
    virtual process_io::Counters io_counters() const {return {};}
    virtual void cancel() noexcept {}
    // 等到完成或期限；回傳是否已完成。傳輸實作以條件變數覆寫，預設以短間隔檢查。
    virtual bool wait_until(std::chrono::steady_clock::time_point deadline) {
        while(!done()){if(std::chrono::steady_clock::now()>=deadline)return false;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        return true;
    }
    // WebSocket 升級成功（101 且驗證 Sec-WebSocket-Accept）後取出連線；其他請求為空。
    virtual std::shared_ptr<NativeWebSocket> upgrade() {return {};}
};
std::shared_ptr<NativeHttp> start_native_http(HttpInput);
// 宿主內部的同步下載（CRL 等）：與客體請求共用同一個傳輸、代理與 TLS 政策；逾時取消並回報 ERROR_WINHTTP_TIMEOUT。
inline HttpResult fetch_native_http(HttpInput input,std::chrono::steady_clock::time_point deadline) {
    HttpResult failed;
    const auto request=start_native_http(std::move(input));
    if(!request){failed.error=50;return failed;}
    if(!request->wait_until(deadline)){request->cancel();failed.error=12002;return failed;}
    return request->take();
}
struct HttpHandle {
    enum class Kind {Session,Connection,Request,Resolver,Probe,WebSocket} kind{Kind::Session};
    std::uint64_t parent{},context{};
    std::uint64_t callback{};
    std::uint32_t callback_flags{};
    bool asynchronous{};
    std::map<std::uint64_t,std::uint64_t> opening; // 每個客體執行緒各自保留同步 InternetOpenUrl 的未完成請求
    std::string agent,host,path;
    std::uint16_t port{};
    bool secure{},sent{},received{};
    bool inet{},cache_hit{};
    std::uint32_t cache_flags{};
    std::uint32_t total{};
    std::size_t cursor{};
    HttpInput input;
    HttpResult response;
    std::shared_ptr<NativeHttp> pending;
    process_io::Counters io_accounted;
    // 升級請求在 101 回應後保存連線，WinHttpWebSocketCompleteUpgrade 移交給新的 WebSocket handle。
    std::shared_ptr<NativeWebSocket> websocket;
    process_io::Counters websocket_io_accounted;
    bool upgraded{},close_delivered{},closed{};
    // 每個客體執行緒進行中的 WebSocket 操作：{種類(1 收、2 送、3 關閉、4 shutdown), 送出序號, 期限(steady ns)}；
    // 重新進入時沿用同一筆，避免重送訊框。
    std::map<std::uint64_t,std::array<std::uint64_t,3>> websocket_operations;
    bool operator==(const HttpHandle&) const = default;
};
// WinINet Cookie（RFC 6265）：domain 小寫且無前導點；expires 為 FILETIME，0 表示 session Cookie。
struct HttpCookie {
    std::string name,value,domain,path;
    std::uint64_t expires{},created{};
    bool host_only{},secure{},http_only{};
    bool operator==(const HttpCookie&) const = default;
};
struct HttpState {
    bool cache_active{};
    // WinINet session Cookie 只存在於本程序；持久 Cookie 存於登錄（url_cache::cookie_index）。
    std::vector<HttpCookie> session_cookies;
    std::uint64_t next{0x68000000};
    std::map<std::uint64_t,HttpHandle> handles;
    std::map<std::uint64_t,std::uint64_t> connection_probes;
    // WinINet 延伸錯誤屬於呼叫執行緒；不能讓另一個請求覆蓋目前執行緒的回應。
    std::map<std::uint64_t,std::pair<std::uint32_t,std::string>> last_responses;
    // WinHttpCloseHandle 關閉 WebSocket 時，其他執行緒停駐中的操作改回報 ERROR_WINHTTP_OPERATION_CANCELLED。
    std::map<std::uint64_t,std::uint64_t> canceled_websocket_operations;
    std::string nearest_server;
    HttpState()=default;
    HttpState(const HttpState&);
    HttpState& operator=(const HttpState&);
    HttpState(HttpState&&) noexcept=default;
    HttpState& operator=(HttpState&&) noexcept=default;
    bool operator==(const HttpState&) const = default;
};
}
