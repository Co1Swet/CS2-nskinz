#include <Windows.h>
#include <objbase.h>
#include <dia2.h>
#include <diacreate.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
HRESULT CreateNoRegObject(
    const wchar_t* dll_path,
    REFCLSID clsid,
    REFIID iid,
    void** object)
{
    if (dll_path == nullptr || object == nullptr)
    {
        return E_INVALIDARG;
    }

    *object = nullptr;

    const HMODULE module = LoadLibraryW(dll_path);
    if (module == nullptr)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    const auto dll_get_class_object =
        reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(module, "DllGetClassObject"));
    if (dll_get_class_object == nullptr)
    {
        FreeLibrary(module);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    IClassFactory* class_factory = nullptr;
    const HRESULT factory_hr =
        dll_get_class_object(clsid, IID_IClassFactory, reinterpret_cast<void**>(&class_factory));
    if (FAILED(factory_hr) || class_factory == nullptr)
    {
        FreeLibrary(module);
        return FAILED(factory_hr) ? factory_hr : E_NOINTERFACE;
    }

    const HRESULT instance_hr = class_factory->CreateInstance(nullptr, iid, object);
    class_factory->Release();
    return instance_hr;
}

template <typename T>
struct ComPtr
{
    T* ptr = nullptr;

    ~ComPtr()
    {
        if (ptr != nullptr)
        {
            ptr->Release();
        }
    }

    T** put()
    {
        if (ptr != nullptr)
        {
            ptr->Release();
            ptr = nullptr;
        }
        return &ptr;
    }

    T* get() const
    {
        return ptr;
    }

    T* operator->() const
    {
        return ptr;
    }
};

std::wstring BstrToWString(BSTR value)
{
    if (value == nullptr)
    {
        return {};
    }

    const std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

std::wstring GetSymbolName(IDiaSymbol* symbol)
{
    if (symbol == nullptr)
    {
        return L"<null>";
    }

    BSTR name = nullptr;
    if (symbol->get_name(&name) == S_OK && name != nullptr)
    {
        return BstrToWString(name);
    }

    return L"<unnamed>";
}

void PrintLineInfo(IDiaSession* session, DWORD rva)
{
    ComPtr<IDiaEnumLineNumbers> lines;
    if (session->findLinesByRVA(rva, 1, lines.put()) != S_OK || lines.get() == nullptr)
    {
        std::wprintf(L"  line: <none>\n");
        return;
    }

    LONG count = 0;
    lines->get_Count(&count);
    if (count <= 0)
    {
        std::wprintf(L"  line: <none>\n");
        return;
    }

    ComPtr<IDiaLineNumber> line;
    ULONG fetched = 0;
    if (lines->Next(1, line.put(), &fetched) != S_OK || fetched == 0 || line.get() == nullptr)
    {
        std::wprintf(L"  line: <none>\n");
        return;
    }

    DWORD line_number = 0;
    line->get_lineNumber(&line_number);

    ComPtr<IDiaSourceFile> source_file;
    std::wstring file_name = L"<unknown>";
    if (line->get_sourceFile(source_file.put()) == S_OK && source_file.get() != nullptr)
    {
        BSTR file = nullptr;
        if (source_file->get_fileName(&file) == S_OK && file != nullptr)
        {
            file_name = BstrToWString(file);
        }
    }

    std::wprintf(L"  line: %ls:%lu\n", file_name.c_str(), line_number);
}

void PrintInlineFrames(IDiaSession* session, IDiaSymbol* parent, DWORD rva)
{
    if (session == nullptr || parent == nullptr)
    {
        return;
    }

    ComPtr<IDiaEnumSymbols> frames;
    if (session->findInlineFramesByRVA(parent, rva, frames.put()) != S_OK || frames.get() == nullptr)
    {
        return;
    }

    LONG count = 0;
    frames->get_Count(&count);
    if (count <= 0)
    {
        return;
    }

    std::wprintf(L"  inline frames:\n");
    for (;;)
    {
        ComPtr<IDiaSymbol> frame;
        ULONG fetched = 0;
        if (frames->Next(1, frame.put(), &fetched) != S_OK || fetched == 0 || frame.get() == nullptr)
        {
            break;
        }

        std::wstring name = GetSymbolName(frame.get());
        DWORD frame_rva = 0;
        frame->get_relativeVirtualAddress(&frame_rva);
        std::wprintf(L"    - %ls @ RVA 0x%08lX\n", name.c_str(), frame_rva);
    }
}

HRESULT CreateDiaDataSource(IDiaDataSource** data_source)
{
    return CreateNoRegObject(
        L"D:\\Visual studio 2022\\DIA SDK\\bin\\amd64\\msdia140.dll",
        __uuidof(DiaSource),
        __uuidof(IDiaDataSource),
        reinterpret_cast<void**>(data_source));
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::fwprintf(stderr, L"usage: dia_rva_lookup <pdb> <hex-rva> [hex-rva...]\n");
        return 1;
    }

    const HRESULT coinit = CoInitialize(nullptr);
    if (FAILED(coinit))
    {
        std::fwprintf(stderr, L"CoInitialize failed: 0x%08lX\n", coinit);
        return 1;
    }

    int exit_code = 0;
    {
        ComPtr<IDiaDataSource> data_source;
        const HRESULT create_hr = CreateDiaDataSource(data_source.put());
        if (FAILED(create_hr))
        {
            std::fwprintf(stderr, L"CreateDiaDataSource failed: 0x%08lX\n", create_hr);
            CoUninitialize();
            return 1;
        }

        const HRESULT load_hr = data_source->loadDataFromPdb(argv[1]);
        if (FAILED(load_hr))
        {
            std::fwprintf(stderr, L"loadDataFromPdb failed: 0x%08lX\n", load_hr);
            CoUninitialize();
            return 1;
        }

        ComPtr<IDiaSession> session;
        if (FAILED(data_source->openSession(session.put())))
        {
            std::fwprintf(stderr, L"openSession failed\n");
            CoUninitialize();
            return 1;
        }

        for (int index = 2; index < argc; ++index)
        {
            const DWORD rva = static_cast<DWORD>(std::wcstoul(argv[index], nullptr, 16));
            std::wprintf(L"RVA 0x%08lX\n", rva);

            ComPtr<IDiaSymbol> symbol;
            LONG displacement = 0;
            const HRESULT symbol_hr =
                session->findSymbolByRVAEx(rva, SymTagFunction, symbol.put(), &displacement);
            if (FAILED(symbol_hr) || symbol.get() == nullptr)
            {
                std::wprintf(L"  function: <not found>\n");
                PrintLineInfo(session.get(), rva);
                continue;
            }

            const std::wstring name = GetSymbolName(symbol.get());
            DWORD symbol_rva = 0;
            symbol->get_relativeVirtualAddress(&symbol_rva);
            ULONGLONG length = 0;
            symbol->get_length(&length);

            std::wprintf(
                L"  function: %ls @ RVA 0x%08lX +0x%lX (len=0x%llX)\n",
                name.c_str(),
                symbol_rva,
                displacement,
                length);

            PrintLineInfo(session.get(), rva);
            PrintInlineFrames(session.get(), symbol.get(), rva);
        }
    }

    CoUninitialize();
    return exit_code;
}
