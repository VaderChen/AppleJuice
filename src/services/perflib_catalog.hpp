#pragma once
#include "perflib_data.hpp"
#include "x64arm/process_identity.hpp"
#include <functional>
#include <memory>

namespace x64arm::perflib {
struct CatalogEntry {
    ProcessIdentity origin;
    std::uint64_t publication{},handle{};
    unsigned pointer_bytes{};
    Guid provider{};
    std::map<Guid,CounterSet> sets;
};
struct CatalogDirectory;
bool registration_compatible(const CounterSet&,const CounterSet&);
class ProviderPublication {
    struct Storage;
    std::unique_ptr<Storage> storage_;
    explicit ProviderPublication(std::unique_ptr<Storage>);
    friend class ProviderCatalog;
public:
    ~ProviderPublication();
    std::uint64_t publication()const;
    void update(const std::map<Guid,CounterSet>&);
};
class ProviderCatalog {
    std::shared_ptr<CatalogDirectory> directory_;
    explicit ProviderCatalog(std::shared_ptr<CatalogDirectory>);
public:
    using AccessCheck=std::function<bool(const Bytes&)>;
    static std::shared_ptr<ProviderCatalog> open(const std::string& session);
    std::shared_ptr<ProviderPublication> publish(std::uint64_t handle,Guid,unsigned pointer_bytes,
        const std::map<Guid,CounterSet>&,const Bytes& descriptor);
    std::vector<CatalogEntry> snapshot(const AccessCheck&)const;
};
}
