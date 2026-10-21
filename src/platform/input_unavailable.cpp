#include "x64arm/input.hpp"
namespace x64arm::input {
namespace {
class Unavailable final:public Backend {
public:
    std::vector<Device> devices()override{return {};}
    std::vector<Event> poll()override{return {};}
    bool available(const Guid&)override{return false;}
};
}
std::shared_ptr<Backend> make_backend(){return std::make_shared<Unavailable>();}
}
