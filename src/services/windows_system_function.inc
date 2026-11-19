// advapi32／cryptbase 的 SystemFunction0xx：LSA 秘密與 NTLM 使用的 DES／MD4／RC4 輔助函式。
// 演算法依 MS-NLMP（LM／NT 雜湊與回應）、MS-SAMR 2.2.11.1（56 位元金鑰展開與 16 位元組區塊加密）及
// MS-LSAD 5.1.2（秘密的長度／版本標頭與循環金鑰）實作；DES 走共用 CryptoCipher（OpenSSL legacy provider），
// MD4 走 message_digest.hpp，RC4 為標準 KSA／PRGA。全部在宿主端直接讀寫客體緩衝，不離開服務呼叫。
namespace {
namespace system_function {
// STATUS_INVALID_PARAMETER 沿用檔案外層的 invalid_parameter。
constexpr std::uint32_t success=0,unsuccessful=0xc0000001U,invalid_parameter_2=0xc00000f0U,
    buffer_too_small=0xc0000023U,unknown_revision=0xc0000058U;
using Block=std::array<std::uint8_t,8>;
constexpr Block lm_magic{'K','G','S','!','@','#','$','%'};

// 7 位元組（56 位元）金鑰每 7 位元切成一段放進 8 位元組的高 7 位元；最低位元是同位位元，DES 不使用。
Block expand_key(const std::uint8_t* k){
    Block out{static_cast<std::uint8_t>(k[0]>>1),static_cast<std::uint8_t>(((k[0]&0x01)<<6)|(k[1]>>2)),
        static_cast<std::uint8_t>(((k[1]&0x03)<<5)|(k[2]>>3)),static_cast<std::uint8_t>(((k[2]&0x07)<<4)|(k[3]>>4)),
        static_cast<std::uint8_t>(((k[3]&0x0f)<<3)|(k[4]>>5)),static_cast<std::uint8_t>(((k[4]&0x1f)<<2)|(k[5]>>6)),
        static_cast<std::uint8_t>(((k[5]&0x3f)<<1)|(k[6]>>7)),static_cast<std::uint8_t>(k[6]&0x7f)};
    for(auto& value:out)value=static_cast<std::uint8_t>(value<<1);
    return out;
}
Block des(const std::uint8_t* key7,const std::uint8_t* input,bool encrypt){
    const auto key=expand_key(key7);CryptoCipher cipher("DES-ECB",key,{},0,encrypt);
    const auto output=cipher.update(std::span<const std::uint8_t>(input,8));
    Block block{};std::copy_n(output.begin(),8,block.begin());return block;
}
// 16 位元組資料分兩個 DES 區塊，分別用金鑰的前 7 與次 7 位元組。
std::array<std::uint8_t,16> des_pair(const std::uint8_t* input,const std::uint8_t* key14,bool encrypt){
    std::array<std::uint8_t,16> output{};const auto first=des(key14,input,encrypt),second=des(key14+7,input+8,encrypt);
    std::copy(first.begin(),first.end(),output.begin());std::copy(second.begin(),second.end(),output.begin()+8);return output;
}
// MS-LSAD 5.1.2：每個 8 位元組區塊用金鑰中下一段 7 位元組；剩餘不足 7 位元組時改從（長度 − 位置）起算。
// 少於 7 位元組的金鑰補 0 後固定使用。
std::vector<std::uint8_t> lsa_blocks(std::span<const std::uint8_t> input,std::vector<std::uint8_t> key,bool encrypt){
    if(key.size()<7)key.resize(7,0);
    std::vector<std::uint8_t> output(input.size());std::size_t k=0;
    for(std::size_t i=0;i+8<=input.size();i+=8,k+=7){
        if(k+7>key.size())k=key.size()-k;
        if(k+7>key.size())k=0;
        const auto block=des(key.data()+k,input.data()+i,encrypt);std::copy(block.begin(),block.end(),output.begin()+static_cast<std::ptrdiff_t>(i));
    }
    return output;
}
void rc4(std::span<std::uint8_t> data,std::span<const std::uint8_t> key){
    std::array<std::uint8_t,256> s{};for(unsigned i=0;i<256;++i)s[i]=static_cast<std::uint8_t>(i);
    for(unsigned i=0,j=0;i<256;++i){j=(j+s[i]+key[i%key.size()])&0xffU;std::swap(s[i],s[j]);}
    unsigned i=0,j=0;
    for(auto& value:data){i=(i+1)&0xffU;j=(j+s[i])&0xffU;std::swap(s[i],s[j]);value^=s[(s[i]+s[j])&0xffU];}
}
struct Counted{std::uint32_t length,maximum;std::uint64_t buffer;};
constexpr std::size_t output_limit=16U*1024U*1024U;
}
std::uint32_t system_function_call(GuestMemory& memory,const WindowsCall& arg,std::string_view symbol,bool x86){
    using namespace system_function;
    const auto bytes=[&](std::uint64_t pointer,std::size_t count){
        if(count>output_limit)throw HostCallFault("SystemFunction 資料超過 16 MiB 上限");
        std::vector<std::uint8_t> value(count);if(count)memory.read(GuestAddress{pointer},value);return value;
    };
    const auto put=[&](std::uint64_t pointer,std::span<const std::uint8_t> value){memory.write(GuestAddress{pointer},value);};
    // struct ustring {DWORD Length; DWORD MaximumLength; PVOID Buffer;}：兩種架構的 Buffer 都在偏移 8。
    const auto counted=[&](std::uint64_t at){
        return Counted{static_cast<std::uint32_t>(memory.load(GuestAddress{at},32)),static_cast<std::uint32_t>(memory.load(GuestAddress{at+4},32)),memory.load(GuestAddress{at+8},x86?32:64)};
    };
    const auto number=std::stoi(std::string(symbol.substr(14)));
    switch(number){
    case 1:case 2:{// 單一 DES 區塊：(資料, 7 位元組金鑰, 輸出)。
        if(!arg(0) || !arg(2))return unsuccessful;
        const auto key=bytes(arg(1),7),data=bytes(arg(0),8);put(arg(2),des(key.data(),data.data(),number==1));return success;
    }
    case 3:{// LM 半段：以 7 位元組金鑰加密固定字串 "KGS!@#$%"。
        if(!arg(0) || !arg(1))return unsuccessful;
        const auto key=bytes(arg(0),7);put(arg(1),des(key.data(),lm_magic.data(),true));return success;
    }
    case 4:{// LSA 秘密加密：標頭（明文長度、版本 1）＋補 0 到 8 的倍數，再以循環金鑰逐塊加密。
        const auto input=counted(arg(0)),key=counted(arg(1)),output=counted(arg(2));
        if(!key.length)return invalid_parameter_2;
        const std::uint64_t padded=(std::uint64_t{input.length}+7)&~std::uint64_t{7};
        if(padded+8>output_limit)return invalid_parameter;
        if(output.maximum<padded+8){memory.store(GuestAddress{arg(2)},32,padded+8);return buffer_too_small;}
        std::vector<std::uint8_t> plain(static_cast<std::size_t>(padded+8));
        for(unsigned i=0;i<4;++i){plain[i]=static_cast<std::uint8_t>(input.length>>(8*i));plain[4+i]=i==0?1:0;}
        const auto data=bytes(input.buffer,input.length);std::copy(data.begin(),data.end(),plain.begin()+8);
        put(output.buffer,lsa_blocks(plain,bytes(key.buffer,key.length),true));
        memory.store(GuestAddress{arg(2)},32,padded+8);return success;
    }
    case 5:{// LSA 秘密解密：先解出標頭確認版本與長度，再解整段並複製明文。
        const auto input=counted(arg(0)),key=counted(arg(1)),output=counted(arg(2));
        if(!key.length)return invalid_parameter_2;
        if(input.length<8)return invalid_parameter;
        const auto cipher=bytes(input.buffer,input.length&~7U);const auto plain=lsa_blocks(cipher,bytes(key.buffer,key.length),false);
        const auto word=[&](std::size_t at){return std::uint32_t{plain[at]}|(std::uint32_t{plain[at+1]}<<8)|(std::uint32_t{plain[at+2]}<<16)|(std::uint32_t{plain[at+3]}<<24);};
        const auto length=word(0);
        const auto version=word(4);
        if(version!=1)return unknown_revision;
        if(length>output.maximum){memory.store(GuestAddress{arg(2)},32,length);return buffer_too_small;}
        if(std::uint64_t{length}+8>plain.size())return invalid_parameter;
        put(output.buffer,std::span<const std::uint8_t>(plain.data()+8,length));memory.store(GuestAddress{arg(2)},32,length);return success;
    }
    case 6:{// LM 雜湊：密碼轉大寫、截到 14 位元組補 0，兩半各加密 "KGS!@#$%"。
        if(!arg(0) || !arg(1))return unsuccessful;
        std::array<std::uint8_t,14> password{};
        for(unsigned i=0;i<14;++i){
            const auto c=static_cast<std::uint8_t>(memory.load(GuestAddress{arg(0)+i},8));if(!c)break;
            password[i]=c>='a' && c<='z'?static_cast<std::uint8_t>(c-32):c;
        }
        const auto first=des(password.data(),lm_magic.data(),true),second=des(password.data()+7,lm_magic.data(),true);
        put(arg(1),first);put(arg(1)+8,second);return success;
    }
    case 7:{// NT 雜湊：UNICODE_STRING 內容（UTF-16LE 位元組）的 MD4。
        if(!arg(0) || !arg(1))return unsuccessful;
        const auto length=static_cast<std::uint16_t>(memory.load(GuestAddress{arg(0)},16));
        const auto buffer=memory.load(GuestAddress{arg(0)+(x86?4U:8U)},x86?32:64);
        const auto text=bytes(buffer,length);put(arg(1),digest::md4(text.data(),text.size()));return success;
    }
    case 8:case 9:{// LM／NTLM 回應：16 位元組雜湊補 0 到 21 位元組，切三段 7 位元組各加密 8 位元組挑戰。
        if(!arg(0) || !arg(2))return unsuccessful;
        const auto challenge=bytes(arg(0),8);auto key=bytes(arg(1),16);key.resize(21,0);
        for(unsigned i=0;i<3;++i)put(arg(2)+8*i,des(key.data()+7*i,challenge.data(),true));
        return success;
    }
    case 10:case 11:{// (保留, 16 位元組資料, 輸出)：資料的 MD4。
        if(!arg(1) || !arg(2))return unsuccessful;
        const auto data=bytes(arg(1),16);put(arg(2),digest::md4(data.data(),data.size()));return success;
    }
    case 24:case 25:case 26:case 27:{// 金鑰為客體 DWORD 索引重複四次組成的 16 位元組。
        if(!arg(0) || !arg(1) || !arg(2))return unsuccessful;
        const auto index=bytes(arg(1),4);std::array<std::uint8_t,16> key{};for(unsigned i=0;i<16;++i)key[i]=index[i%4];
        const auto data=bytes(arg(0),16);put(arg(2),des_pair(data.data(),key.data(),number%2==0));return success;
    }
    case 30:case 31:{// 16 位元組比較，相同回傳 TRUE。
        const auto left=bytes(arg(0),16),right=bytes(arg(1),16);return left==right?1U:0U;
    }
    case 32:{// RC4 就地加解密：(資料 ustring, 金鑰 ustring)。
        const auto data=counted(arg(0)),key=counted(arg(1));
        if(!key.length)return invalid_parameter_2;
        if(data.length){auto buffer=bytes(data.buffer,data.length);rc4(buffer,bytes(key.buffer,key.length));put(data.buffer,buffer);}return success;
    }
    default:break;
    }
    if(number>=12 && number<=23){// 偶數加密、奇數解密：(16 位元組資料, 16 位元組金鑰, 輸出)。
        if(!arg(0) || !arg(1) || !arg(2))return unsuccessful;
        const auto data=bytes(arg(0),16),key=bytes(arg(1),14);put(arg(2),des_pair(data.data(),key.data(),number%2==0));return success;
    }
    return unsuccessful;
}
}
