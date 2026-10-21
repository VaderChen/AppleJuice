#include "x64arm/native_v8_protocol.hpp"
#include <v8.h>
#include <libplatform/libplatform.h>
#include <map>
#include <memory>
#include <iostream>
#include <algorithm>

namespace nv=x64arm::native_v8;
namespace {
struct Vm {
    std::unique_ptr<v8::ArrayBuffer::Allocator> allocator;
    v8::Isolate* isolate{};
    v8::Global<v8::Context> context;
    unsigned active{};
    ~Vm(){context.Reset();if(isolate)isolate->Dispose();}
};
struct Value {std::uint32_t vm;v8::Global<v8::Value> value;};
class Server;
struct Binding {Server* server;std::uint32_t vm,token;};
class Server {
    std::map<std::uint32_t,std::unique_ptr<Vm>> vms;
    std::map<std::uint32_t,Value> values;
    std::vector<std::unique_ptr<Binding>> bindings;
    std::uint32_t next_vm{1},next_value{1},next_callback{1};
    std::uint32_t keep(std::uint32_t vm,v8::Local<v8::Value> value){
        if(!next_value)throw std::runtime_error("V8 token 已耗盡");
        const auto id=next_value++;values.emplace(id,Value{vm,v8::Global<v8::Value>(vms.at(vm)->isolate,value)});return id;
    }
    v8::Local<v8::Value> get(std::uint32_t vm,std::uint32_t id){
        const auto found=values.find(id);
        if(found==values.end() || found->second.vm!=vm)throw std::runtime_error("V8 handle 不存在或屬於其他 Isolate");
        return found->second.value.Get(vms.at(vm)->isolate);
    }
    static v8::Local<v8::String> string(v8::Isolate* isolate,const std::string& s){
        v8::Local<v8::String> result;
        if(!v8::String::NewFromUtf8(isolate,s.data(),v8::NewStringType::kNormal,static_cast<int>(s.size())).ToLocal(&result))
            throw std::runtime_error("V8 字串配置失敗");
        return result;
    }
    static std::string utf8(v8::Isolate* isolate,v8::Local<v8::Value> value){
        v8::String::Utf8Value s(isolate,value);return *s?std::string(*s,static_cast<std::size_t>(s.length())):std::string{};
    }
    static void callback(const v8::FunctionCallbackInfo<v8::Value>& info){
        auto* binding=static_cast<Binding*>(info.Data().As<v8::External>()->Value());
        try{binding->server->invoke_callback(*binding,info);}
        catch(const std::exception& error){info.GetIsolate()->ThrowException(v8::Exception::Error(string(info.GetIsolate(),error.what())));}
    }
    void invoke_callback(const Binding& binding,const v8::FunctionCallbackInfo<v8::Value>& info){
        nv::Packet request;request.header.kind=nv::Kind::Callback;request.header.sequence=next_callback++;
        request.header.isolate=binding.vm;request.header.handle=binding.token;
        std::vector<std::uint32_t> borrowed;
        struct Cleanup {Server& server;std::vector<std::uint32_t>& handles;~Cleanup(){for(auto id:handles)server.values.erase(id);}} cleanup{*this,borrowed};
        // argv[0] 是 this；其後為 JS 引數。callback 結束即撤銷借用 token，Retain 可另建持久 token。
        borrowed.push_back(keep(binding.vm,info.This()));
        for(int i=0;i<info.Length();++i)borrowed.push_back(keep(binding.vm,info[i]));
        for(auto id:borrowed)nv::append(request.data,id);
        nv::send(STDOUT_FILENO,request);
        for(;;){auto response=nv::receive(STDIN_FILENO);
            if(response.header.kind==nv::Kind::Request){serve(response);continue;} // callback 同步重入同一 Isolate。
            if(response.header.kind!=nv::Kind::CallbackReturn || response.header.sequence!=request.header.sequence)
                throw std::runtime_error("V8 callback 回傳順序不符");
            if(response.header.status!=nv::Status::Ok || !response.header.handle)throw std::runtime_error("客體 V8 callback 回報例外");
            info.GetReturnValue().Set(get(binding.vm,response.header.handle));return;
        }
    }
    nv::Packet execute(const nv::Packet& request){
        auto response=request;response.header.kind=nv::Kind::Response;response.data.clear();
        auto& h=response.header;const auto& r=request.header;
        if(r.op==nv::Op::Version){response.data=nv::bytes(v8::V8::GetVersion());return response;}
        if(r.op==nv::Op::Create){
            if(!next_vm)throw std::runtime_error("V8 Isolate token 已耗盡");
            auto vm=std::make_unique<Vm>();vm->allocator.reset(v8::ArrayBuffer::Allocator::NewDefaultAllocator());
            v8::Isolate::CreateParams params;params.array_buffer_allocator=vm->allocator.get();vm->isolate=v8::Isolate::New(params);
            vm->isolate->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
            {v8::Isolate::Scope scope(vm->isolate);v8::HandleScope handles(vm->isolate);vm->context.Reset(vm->isolate,v8::Context::New(vm->isolate));}
            h.isolate=next_vm++;vms.emplace(h.isolate,std::move(vm));return response;
        }
        const auto it=vms.find(r.isolate);if(it==vms.end())throw std::runtime_error("V8 Isolate 不存在");
        auto& vm=*it->second;
        if(r.op==nv::Op::Dispose){
            if(vm.active){h.status=nv::Status::Busy;return response;}
            for(auto i=values.begin();i!=values.end();)if(i->second.vm==r.isolate)i=values.erase(i);else ++i;
            // 函式物件先隨 Isolate 銷毀，才釋放 External 指向的 binding。
            vms.erase(it);std::erase_if(bindings,[&](const auto& b){return b->vm==r.isolate;});return response;
        }
        ++vm.active;struct Active {Vm& vm;~Active(){--vm.active;}} active{vm};
        auto* isolate=vm.isolate;v8::Isolate::Scope isolate_scope(isolate);v8::HandleScope handles(isolate);
        const auto context=vm.context.Get(isolate);v8::Context::Scope context_scope(context);v8::TryCatch caught(isolate);
        v8::Local<v8::Value> result;bool failed=false;
        switch(r.op){
        case nv::Op::Eval:{v8::Local<v8::Script> script;failed=!v8::Script::Compile(context,string(isolate,nv::text(request))).ToLocal(&script);
            if(!failed)failed=!script->Run(context).ToLocal(&result);break;}
        case nv::Op::Retain:result=get(r.isolate,r.handle);break;
        case nv::Op::Release:(void)get(r.isolate,r.handle);values.erase(r.handle);break;
        case nv::Op::String:{v8::Local<v8::String> s;failed=!get(r.isolate,r.handle)->ToString(context).ToLocal(&s);
            if(!failed)response.data=nv::bytes(utf8(isolate,s));break;}
        case nv::Op::Get:case nv::Op::Set:{const auto object=get(r.isolate,r.handle);if(!object->IsObject())throw std::runtime_error("V8 目標不是物件");
            auto key=string(isolate,nv::text(request));
            if(r.op==nv::Op::Get)failed=!object.As<v8::Object>()->Get(context,key).ToLocal(&result);
            else {auto set=object.As<v8::Object>()->Set(context,key,get(r.isolate,r.aux));failed=set.IsNothing();h.handle=set.FromMaybe(false)?1U:0U;}break;}
        case nv::Op::Call:{const auto fn=get(r.isolate,r.handle);if(!fn->IsFunction())throw std::runtime_error("V8 目標不是函式");
            if(request.data.size()%4 || request.data.size()/4>1024)throw std::runtime_error("V8 呼叫參數無效");
            std::vector<v8::Local<v8::Value>> args;for(std::size_t at=0;at<request.data.size();at+=4)args.push_back(get(r.isolate,nv::word(request.data,at)));
            v8::Local<v8::Value> receiver=r.aux?get(r.isolate,r.aux):v8::Local<v8::Value>(v8::Undefined(isolate));
            failed=!fn.As<v8::Function>()->Call(context,receiver,static_cast<int>(args.size()),args.data()).ToLocal(&result);break;}
        case nv::Op::Function:{auto binding=std::make_unique<Binding>(Binding{this,r.isolate,r.aux});
            v8::Local<v8::Function> function;failed=!v8::Function::New(context,callback,v8::External::New(isolate,binding.get())).ToLocal(&function);
            if(!failed){result=function;bindings.push_back(std::move(binding));}break;}
        case nv::Op::Equal:h.handle=get(r.isolate,r.handle)->StrictEquals(get(r.isolate,r.aux))?1U:0U;break;
        case nv::Op::Collect:isolate->LowMemoryNotification();break;
        case nv::Op::Microtasks:isolate->PerformMicrotaskCheckpoint();break;
        case nv::Op::ReadBuffer:case nv::Op::WriteBuffer:{const auto value=get(r.isolate,r.handle);v8::Local<v8::ArrayBuffer> buffer;std::size_t start=0,size=0;
            if(value->IsArrayBuffer()){buffer=value.As<v8::ArrayBuffer>();size=buffer->ByteLength();}
            else if(value->IsArrayBufferView()){const auto view=value.As<v8::ArrayBufferView>();buffer=view->Buffer();start=view->ByteOffset();size=view->ByteLength();}
            else throw std::runtime_error("V8 目標不是 ArrayBuffer／View");
            if(buffer->WasDetached())throw std::runtime_error("V8 backing store 已分離");
            const auto backing=buffer->GetBackingStore();if(start>backing->ByteLength() || size>backing->ByteLength()-start)throw std::runtime_error("V8 view 範圍無效");
            if(r.aux>size)throw std::runtime_error("V8 buffer offset 超過範圍");
            const auto count=r.op==nv::Op::ReadBuffer?size-r.aux:request.data.size();
            if(count>size-r.aux || count>nv::limit)throw std::runtime_error("V8 buffer 長度超過範圍");
            if(count){auto* data=static_cast<std::uint8_t*>(backing->Data())+start+r.aux;
                if(r.op==nv::Op::ReadBuffer)response.data.assign(data,data+count);else std::copy(request.data.begin(),request.data.end(),data);}break;}
        default:throw std::runtime_error("V8 模組操作未定義");
        }
        if(failed || caught.HasCaught()){h.status=nv::Status::Exception;response.data=nv::bytes(utf8(isolate,caught.Exception()));h.handle=0;}
        else if(!result.IsEmpty())h.handle=keep(r.isolate,result);
        return response;
    }
public:
    ~Server(){values.clear();vms.clear();bindings.clear();}
    void serve(const nv::Packet& request){
        if(request.header.kind!=nv::Kind::Request)throw std::runtime_error("V8 預期模組請求");
        nv::Packet response;
        try{response=execute(request);}catch(const std::exception& error){response.header=request.header;response.header.kind=nv::Kind::Response;
            response.header.status=nv::Status::Invalid;response.header.handle=0;response.data=nv::bytes(error.what());}
        nv::send(STDOUT_FILENO,response);
    }
    void run(){for(;;)serve(nv::receive(STDIN_FILENO));}
};
}
int main(int argc,char** argv){
    (void)argc;v8::V8::InitializeICUDefaultLocation(argv[0]);v8::V8::InitializeExternalStartupData(argv[0]);
    auto platform=v8::platform::NewDefaultPlatform(2);v8::V8::InitializePlatform(platform.get());
    if(!v8::V8::Initialize())return 1;
    try{Server server;server.run();}catch(const std::exception& e){if(!std::cin.eof())std::cerr<<e.what()<<'\n';}
    v8::V8::Dispose();v8::V8::DisposePlatform();return 0;
}
