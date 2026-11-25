#pragma once
/* CRT 兩種字元寬度共用 errno 字串及有界格式化。位址和容量以呼叫端的指標寬度計算。 */
typedef __UINTPTR_TYPE__ CrtErrorAddress;
static inline const char* crt_errno_message(unsigned code) {
    switch(code) {
    case 0:return "Error 0";case 1:return "Operation not permitted";case 2:return "No such file or directory";
    case 3:return "No such process";case 4:return "Interrupted function call";case 5:return "Input/output error";
    case 6:return "No such device or address";case 7:return "Arg list too long";
    case 8:return "Exec format error";case 9:return "Bad file number";case 10:return "No child processes";
    case 11:return "Resource temporarily unavailable";case 12:return "Not enough core";case 13:return "Permission denied";
    case 14:return "Bad address";case 16:return "Device or resource busy";case 17:return "File exists";case 18:return "Cross-device link";
    case 19:return "No such device";case 20:return "Not a directory";case 21:return "Is a directory";case 22:return "Invalid argument";
    case 23:return "Too many open files in system";case 24:return "Too many open files";case 25:return "Inappropriate I/O control operation";
    case 27:return "File too large";case 28:return "No space left on device";case 29:return "Invalid seek";case 30:return "Read-only file system";
    case 31:return "Too many links";case 32:return "Broken pipe";case 33:return "Math argument";case 34:return "Result too large";
    case 36:return "Resource deadlock would occur";case 38:return "Filename too long";case 39:return "No locks available";
    case 40:return "Function not implemented";case 41:return "Directory not empty";case 42:return "Illegal byte sequence";
    default:return "Unknown error";
    }
}

typedef struct {
    void* context;
    CrtErrorAddress (*length)(void*, CrtErrorAddress, CrtErrorAddress, unsigned);
    void (*copy)(void*, CrtErrorAddress, CrtErrorAddress, CrtErrorAddress);
    /* 寫入 count 個 ASCII 字元並補上一個目的字寬的 NUL。 */
    void (*ascii)(void*, CrtErrorAddress, unsigned, const char*, CrtErrorAddress);
} CrtErrorWriter;

static inline void crt_format_error(const CrtErrorWriter* writer, CrtErrorAddress destination,
                                    CrtErrorAddress capacity, unsigned width,
                                    CrtErrorAddress prefix, unsigned code) {
    /* 前綴要容得下冒號、空格、至少五字元的說明與結尾。 */
    if (prefix && capacity > 7) {
        writer->ascii(writer->context, destination, width, "", 0);
        const CrtErrorAddress limit = capacity - 7;
        const CrtErrorAddress length = writer->length(writer->context, prefix, limit, width);
        if (length && length < limit) {
            writer->copy(writer->context, destination, prefix, length * width);
            writer->ascii(writer->context, destination + length * width, width, ": ", 2);
            destination += (length + 2) * width;
            capacity -= length + 2;
        }
    }
    const char* message = crt_errno_message(code);
    CrtErrorAddress length = 0;
    while (length < capacity - 1 && message[length]) ++length;
    writer->ascii(writer->context, destination, width, message, length);
}
