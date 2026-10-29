// SPDX-License-Identifier: Apache-2.0
// Point d'entrée de la DLL APO : définition des CLSID gelés, fabrique de classe COM,
// exports (DllGetClassObject, DllCanUnloadNow, DllRegisterServer, DllUnregisterServer)
// et verrou de module.
//
// DllRegisterServer écrit COM (HKCR\CLSID\{guid}\InprocServer32,
// ThreadingModel=Both) et les métadonnées AudioEngine\AudioProcessingObjects.
// Le RATTACHEMENT à un endpoint (clés FxProperties) est une étape
// de déploiement SÉPARÉE et réversible, jamais faite ici : enregistrer la DLL ne modifie
// aucun endpoint ni le son.
#include "xonar_apo.hpp"

#include <windows.h>

#include <atomic>
#include <new>
#include <string>

namespace xonar::apo {

// CLSID gelés par E0-02 (contracts/manifest.json ; miroir de contracts.hpp).
extern "C" const CLSID CLSID_XonarSfxApo = {
    0xC243A8CB, 0xD535, 0x4BA0, {0xBC, 0xC5, 0xCC, 0x43, 0x38, 0x83, 0x3E, 0x7A}};
extern "C" const CLSID CLSID_XonarEfxApo = {
    0xD2FBC737, 0x00C3, 0x4482, {0xB3, 0xDD, 0x02, 0xD9, 0x2C, 0x06, 0x00, 0x3E}};

static std::atomic<long> g_moduleRefs{0};
void XonarApoModuleLock() noexcept { g_moduleRefs.fetch_add(1); }
void XonarApoModuleUnlock() noexcept { g_moduleRefs.fetch_sub(1); }

} // namespace xonar::apo

using xonar::apo::CLSID_XonarEfxApo;
using xonar::apo::CLSID_XonarSfxApo;
using xonar::apo::Stage;
using xonar::apo::XonarApo;

static HMODULE g_hModule = nullptr;

// ---------------------------------------------------------------------------
// Fabrique de classe
// ---------------------------------------------------------------------------
namespace {

class XonarApoFactory final : public IClassFactory {
public:
    explicit XonarApoFactory(Stage stage) : stage_(stage) { xonar::apo::XonarApoModuleLock(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        wchar_t text[40] = {};
        StringFromGUID2(riid, text, 40);
        const std::wstring wide(text);
        xonar::apo::apoLog(stage_, 0, "Factory QueryInterface " + std::string(wide.begin(), wide.end()));
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) {
            xonar::apo::XonarApoModuleUnlock();
            delete this;
        }
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        xonar::apo::apoLog(stage_, 0, pUnkOuter ? "CreateInstance aggregated=true" : "CreateInstance aggregated=false");
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter && riid != __uuidof(IUnknown)) return CLASS_E_NOAGGREGATION;
        auto* apo = new (std::nothrow) XonarApo(stage_, pUnkOuter);
        if (!apo) return E_OUTOFMEMORY;
        const HRESULT hr = apo->innerUnknown()->QueryInterface(riid, ppv);
        apo->innerUnknown()->Release(); // ne jamais déléguer la référence initiale à outer
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) {
            xonar::apo::XonarApoModuleLock();
        } else {
            xonar::apo::XonarApoModuleUnlock();
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    Stage stage_;
};

std::wstring clsidToString(const CLSID& clsid) {
    wchar_t buf[64] = {};
    StringFromGUID2(clsid, buf, 64);
    return std::wstring(buf);
}

// Écrit HKCR\CLSID\{clsid}\ (+ InprocServer32, ThreadingModel) pour une classe.
LONG registerClsid(const CLSID& clsid, const wchar_t* friendlyName, const wchar_t* modulePath) {
    const std::wstring base = L"CLSID\\" + clsidToString(clsid);
    HKEY k = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CLASSES_ROOT, base.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(friendlyName),
                   static_cast<DWORD>((wcslen(friendlyName) + 1) * sizeof(wchar_t)));
    HKEY ips = nullptr;
    rc = RegCreateKeyExW(k, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &ips, nullptr);
    if (rc == ERROR_SUCCESS) {
        RegSetValueExW(ips, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(modulePath),
                       static_cast<DWORD>((wcslen(modulePath) + 1) * sizeof(wchar_t)));
        const wchar_t* both = L"Both";
        RegSetValueExW(ips, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(both),
                       static_cast<DWORD>((wcslen(both) + 1) * sizeof(wchar_t)));
        RegCloseKey(ips);
    }
    RegCloseKey(k);
    return rc;
}

