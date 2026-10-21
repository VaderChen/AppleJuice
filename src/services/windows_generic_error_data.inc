// Windows system_category／errc 資料契約；不使用宿主 errno 或 strerror。
// 來源：https://github.com/microsoft/STL/blob/main/stl/src/syserror.cpp
// errc 數值：https://github.com/microsoft/STL/blob/main/stl/inc/xerrc.h
struct WindowsErrorConditionData {std::uint32_t system,generic;};
constexpr WindowsErrorConditionData windows_error_conditions[]{
    {0,0}, // ERROR_SUCCESS
    {1,40}, // ERROR_INVALID_FUNCTION
    {2,2}, // ERROR_FILE_NOT_FOUND
    {3,2}, // ERROR_PATH_NOT_FOUND
    {4,24}, // ERROR_TOO_MANY_OPEN_FILES
    {5,13}, // ERROR_ACCESS_DENIED
    {6,22}, // ERROR_INVALID_HANDLE
    {8,12}, // ERROR_NOT_ENOUGH_MEMORY
    {12,13}, // ERROR_INVALID_ACCESS
    {14,12}, // ERROR_OUTOFMEMORY
    {15,19}, // ERROR_INVALID_DRIVE
    {16,13}, // ERROR_CURRENT_DIRECTORY
    {17,18}, // ERROR_NOT_SAME_DEVICE
    {19,13}, // ERROR_WRITE_PROTECT
    {20,19}, // ERROR_BAD_UNIT
    {21,11}, // ERROR_NOT_READY
    {25,5}, // ERROR_SEEK
    {29,5}, // ERROR_WRITE_FAULT
    {30,5}, // ERROR_READ_FAULT
    {32,13}, // ERROR_SHARING_VIOLATION
    {33,39}, // ERROR_LOCK_VIOLATION
    {39,28}, // ERROR_HANDLE_DISK_FULL
    {50,129}, // ERROR_NOT_SUPPORTED
    {53,2}, // ERROR_BAD_NETPATH
    {55,19}, // ERROR_DEV_NOT_EXIST
    {67,2}, // ERROR_BAD_NET_NAME
    {80,17}, // ERROR_FILE_EXISTS
    {82,13}, // ERROR_CANNOT_MAKE
    {87,22}, // ERROR_INVALID_PARAMETER
    {109,32}, // ERROR_BROKEN_PIPE
    {110,5}, // ERROR_OPEN_FAILED
    {111,38}, // ERROR_BUFFER_OVERFLOW
    {112,28}, // ERROR_DISK_FULL
    {121,138}, // ERROR_SEM_TIMEOUT
    {123,2}, // ERROR_INVALID_NAME
    {131,22}, // ERROR_NEGATIVE_SEEK
    {142,16}, // ERROR_BUSY_DRIVE
    {145,41}, // ERROR_DIR_NOT_EMPTY
    {170,16}, // ERROR_BUSY
    {183,17}, // ERROR_ALREADY_EXISTS
    {206,38}, // ERROR_FILENAME_EXCED_RANGE
    {212,39}, // ERROR_LOCKED
    {258,138}, // WAIT_TIMEOUT
    {267,22}, // ERROR_DIRECTORY
    {995,105}, // ERROR_OPERATION_ABORTED
    {998,13}, // ERROR_NOACCESS
    {1011,5}, // ERROR_CANTOPEN
    {1012,5}, // ERROR_CANTREAD
    {1013,5}, // ERROR_CANTWRITE
    {1237,11}, // ERROR_RETRY
    {1460,138}, // ERROR_TIMEOUT
    {2401,16}, // ERROR_OPEN_FILES
    {2404,16}, // ERROR_DEVICE_IN_USE
    {4393,22}, // ERROR_REPARSE_TAG_INVALID
    {10004,4}, // WSAEINTR
    {10009,9}, // WSAEBADF
    {10013,13}, // WSAEACCES
    {10014,14}, // WSAEFAULT
    {10022,22}, // WSAEINVAL
    {10024,24}, // WSAEMFILE
    {10035,140}, // WSAEWOULDBLOCK
    {10036,112}, // WSAEINPROGRESS
    {10037,103}, // WSAEALREADY
    {10038,128}, // WSAENOTSOCK
    {10039,109}, // WSAEDESTADDRREQ
    {10040,115}, // WSAEMSGSIZE
    {10041,136}, // WSAEPROTOTYPE
    {10042,123}, // WSAENOPROTOOPT
    {10043,135}, // WSAEPROTONOSUPPORT
    {10045,130}, // WSAEOPNOTSUPP
    {10047,102}, // WSAEAFNOSUPPORT
    {10048,100}, // WSAEADDRINUSE
    {10049,101}, // WSAEADDRNOTAVAIL
    {10050,116}, // WSAENETDOWN
    {10051,118}, // WSAENETUNREACH
    {10052,117}, // WSAENETRESET
    {10053,106}, // WSAECONNABORTED
    {10054,108}, // WSAECONNRESET
    {10055,119}, // WSAENOBUFS
    {10056,113}, // WSAEISCONN
    {10057,126}, // WSAENOTCONN
    {10060,138}, // WSAETIMEDOUT
    {10061,107}, // WSAECONNREFUSED
    {10063,38}, // WSAENAMETOOLONG
    {10065,110}, // WSAEHOSTUNREACH
};
struct WindowsGenericMessageData {std::uint32_t code;std::string_view message;};
constexpr WindowsGenericMessageData windows_generic_messages[]{
    {0,"success"},
    {1,"operation not permitted"},
    {2,"no such file or directory"},
    {3,"no such process"},
    {4,"interrupted"},
    {5,"io error"},
    {6,"no such device or address"},
    {7,"argument list too long"},
    {8,"executable format error"},
    {9,"bad file descriptor"},
    {10,"no child process"},
    {11,"resource unavailable try again"},
    {12,"not enough memory"},
    {13,"permission denied"},
    {14,"bad address"},
    {16,"device or resource busy"},
    {17,"file exists"},
    {18,"cross device link"},
    {19,"no such device"},
    {20,"not a directory"},
    {21,"is a directory"},
    {22,"invalid argument"},
    {23,"too many files open in system"},
    {24,"too many files open"},
    {25,"inappropriate io control operation"},
    {27,"file too large"},
    {28,"no space on device"},
    {29,"invalid seek"},
    {30,"read only file system"},
    {31,"too many links"},
    {32,"broken pipe"},
    {33,"argument out of domain"},
    {34,"result out of range"},
    {36,"resource deadlock would occur"},
    {38,"filename too long"},
    {39,"no lock available"},
    {40,"function not supported"},
    {41,"directory not empty"},
    {42,"illegal byte sequence"},
    {100,"address in use"},
    {101,"address not available"},
    {102,"address family not supported"},
    {103,"connection already in progress"},
    {104,"bad message"},
    {105,"operation canceled"},
    {106,"connection aborted"},
    {107,"connection refused"},
    {108,"connection reset"},
    {109,"destination address required"},
    {110,"host unreachable"},
    {111,"identifier removed"},
    {112,"operation in progress"},
    {113,"already connected"},
    {114,"too many symbolic link levels"},
    {115,"message size"},
    {116,"network down"},
    {117,"network reset"},
    {118,"network unreachable"},
    {119,"no buffer space"},
    {120,"no message available"},
    {121,"no link"},
    {122,"no message"},
    {123,"no protocol option"},
    {124,"no stream resources"},
    {125,"not a stream"},
    {126,"not connected"},
    {127,"state not recoverable"},
    {128,"not a socket"},
    {129,"not supported"},
    {130,"operation not supported"},
    {132,"value too large"},
    {133,"owner dead"},
    {134,"protocol error"},
    {135,"protocol not supported"},
    {136,"wrong protocol type"},
    {137,"stream timeout"},
    {138,"timed out"},
    {139,"text file busy"},
    {140,"operation would block"},
};
