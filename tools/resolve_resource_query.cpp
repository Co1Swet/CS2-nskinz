#include <Windows.h>
#include <Psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

using CreateInterfaceFn = void* (*)(const char*, int*);
using QueryFn = std::uint64_t(__fastcall*)(void*, const char*, std::uint64_t, std::uint64_t);

static bool SafeRead(const void* src, void* dst, size_t n) {
    __try { std::memcpy(dst, src, n); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void* Follow(void* p) {
    auto* cur = static_cast<unsigned char*>(p);
    for(int d=0; d<4 && cur; ++d) {
        __try {
            if(cur[0]==0xE9){ auto rel=*reinterpret_cast<int*>(cur+1); cur=cur+5+rel; continue; }
            if(cur[0]==0xEB){ auto rel=*reinterpret_cast<signed char*>(cur+1); cur=cur+2+rel; continue; }
            if(cur[0]==0xFF && cur[1]==0x25){ auto rel=*reinterpret_cast<int*>(cur+2); void* out=nullptr; auto** tgt=reinterpret_cast<void***>(cur+6+rel); return (tgt && SafeRead(tgt,&out,sizeof(out))) ? out : nullptr; }
            if(cur[0]==0x48 && cur[1]==0xB8 && cur[10]==0xFF && cur[11]==0xE0){ void* out=nullptr; std::memcpy(&out,cur+2,sizeof(out)); cur=(unsigned char*)out; continue; }
        } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        break;
    }
    return cur;
}

static bool Probe(void* fn, void* iface, const char* path, std::uint64_t& out) {
    out=0;
    __try {
        auto a = reinterpret_cast<QueryFn>(fn)(iface,path,0,0);
        auto b = reinterpret_cast<QueryFn>(fn)(iface,path,0,0);
        if(a!=0 && a==b){ out=a; return true; }
        return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int main() {
    auto mod = LoadLibraryW(L"E:\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\game\\bin\\win64\\resourcesystem.dll");
    if(!mod){ std::printf("load failed\n"); return 1; }
    auto create = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(mod, "CreateInterface"));
    if(!create){ std::printf("CreateInterface missing\n"); return 1; }

    const char* versions[] = {"ResourceSystem013", "ResourceSystem014", "ResourceHandleUtils001", "ResourceHandleUtils002"};
    const char* paths[] = {
        "models/weapons/w_knife_butterfly.vmdl_c",
        "models/weapons/w_knife_karam.vmdl_c",
        "models/weapons/v_knife_butterfly.vmdl_c",
        "models/weapons/v_knife_karam.vmdl_c"
    };
    for(const char* ver : versions) {
        int rc = 0;
        void* iface = create(ver, &rc);
        std::printf("iface %s => %p rc=%d\n", ver, iface, rc);
        if(!iface) continue;
        void** vtbl = *reinterpret_cast<void***>(iface);
        for(size_t slot=0; slot<=64; ++slot) {
            void* fn = vtbl[slot];
            void* body = Follow(fn);
            if(!body) body = fn;
            std::uint64_t h1=0,h2=0;
            if(Probe(body, iface, paths[0], h1) && Probe(body, iface, paths[1], h2) && h1 != h2) {
                std::printf("candidate ver=%s slot=%zu fn=%p body=%p h1=%llx h2=%llx\n", ver, slot, fn, body, (unsigned long long)h1, (unsigned long long)h2);
                unsigned char bytes[32]{};
                if(SafeRead(body, bytes, sizeof(bytes))) {
                    for(unsigned char b : bytes) std::printf("%02X ", b);
                    std::printf("\n");
                }
            }
        }
    }
    return 0;
}

