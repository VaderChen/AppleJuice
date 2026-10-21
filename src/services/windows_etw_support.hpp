#pragma once
#include "x64arm/etw_guest.hpp"

namespace x64arm::etw {
inline std::uint32_t win_error(Status status){
    switch(status){case Status::Success:return 0;case Status::InvalidParameter:return 87;case Status::InvalidHandle:return 6;
    case Status::AlreadyExists:return 183;case Status::NotFound:return 4201;case Status::NoResources:return 8;case Status::Unsupported:return 50;
    case Status::EventTooLarge:return 534;case Status::BufferTooSmall:return 234;case Status::QueueFull:return 8;case Status::FileFull:return 1502;case Status::RealtimeFull:return 0xc0000188U;
    case Status::Busy:return 170;case Status::ClosePending:return 7007;case Status::InvalidTime:return 1901;}
    return 87;
}
}
