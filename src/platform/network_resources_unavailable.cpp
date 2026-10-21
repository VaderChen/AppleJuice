#include "x64arm/network_resources.hpp"
namespace x64arm::network_resources {
namespace {
class Unavailable final:public Backend {
public:
    Servers servers()override{return {{},1222};}
    Shares shares(std::u16string_view,const Credentials&)override{return {{},1222};}
    Connection connect(const Path&,const Credentials&,const std::filesystem::path&,bool)override{return {{},1222};}
};
}
std::shared_ptr<Backend> make_native_backend(){return std::make_shared<Unavailable>();}
}