LONG unregisterClsid(const CLSID& clsid) {
    const std::wstring base = L"CLSID\\" + clsidToString(clsid);
    return RegDeleteTreeW(HKEY_CLASSES_ROOT, base.c_str());
}

void setSz(HKEY k, const wchar_t* name, const wchar_t* value) {
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                   static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}
void setDword(HKEY k, const wchar_t* name, DWORD value) {
    RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

// Liste d'APO du moteur audio (normalement écrite par RegisterAPO) :
// HKCR\AudioEngine\AudioProcessingObjects\{clsid}. Requise pour que le moteur
// reconnaisse la coclasse comme un APO système (confirmé E0-04).
LONG registerApoList(const CLSID& clsid, const wchar_t* friendly, DWORD flags) {
    const std::wstring base = L"AudioEngine\\AudioProcessingObjects\\" + clsidToString(clsid);
    HKEY k = nullptr;
    const LONG rc =
        RegCreateKeyExW(HKEY_CLASSES_ROOT, base.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
    if (rc != ERROR_SUCCESS) return rc;
    setSz(k, nullptr, friendly);
    setSz(k, L"FriendlyName", friendly);
    setSz(k, L"Copyright", L"Xonar SE Open Driver");
    setDword(k, L"MajorVersion", 1);
    setDword(k, L"MinorVersion", 0);
    setDword(k, L"Flags", flags);
    setDword(k, L"MinInputConnections", 1);
    setDword(k, L"MaxInputConnections", 1);
    setDword(k, L"MinOutputConnections", 1);
    setDword(k, L"MaxOutputConnections", 1);
    setDword(k, L"MaxInstances", 0xFFFFFFFF);
    setDword(k, L"NumAPOInterfaces", 1);
    setSz(k, L"APOInterface0", L"{FD7F2B29-24D0-4B5C-B177-592C39F9CA10}"); // IID_IAudioProcessingObject
    RegCloseKey(k);
    return ERROR_SUCCESS;
}

LONG unregisterApoList(const CLSID& clsid) {
    const std::wstring base = L"AudioEngine\\AudioProcessingObjects\\" + clsidToString(clsid);
    return RegDeleteTreeW(HKEY_CLASSES_ROOT, base.c_str());
}

} // namespace

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------
extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hInst;
        DisableThreadLibraryCalls(hInst);
    } else if (reason == DLL_PROCESS_DETACH) {
        xonar::apo::apoTraceUnregister();
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    Stage stage;
    if (rclsid == CLSID_XonarSfxApo) {
        stage = Stage::Sfx;
    } else if (rclsid == CLSID_XonarEfxApo) {
        stage = Stage::Efx;
    } else {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    xonar::apo::apoLog(stage, 0, "DllGetClassObject");
    auto* factory = new (std::nothrow) XonarApoFactory(stage);
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return xonar::apo::g_moduleRefs.load() == 0 ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(g_hModule, path, MAX_PATH) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const DWORD sfxFlags = APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH;
    const DWORD efxFlags = sfxFlags | APO_FLAG_SAMPLESPERFRAME_MUST_MATCH;

    LONG rc = registerClsid(CLSID_XonarSfxApo, L"Xonar SE SFX APO", path);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = registerClsid(CLSID_XonarEfxApo, L"Xonar SE EFX APO", path);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = registerApoList(CLSID_XonarSfxApo, L"Xonar SE SFX APO", sfxFlags);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    rc = registerApoList(CLSID_XonarEfxApo, L"Xonar SE EFX APO", efxFlags);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    const LONG a = unregisterClsid(CLSID_XonarSfxApo);
    const LONG b = unregisterClsid(CLSID_XonarEfxApo);
    const LONG c = unregisterApoList(CLSID_XonarSfxApo);
    const LONG d = unregisterApoList(CLSID_XonarEfxApo);
    auto ok = [](LONG r) { return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND; };
    return (ok(a) && ok(b) && ok(c) && ok(d)) ? S_OK : E_FAIL;
}
