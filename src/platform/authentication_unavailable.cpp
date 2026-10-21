#include "x64arm/native_authentication.hpp"
namespace x64arm::authentication {
namespace {class Unavailable final:public Backend {
    Result verify(std::u16string_view,std::u16string_view,std::u16string_view)override{return {0xc00000bbU,0,{}};}
};}
std::shared_ptr<Backend> native_backend(){return std::make_shared<Unavailable>();}
}
