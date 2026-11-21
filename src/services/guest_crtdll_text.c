typedef __UINTPTR_TYPE__ Address;
typedef unsigned char Byte;

// CRTDLL 的窄字元游標依位元組移動，與 CRT 的 DBCS 字串游標各自維持原 ABI。
__declspec(dllexport) Address _strdec(Address begin,Address current){
    (void)begin;
    return current-1;
}

__declspec(dllexport) Address _strinc(Address current){
    return current+1;
}

__declspec(dllexport) Address _strninc(Address current,Address count){
    return current+count;
}

__declspec(dllexport) unsigned _strnextc(const Byte* current){
    return *current;
}

__declspec(dllexport) Address _strncnt(const Byte* text,Address maximum){
    Address count=0;
    while(count<maximum && text[count])++count;
    return count;
}

__declspec(dllexport) const Byte* _strspnp(const Byte* text,const Byte* accept){
    for(;*text;++text){
        const Byte* candidate=accept;
        while(*candidate && *candidate!=*text)++candidate;
        if(!*candidate)return text;
    }
    return 0;
}
