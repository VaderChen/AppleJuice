#include <windows.h>
#include "x64arm/native_v8_guest.h"
__declspec(dllimport) uint32_t __cdecl X64ArmV8Dispatch(X64ArmV8Request*);
static char output[1024];static unsigned checks,callback_count;static uint32_t borrowed,retained;
static unsigned length(const char* s){unsigned n=0;while(s[n])++n;return n;}
static void say(const char* s){DWORD written;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,length(s),&written,0);}
static void number(unsigned value){char digits[16];unsigned n=0;do{digits[n++]=(char)('0'+value%10);value/=10;}while(value);while(n){DWORD w;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),&digits[--n],1,&w,0);}}
static void need(int ok){++checks;if(!ok){say("V8 FAIL check=");number(checks);say("\n");ExitProcess(checks);}}
static int same(const char* a,const char* b){unsigned i=0;while(a[i]&&b[i]&&a[i]==b[i])++i;return a[i]==b[i];}
static int contains(const char* a,const char* b){unsigned i,j;for(i=0;a[i];++i){for(j=0;b[j]&&a[i+j]==b[j];++j){}if(!b[j])return 1;}return 0;}
static X64ArmV8Request request(uint32_t op,uint32_t iso,uint32_t handle,uint32_t aux,const void* input,unsigned size){
    X64ArmV8Request r={0};r.op=op;r.isolate=iso;r.handle=handle;r.aux=aux;r.input=(uint64_t)(uintptr_t)input;r.input_size=size;
    r.output=(uint64_t)(uintptr_t)output;r.output_capacity=sizeof(output)-1;return r;
}
static uint32_t run(X64ArmV8Request* r){uint32_t s=X64ArmV8Dispatch(r);need(s==r->status);if(r->output_size<sizeof(output))output[r->output_size]=0;return s;}
static uint32_t eval(uint32_t iso,const char* js){X64ArmV8Request r=request(XV8_EVAL,iso,0,0,js,length(js));need(run(&r)==0);return r.result;}
static void text_is(uint32_t iso,uint32_t value,const char* expected){X64ArmV8Request r=request(XV8_STRING,iso,value,0,0,0);need(run(&r)==0);need(same(output,expected));}
static uint32_t property(uint32_t iso,uint32_t object,const char* name){X64ArmV8Request r=request(XV8_GET,iso,object,0,name,length(name));need(run(&r)==0);return r.result;}
static void release(uint32_t iso,uint32_t handle){X64ArmV8Request r=request(XV8_RELEASE,iso,handle,0,0,0);need(run(&r)==0);}
static DWORD WINAPI callback(DWORD iso,DWORD argc,const DWORD* argv,UINT_PTR user){
    X64ArmV8Request r;need(argc==2 && user==0xcafe);const unsigned ordinal=++callback_count;borrowed=argv[1];
    if(ordinal==1){r=request(XV8_RETAIN,iso,borrowed,0,0,0);need(run(&r)==0);retained=r.result;}
    text_is(iso,argv[1],ordinal==1?"21":"7");
    r=request(XV8_DISPOSE,iso,0,0,0,0);need(run(&r)==XV8_BUSY);
    /* native JS → guest callback → native JS function → guest callback，保留同步重入棧。 */
    if(ordinal==1){uint32_t function=eval(iso,"()=>host(7)");r=request(XV8_CALL,iso,function,0,0,0);need(run(&r)==0);text_is(iso,r.result,"14");}
    return eval(iso,ordinal==1?"42":"14");
}
static DWORD WINAPI throwing_callback(DWORD iso,DWORD argc,const DWORD* argv,UINT_PTR user){(void)iso;(void)argc;(void)argv;(void)user;return 0;}
static DWORD WINAPI foreign_thread(LPVOID parameter){uint32_t iso=(uint32_t)(uintptr_t)parameter;X64ArmV8Request r=request(XV8_COLLECT,iso,0,0,0,0);return X64ArmV8Dispatch(&r);}
void entry(void){
    X64ArmV8Request r=request(XV8_VERSION,0,0,0,0,0);need(sizeof(r)==72);need(run(&r)==0);say("native_v8_version=");say(output);say("\n");
    r=request(XV8_CREATE,0,0,0,0,0);need(run(&r)==0);uint32_t iso=r.result;
    r=request(XV8_CREATE,0,0,0,0,0);need(run(&r)==0);uint32_t other=r.result;
    uint32_t object=eval(iso,"globalThis.obj={n:41}; obj"),value=property(iso,object,"n");text_is(iso,value,"41");
    uint32_t forty_two=eval(iso,"42");r=request(XV8_SET,iso,object,forty_two,"n",1);need(run(&r)==0);text_is(iso,property(iso,object,"n"),"42");
    text_is(iso,eval(iso,"'繁體中文😀'.normalize('NFC')"),"繁體中文😀");
    uint32_t method=eval(iso,"(function(x){return this.n+x})"),argument=eval(iso,"8");
    r=request(XV8_CALL,iso,method,object,&argument,sizeof(argument));need(run(&r)==0);text_is(iso,r.result,"50");
    uint32_t duplicate=eval(iso,"obj");r=request(XV8_EQUAL,iso,object,duplicate,0,0);need(run(&r)==0 && r.result==1);
    r=request(XV8_COLLECT,iso,0,0,0,0);need(run(&r)==0);text_is(iso,property(iso,object,"n"),"42");
    r=request(XV8_STRING,other,object,0,0,0);need(run(&r)==XV8_INVALID);
    release(iso,value);r=request(XV8_STRING,iso,value,0,0,0);need(run(&r)==XV8_INVALID);
    const char* bad="throw new Error('module-exception')";r=request(XV8_EVAL,iso,0,0,bad,length(bad));need(run(&r)==XV8_EXCEPTION && contains(output,"module-exception"));
    const char* syntax="function (";r=request(XV8_EVAL,iso,0,0,syntax,length(syntax));need(run(&r)==XV8_EXCEPTION && contains(output,"SyntaxError"));
    text_is(iso,eval(iso,"(()=>{function f(n){return n+1};let x=0;for(let i=0;i<100000;i++)x=f(x);return x})()"),"100000");
    r=request(XV8_FUNCTION,iso,0,0,0,0);r.callback=(uint64_t)(uintptr_t)callback;r.userdata=0xcafe;need(run(&r)==0);uint32_t fn=r.result;
    uint32_t global=eval(iso,"globalThis");r=request(XV8_SET,iso,global,fn,"host",4);need(run(&r)==0);
    /* 巢狀回呼返回後，外層仍回傳自己的結果。 */
    uint32_t result=eval(iso,"host(21)");need(callback_count==2);text_is(iso,result,"42");text_is(iso,retained,"21");
    r=request(XV8_STRING,iso,borrowed,0,0,0);need(run(&r)==XV8_INVALID);
    r=request(XV8_FUNCTION,iso,0,0,0,0);r.callback=(uint64_t)(uintptr_t)throwing_callback;need(run(&r)==0);fn=r.result;
    r=request(XV8_SET,iso,global,fn,"fail",4);need(run(&r)==0);bad="fail()";r=request(XV8_EVAL,iso,0,0,bad,length(bad));need(run(&r)==XV8_EXCEPTION);
    text_is(iso,eval(iso,"try{fail()}catch(e){'caught'}"),"caught");
    text_is(iso,eval(iso,"globalThis.tick=0;Promise.resolve().then(()=>tick=9);tick"),"0");
    r=request(XV8_MICROTASKS,iso,0,0,0,0);need(run(&r)==0);text_is(iso,eval(iso,"tick"),"9");
    uint32_t buffer=eval(iso,"globalThis.view=new Uint8Array([1,2,3,4]).subarray(1,3);view");
    r=request(XV8_READ_BUFFER,iso,buffer,0,0,0);need(run(&r)==0 && r.output_size==2 && output[0]==2 && output[1]==3);
    unsigned char bytes[2]={8,9};r=request(XV8_WRITE_BUFFER,iso,buffer,0,bytes,2);need(run(&r)==0);text_is(iso,eval(iso,"Array.from(view).join(',')"),"8,9");
    r=request(XV8_COLLECT,iso,0,0,0,0);need(run(&r)==0);r=request(XV8_READ_BUFFER,iso,buffer,0,0,0);need(run(&r)==0 && output[0]==8 && output[1]==9);
    r=request(XV8_WRITE_BUFFER,iso,buffer,1,bytes,2);need(run(&r)==XV8_INVALID);
    uint32_t detached=eval(iso,"globalThis.detached=new Uint8Array([6]);detached.buffer.transfer();detached");
    r=request(XV8_READ_BUFFER,iso,detached,0,0,0);need(run(&r)==XV8_INVALID);
    r=request(0xffff,iso,0,0,0,0);need(run(&r)==XV8_INVALID);
    text_is(iso,eval(iso,"6*7"),"42");
    r=request(XV8_STRING,iso,forty_two,0,0,0);r.output_capacity=1;need(run(&r)==XV8_CAPACITY && r.output_size==2);
    HANDLE thread=CreateThread(0,0,foreign_thread,(LPVOID)(uintptr_t)iso,0,0);need(thread!=0);need(WaitForSingleObject(thread,INFINITE)==WAIT_OBJECT_0);DWORD code;need(GetExitCodeThread(thread,&code) && code==XV8_THREAD);CloseHandle(thread);
    HMODULE module=LoadLibraryA("x64arm_v8.dll");need(module!=0);need(GetProcAddress(module,"X64ArmV8Dispatch")!=0);need(!GetProcAddress(module,"ScriptRun"));FreeLibrary(module);
    r=request(XV8_DISPOSE,iso,0,0,0,0);need(run(&r)==0);r=request(XV8_STRING,iso,object,0,0,0);need(run(&r)==XV8_INVALID);
    r=request(XV8_DISPOSE,other,0,0,0,0);need(run(&r)==0);
    r=request(XV8_CREATE,0,0,0,0,0);need(run(&r)==0 && r.result!=iso && r.result!=other);iso=r.result;text_is(iso,eval(iso,"6*7"),"42");
    r=request(XV8_DISPOSE,iso,0,0,0,0);need(run(&r)==0);say("checks=");number(checks);say("\nV8 whole-module guest ABI lifecycle GC exception nested-callback buffer microtask thread PASS\n");ExitProcess(0);
}
