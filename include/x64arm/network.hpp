#pragma once
#include <cstdint>
#include <atomic>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "x64arm/socket_namespace.hpp"
namespace x64arm {
namespace socket_database {struct Job;}
struct PendingSocketLookup {
    std::uint64_t window{},buffer{};
    std::uint32_t message{},capacity{};
    std::shared_ptr<socket_database::Job> job;
    bool operator==(const PendingSocketLookup&) const = default;
};
// 純量狀態透過 descriptor 支援的共享頁傳遞；不傳送宿主指標或 C++ 容器。
struct SharedSocketData {
    std::uint64_t magic{UINT64_C(0x32534b5341523458)};
    std::uint64_t accept_listener{},accepted_at{UINT64_MAX};
    bool overlapped{true},accept_context{},connect_context{},listening{},reusable{},send_shutdown{};
    std::array<std::uint8_t,28> bound_address{};
    std::uint32_t bound_size{};
    std::atomic<std::uint64_t> watch_generation{};
    std::atomic<std::uint32_t> watch_rearm{},watch_recorded{};
    std::array<std::atomic<std::uint32_t>,10> watch_errors{};
    std::atomic<std::uint32_t> zero_send_buffer{};
};
class SharedSocketState {
public:
    explicit SharedSocketState(int inherited_descriptor=-1);
    ~SharedSocketState();
    SharedSocketState(const SharedSocketState&)=delete;
    SharedSocketState& operator=(const SharedSocketState&)=delete;
    int descriptor() const {return descriptor_;}
    const std::pair<std::uint64_t,std::uint64_t>& identity() const {return identity_;}
    SharedSocketData& data() const {return *data_;}
private:
    int descriptor_{-1};SharedSocketData* data_{};
    std::pair<std::uint64_t,std::uint64_t> identity_{};
};
std::uint64_t socket_clock_ticks();
struct NativeSocket {
    int descriptor{-1};
    int family{}, type{}, protocol{};
    std::shared_ptr<SharedSocketState> shared;
    bool& overlapped;
    std::uint64_t completion_port{},completion_key{};
    std::uint64_t &accept_listener,&accepted_at;
    bool &accept_context,&connect_context,&listening,&reusable,&send_shutdown;
    std::uint8_t completion_modes{};
    bool io_signaled{true};
    // UDP 共享佇列「已確認為空」時狀態檔的身分與時間；原生 socket 無資料且狀態檔未變時可直接判定不可讀。
    mutable std::array<std::int64_t,6> datagram_idle_stamp{};
    mutable bool datagram_idle{};
    NativeSocket(int fd,int af,int kind,int proto,std::shared_ptr<SharedSocketState> state={});
    std::vector<std::uint8_t> bound_address() const;
    void set_bound_address(const std::vector<std::uint8_t>&);
    ~NativeSocket();
    NativeSocket(const NativeSocket&) = delete;
    NativeSocket& operator=(const NativeSocket&) = delete;
};
struct SocketDuplication {
    std::shared_ptr<NativeSocket> socket;
    std::uint32_t sender{};
    std::uint64_t sequence{};
    bool operator==(const SocketDuplication&) const = default;
};
struct InterfaceAddress {
    std::vector<std::uint8_t> bytes;
    std::uint8_t prefix{};
};
struct NetworkInterface {
    std::string name;
    std::string description;
    std::uint32_t index{}, mtu{}, type{}, status{}, admin_status{}, media_state{};
    std::uint32_t last_change{};
    bool multicast{},hardware{},statistics_available{};
    bool dhcp_enabled{};
    std::map<std::uint8_t,std::vector<std::uint8_t>> dhcp_options;
    std::int64_t lease_obtained{},lease_expires{};
    std::array<std::uint8_t,4> dhcp_server{};
    std::vector<std::array<std::uint8_t,4>> gateways,wins;
    // IPv6 預設路由閘道（Windows SOCKADDR_IN6 位元組，scope 為介面索引）。
    std::vector<std::vector<std::uint8_t>> ipv6_gateways;
    std::uint64_t speed{};
    std::array<std::uint64_t,18> statistics{};
    std::vector<std::uint8_t> physical;
    std::vector<InterfaceAddress> addresses;
};
struct NetworkRoute {
    std::uint32_t status{};
    std::array<std::uint8_t,104> row{};
    std::array<std::uint8_t,28> source{};
};
struct NetworkConfiguration {
    bool captured{};
    std::uint64_t host_generation{}; // 擷取時的宿主網路設定世代；變更後重新擷取。
    bool descriptions_loaded{}; // 介面顯示名稱（SystemConfiguration）第一次需要時才取得。
    bool metadata_available{},routing_enabled{},proxy_enabled{};
    std::string host_name,domain_name,scope_id;
    std::vector<NetworkInterface> interfaces;
    std::vector<std::vector<std::uint8_t>> dns;
    std::map<std::vector<std::uint8_t>,NetworkRoute> routes;
};
bool snapshot_network_configuration(NetworkConfiguration&);
struct PendingAccept {
    std::uint64_t listener{},accepted{},thread{},buffer{},overlapped{},event{},port{},key{};
    std::uint32_t receive_size{},local_size{},remote_size{},error{},transferred{};
    bool connected{},done{},notify{};
    bool operator==(const PendingAccept&) const = default;
};
struct SocketBuffer {
    std::uint64_t pointer{};
    std::uint32_t length{};
    bool operator==(const SocketBuffer&) const = default;
};
struct PendingSocketIo {
    enum class Kind {Connect,Disconnect,Send,Receive,Control} kind{};
    std::uint64_t socket{},thread{},overlapped{},event{},routine{},port{},key{},address_pointer{},address_size_pointer{};
    std::uint32_t flags{},error{},transferred{},result_flags{};
    bool started{},connected{},done{},notify{},addressed{},attempted{},io_recorded{};
    std::uint64_t io_accounted{};
    std::uint64_t message_pointer{},control_pointer{};
    std::uint32_t control_capacity{},control_used{};
    std::vector<SocketBuffer> buffers;
    std::vector<std::uint8_t> data,address,control;
    bool operator==(const PendingSocketIo&) const = default;
};
inline std::uint32_t socket_io_status(std::uint32_t error){
    switch(error){case 0:return 0;case 995:return 0xc0000120;case 10054:return 0xc000020d;case 10053:return 0xc0000241;case 10055:return 0xc000009a;case 10060:return 0xc00000b5;case 10061:return 0xc0000236;case 10051:return 0xc000023c;case 10065:return 0xc000023d;case 10057:return 0xc000020c;case 10040:return 0x80000005;default:return 0xc000000d;}
}
inline std::uint32_t socket_io_error(std::uint32_t status){
    switch(status){case 0:return 0;case 0x103:return 996;case 0xc0000120:return 995;case 0xc000020d:return 10054;case 0xc0000241:return 10053;case 0xc000009a:return 10055;case 0xc00000b5:return 10060;case 0xc0000236:return 10061;case 0xc000023c:return 10051;case 0xc000023d:return 10065;case 0xc000020c:return 10057;case 0x80000005:return 10040;default:return 10022;}
}
struct NetworkState {
    std::map<std::uint64_t,socket_namespace::Lookup> namespace_lookups;
    std::map<std::uint32_t,SocketDuplication> duplications;
    std::map<std::uint64_t,PendingSocketLookup> pending_lookups;
    std::map<std::uint64_t,std::shared_ptr<socket_database::Job>> resolvers;
    std::uint64_t next_lookup{0xa0000};
    std::vector<PendingAccept> pending_accepts;
    std::vector<PendingSocketIo> pending_socket_io;
    std::map<std::uint64_t,std::pair<std::uint64_t,std::uint32_t>> completion_flags;
    // 既有 NTSTATUS 橋接無法一對一表示的 Winsock 錯誤，保留實際完成結果。
    std::map<std::uint64_t,std::pair<std::uint64_t,std::uint32_t>> completion_errors;

    bool external_io{true};
    std::shared_ptr<NetworkConfiguration> configuration{std::make_shared<NetworkConfiguration>()};
    std::uint32_t startups{};
    std::uint64_t next_socket{0x90000};
    std::map<std::uint64_t,std::shared_ptr<NativeSocket>> sockets;
    std::set<std::uint64_t> addresses;
    std::map<std::uint64_t,std::uint64_t> service_buffers;
    NetworkState() = default;
    ~NetworkState();
    NetworkState(const NetworkState& other);
    NetworkState& operator=(const NetworkState& other);
    NetworkState(NetworkState&&) noexcept = default;
    NetworkState& operator=(NetworkState&&) noexcept = default;
    bool operator==(const NetworkState& other) const;
};
}
