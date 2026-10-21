#pragma once
namespace x64arm {
struct WindowsServiceDescriptor;
struct CrtCharacterOperation;
const CrtCharacterOperation* crt_character_operation(const WindowsServiceDescriptor&);
bool crt_character_requested() noexcept;
}
