// SPDX-License-Identifier: Apache-2.0
// Diagnostic utilisateur : aucun changement de registre, volume ou format système.
// --inspect charge une DLL explicitement dans CET hôte ; ce n'est pas une preuve live.
// --render ouvre uniquement l'endpoint Xonar exact, en partagé non RAW, pendant 8 s.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audioenginebaseapo.h>
#include <mmreg.h>
#include <cfgmgr32.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include "../../src/windows-apo/src/media_type_min.hpp"

namespace {
std::wstring endpointGuid = L"{1006bf93-c025-454c-8caa-45c633fb1eb7}";
const CLSID sfx = {0xc243a8cb,0xd535,0x4ba0,{0xbc,0xc5,0xcc,0x43,0x38,0x83,0x3e,0x7a}};
const CLSID efx = {0xd2fbc737,0x00c3,0x4482,{0xb3,0xdd,0x02,0xd9,0x2c,0x06,0x00,0x3e}};
void check(HRESULT hr, const char* operation) {
    if(FAILED(hr)) { std::cerr << operation << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << '\n'; throw hr; }
}
template<class T> struct Com {
    T* p=nullptr;
    ~Com(){if(p)p->Release();}
    T** put(){return &p;}
    T* operator->()const{return p;}
};
bool targetMatchesHardware() {
    std::wstring path=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\";
    path+=endpointGuid; path+=L"\\Properties";
    wchar_t devnode[2048]={}; DWORD size=sizeof(devnode);
    auto rc=RegGetValueW(HKEY_LOCAL_MACHINE,path.c_str(),L"{b3f8fa53-0004-438e-9003-51a46e139bfc},2",
                        RRF_RT_REG_SZ,nullptr,devnode,&size);
    if(rc!=ERROR_SUCCESS)return false;
    std::wstring node(devnode);
    for(auto& c:node)c=static_cast<wchar_t>(towupper(c));
    return node.find(L"USB\\VID_0B05&PID_189D&MI_00\\")!=std::wstring::npos;
}
int modes() {
    GUID audio={STATIC_KSCATEGORY_AUDIO};
    GUID processing={STATIC_KSPROPSETID_AudioSignalProcessing};
    wchar_t instance[]=L"USB\\VID_0B05&PID_189D&MI_00\\9&120D40DE&0&0000";
    ULONG length=0;
    if(CM_Get_Device_Interface_List_SizeW(&length,&audio,instance,CM_GET_DEVICE_INTERFACE_LIST_PRESENT)!=CR_SUCCESS)return 2;
    std::vector<wchar_t> paths(length);
    if(CM_Get_Device_Interface_ListW(&audio,instance,paths.data(),length,CM_GET_DEVICE_INTERFACE_LIST_PRESENT)!=CR_SUCCESS)return 2;
    bool found=false;
    for(const wchar_t* p=paths.data();*p;p+=wcslen(p)+1){
        std::wstring path(p);for(auto& c:path)c=static_cast<wchar_t>(towlower(c));
        if(!path.ends_with(L"\\wavespeaker"))continue;
        found=true;
        HANDLE filter=CreateFileW(p,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        if(filter==INVALID_HANDLE_VALUE){std::cout<<"KS open error="<<GetLastError()<<'\n';return 2;}
        KSP_PIN request={};request.Property.Set=processing;request.Property.Id=KSPROPERTY_AUDIOSIGNALPROCESSING_MODES;
        request.Property.Flags=KSPROPERTY_TYPE_GET;request.PinId=0;
        std::vector<BYTE> buffer(4096);DWORD bytes=0;
        BOOL ok=DeviceIoControl(filter,IOCTL_KS_PROPERTY,&request,sizeof(request),buffer.data(),static_cast<DWORD>(buffer.size()),&bytes,nullptr);
        DWORD error=ok?0:GetLastError();CloseHandle(filter);
        std::cout<<"KS WaveSpeaker pin0 AUDIOSIGNALPROCESSING_MODES ok="<<ok<<" error="<<error<<" bytes="<<bytes<<'\n';
        if(ok && bytes>=sizeof(KSMULTIPLE_ITEM)){
            auto* header=reinterpret_cast<const KSMULTIPLE_ITEM*>(buffer.data());
            if(header->Count>(bytes-sizeof(*header))/sizeof(GUID))return 2;
            auto* values=reinterpret_cast<const GUID*>(buffer.data()+sizeof(*header));
            for(ULONG i=0;i<header->Count;i++){wchar_t text[40]={};StringFromGUID2(values[i],text,40);std::wcout<<L"MODE "<<text<<L'\n';}
        }
    }
    return found?0:2;
}
int inspect(const wchar_t* path, bool registerMetadata=false) {
    HMODULE module=LoadLibraryExW(path,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module){std::cerr<<"LoadLibrary error="<<GetLastError()<<'\n';return 1;}
    auto entry=reinterpret_cast<HRESULT(__stdcall*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
    if(!entry){FreeLibrary(module);return 1;}
    int errors=0;
    for(const CLSID& id:{sfx,efx}) {
        Com<IClassFactory> factory; check(entry(id,__uuidof(IClassFactory),reinterpret_cast<void**>(factory.put())),"DllGetClassObject");
        if(!registerMetadata) {
            struct Outer final : IUnknown {
                ULONG refs=1;
                IUnknown* inner=nullptr;
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
                    if(!out)return E_POINTER;
                    if(iid==__uuidof(IUnknown)){*out=this;AddRef();return S_OK;}
                    return inner->QueryInterface(iid,out);
                }
                ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
                ULONG STDMETHODCALLTYPE Release() override{return --refs;}
            } outer;
            void* invalid=reinterpret_cast<void*>(1);
            HRESULT rejected=factory->CreateInstance(&outer,__uuidof(IAudioProcessingObject),&invalid);
            if(rejected!=CLASS_E_NOAGGREGATION || invalid)++errors;
            check(factory->CreateInstance(&outer,__uuidof(IUnknown),reinterpret_cast<void**>(&outer.inner)),"CreateInstance aggregated IUnknown");
            if(outer.refs!=1)++errors; // l'interne ne possède pas son contrôleur
            IAudioProcessingObject* aggregated=nullptr;
            check(outer.inner->QueryInterface(__uuidof(IAudioProcessingObject),reinterpret_cast<void**>(&aggregated)),"inner QI APO");
            if(outer.refs!=2)++errors;
            IUnknown* identity=nullptr;
            check(aggregated->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&identity)),"aggregated QI identity");
            if(identity!=static_cast<IUnknown*>(&outer) || outer.refs!=3)++errors;
            identity->Release();aggregated->Release();
            if(outer.refs!=1 || outer.inner->Release()!=0)++errors;
            std::cout<<"COM aggregation: controlling identity, references and rejection verified\n";
        }
        Com<IAudioProcessingObject> apo;
        check(factory->CreateInstance(nullptr,__uuidof(IAudioProcessingObject),reinterpret_cast<void**>(apo.put())),"CreateInstance");
        APO_REG_PROPERTIES* props=nullptr;check(apo->GetRegistrationProperties(&props),"GetRegistrationProperties");
        wchar_t guid[40]={};StringFromGUID2(props->iidAPOInterfaceList[0],guid,40);
        std::wcout<<(id==efx?L"EFX":L"SFX")<<L" NumAPOInterfaces="<<props->u32NumAPOInterfaces
                  <<L" IID[0]="<<guid<<L" flags="<<props->Flags<<'\n';
        if(props->u32NumAPOInterfaces!=1 || props->iidAPOInterfaceList[0]!=__uuidof(IAudioProcessingObject) || props->clsid!=id){
            ++errors;CoTaskMemFree(props);continue;
        }
        if(registerMetadata){
            HRESULT registerHr=RegisterAPO(props);
            CoTaskMemFree(props);
            check(registerHr,"RegisterAPO (API SDK officielle)");
            std::cout<<"RegisterAPO S_OK\n";
            continue;
        }
        CoTaskMemFree(props);
        auto* mediaType=new xonar::apo::MediaType(xonar::apo::makeFloatFormat(48000,6,0x60f));
        IAudioMediaType* supported=nullptr;
        HRESULT formatHr=apo->IsInputFormatSupported(nullptr,mediaType,&supported);
        bool validInput=formatHr==S_OK && supported==mediaType;
        if(supported)supported->Release();
        supported=nullptr;
        formatHr=apo->IsOutputFormatSupported(nullptr,mediaType,&supported);
        bool validOutput=formatHr==S_OK && supported==mediaType;
        if(supported)supported->Release();
        std::cout<<"FORMAT S_OK returns AddRef format: input="<<validInput<<" output="<<validOutput<<'\n';
        if(!validInput || !validOutput)++errors;
        check(apo->Initialize(0,nullptr),"Initialize hote hors materiel");
        Com<IAudioProcessingObjectConfiguration> config;
        Com<IAudioProcessingObjectRT> rt;
        check(apo->QueryInterface(__uuidof(IAudioProcessingObjectConfiguration),reinterpret_cast<void**>(config.put())),"QI Configuration");
        check(apo->QueryInterface(__uuidof(IAudioProcessingObjectRT),reinterpret_cast<void**>(rt.put())),"QI RT");
        constexpr unsigned frames=128;
        std::vector<float> input(frames*6),output(frames*6);
        APO_CONNECTION_DESCRIPTOR inDescriptor={APO_CONNECTION_BUFFER_TYPE_EXTERNAL,reinterpret_cast<UINT_PTR>(input.data()),frames,mediaType,APO_CONNECTION_DESCRIPTOR_SIGNATURE};
        APO_CONNECTION_DESCRIPTOR outDescriptor={APO_CONNECTION_BUFFER_TYPE_EXTERNAL,reinterpret_cast<UINT_PTR>(output.data()),frames,mediaType,APO_CONNECTION_DESCRIPTOR_SIGNATURE};
        APO_CONNECTION_DESCRIPTOR* inDescriptors[]={&inDescriptor};APO_CONNECTION_DESCRIPTOR* outDescriptors[]={&outDescriptor};
        check(config->LockForProcess(1,inDescriptors,1,outDescriptors),"LockForProcess 6xfloat32");
        double error=0;
        for(unsigned channel=0;channel<6;++channel){
            std::fill(input.begin(),input.end(),0.f);std::fill(output.begin(),output.end(),0.f);
            input[channel]=.01f;
            APO_CONNECTION_PROPERTY inProperty={reinterpret_cast<UINT_PTR>(input.data()),frames,BUFFER_VALID,APO_CONNECTION_PROPERTY_SIGNATURE};
            APO_CONNECTION_PROPERTY outProperty={reinterpret_cast<UINT_PTR>(output.data()),0,BUFFER_INVALID,APO_CONNECTION_PROPERTY_SIGNATURE};
            APO_CONNECTION_PROPERTY* inProperties[]={&inProperty};APO_CONNECTION_PROPERTY* outProperties[]={&outProperty};
            rt->APOProcess(1,inProperties,1,outProperties);
            for(unsigned n=0;n<frames*6;++n)error=std::max(error,std::fabs(static_cast<double>(output[n]-input[n])));
            if(outProperty.u32ValidFrameCount!=frames || outProperty.u32BufferFlags!=BUFFER_VALID)++errors;
        }
        check(config->UnlockForProcess(),"UnlockForProcess");
        std::cout<<"DSP 6 individual impulses, identity maxError="<<error<<'\n';
        if(error>2e-6)++errors;
        mediaType->Release();
    }
    FreeLibrary(module);return errors?2:0;
}
int render(bool audible, bool media, unsigned seconds=8, double frequency=997., int selectedChannel=0, bool sourceStereo=false, bool sourceMono=false, unsigned sourceRate=48000, bool pcm16=false) {
    if(!targetMatchesHardware()) {std::cerr<<"HWID de l'endpoint non conforme : refus.\n";return 2;}
    std::wstring id=L"{0.0.0.00000000}.";id+=endpointGuid;
    Com<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(enumerator.put())),"MMDeviceEnumerator");
    Com<IMMDevice> device;check(enumerator->GetDevice(id.c_str(),device.put()),"GetDevice exact");
    Com<IAudioClient> client;check(device->Activate(__uuidof(IAudioClient),CLSCTX_INPROC_SERVER,nullptr,reinterpret_cast<void**>(client.put())),"Activate");
    Com<IAudioClient2> client2;check(client->QueryInterface(__uuidof(IAudioClient2),reinterpret_cast<void**>(client2.put())),"IAudioClient2");
    AudioClientProperties properties={};properties.cbSize=sizeof(properties);properties.bIsOffload=FALSE;
    properties.eCategory=media?AudioCategory_Media:AudioCategory_Other;properties.Options=AUDCLNT_STREAMOPTIONS_NONE;
    check(client2->SetClientProperties(&properties),"SetClientProperties non RAW");
    WAVEFORMATEX* raw=nullptr;check(client->GetMixFormat(&raw),"GetMixFormat");
    std::vector<BYTE> storage(sizeof(WAVEFORMATEX)+raw->cbSize);memcpy(storage.data(),raw,storage.size());CoTaskMemFree(raw);
    auto* f=reinterpret_cast<WAVEFORMATEX*>(storage.data());
    if(f->wFormatTag!=WAVE_FORMAT_EXTENSIBLE || f->cbSize<22 || f->nChannels!=6 || f->wBitsPerSample!=32 || f->nSamplesPerSec!=48000) {
        std::cerr<<"Format 6xfloat32x48000 attendu : refus sans modification.\n";return 2;
    }
    auto* ext=reinterpret_cast<WAVEFORMATEXTENSIBLE*>(f);
    const GUID floatSubtype={3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
    if(ext->dwChannelMask!=0x60f || ext->SubFormat!=floatSubtype){std::cerr<<"Masque/sous-type inattendu.\n";return 2;}
    if(sourceStereo || sourceMono) {
        if(selectedChannel>1)return 2;
        f->nChannels=sourceMono?1:2;f->nBlockAlign=static_cast<WORD>(f->nChannels*4);
        ext->dwChannelMask=sourceMono?4:3;
    }
    f->nSamplesPerSec=sourceRate;f->nAvgBytesPerSec=sourceRate*f->nBlockAlign;
    if(pcm16) {
        f->wBitsPerSample=16;ext->Samples.wValidBitsPerSample=16;ext->SubFormat.Data1=1;
        f->nBlockAlign=static_cast<WORD>(f->nChannels*2);f->nAvgBytesPerSec=sourceRate*f->nBlockAlign;
    }
    const unsigned streamChannels=f->nChannels;
    DWORD flags = AUDCLNT_STREAMFLAGS_NOPERSIST;
    if(sourceRate != 48000) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,flags,1000000,0,f,nullptr),"Initialize shared");
    Com<IAudioRenderClient> renderer;check(client->GetService(__uuidof(IAudioRenderClient),reinterpret_cast<void**>(renderer.put())),"RenderClient");
    UINT32 capacity=0;check(client->GetBufferSize(&capacity),"GetBufferSize");
    UINT64 frame=0;
    auto fill=[&](UINT32 count){
        BYTE* bytes=nullptr;check(renderer->GetBuffer(count,&bytes),"GetBuffer");
        if(audible) {
            auto* samples=reinterpret_cast<float*>(bytes);
            for(UINT32 n=0;n<count;++n,++frame){
                const double t=static_cast<double>(frame)/sourceRate;
                const double ramp=std::min(1.,std::min(t/.02,(seconds-t)/.02));
                const float value=static_cast<float>(.01*std::max(0.,ramp)*std::sin(2.*3.141592653589793*frequency*t));
                for(unsigned channel=0;channel<streamChannels;++channel) {
                    const float sample=(selectedChannel==static_cast<int>(channel) || (selectedChannel==-1 && channel<2))?value:0.f;
                    if(pcm16)reinterpret_cast<INT16*>(bytes)[n*streamChannels+channel]=static_cast<INT16>(sample*32767.f);
                    else samples[n*streamChannels+channel]=sample;
                }
            }
        }
        check(renderer->ReleaseBuffer(count,audible?0:AUDCLNT_BUFFERFLAGS_SILENT),"ReleaseBuffer");
    };
    fill(capacity);check(client->Start(),"Start");
    std::wcout<<L"LIVE endpoint="<<endpointGuid<<L" ";
    std::cout<<"mixChannels=6 streamChannels="<<streamChannels<<" mixRate=48000 streamRate="<<sourceRate<<" streamBits="<<f->wBitsPerSample<<" category="<<(media?"Media":"Other")
             <<" offload=false raw=false audible="<<audible<<" frequency="<<frequency<<" channel="<<selectedChannel<<" level=-40dBFS"<<std::endl;
    ULONGLONG start=GetTickCount64();
    while(GetTickCount64()-start<static_cast<ULONGLONG>(seconds)*1000){
        Sleep(5);UINT32 padding=0;check(client->GetCurrentPadding(&padding),"GetCurrentPadding");
        if(padding<capacity)fill(capacity-padding);
    }
    check(client->Stop(),"Stop");
    std::cout<<"LIVE stopped normally\n";return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    HRESULT init=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(init))return 1;
    int result=1;
    try {
        for(int i=1;i+1<argc;++i){
            if(std::wstring(argv[i])==L"--endpoint"){
                CLSID guid={};check(CLSIDFromString(argv[++i],&guid),"GUID endpoint");
                wchar_t canonical[40]={};StringFromGUID2(guid,canonical,40);endpointGuid=canonical;
            }
        }
        if(argc==3 && std::wstring(argv[1])==L"--inspect")result=inspect(argv[2]);
        else if(argc==3 && std::wstring(argv[1])==L"--register-metadata")result=inspect(argv[2],true);
        else if(argc==2 && std::wstring(argv[1])==L"--ks-modes")result=modes();
        else if(argc>=2 && std::wstring(argv[1])==L"--render"){
            bool audible=false,media=false,sourceStereo=false,sourceMono=false,pcm16=false;unsigned seconds=8,sourceRate=48000;double frequency=997;int channel=0;
            for(int i=2;i<argc;++i){
                std::wstring arg=argv[i];audible|=arg==L"--audible";media|=arg==L"--media";
                sourceStereo|=arg==L"--source-stereo";
                sourceMono|=arg==L"--source-mono";
                pcm16|=arg==L"--source-pcm16";
                if(arg==L"--source-rate" && i+1<argc){sourceRate=std::stoul(argv[++i]);if(sourceRate!=44100 && sourceRate!=48000 && sourceRate!=96000 && sourceRate!=192000)return 2;}
                if(arg==L"--seconds" && i+1<argc)seconds=std::clamp(static_cast<unsigned>(std::stoul(argv[++i])),1u,600u);
                if(arg==L"--frequency" && i+1<argc)frequency=std::clamp(std::stod(argv[++i]),20.,16000.);
                if(arg==L"--channel" && i+1<argc){std::wstring name=argv[++i];channel=name==L"stereo"?-1:std::stoi(name);if(channel < -1 || channel > 5)return 2;}
            }
            result=render(audible,media,seconds,frequency,channel,sourceStereo,sourceMono,sourceRate,pcm16);
        }else std::cout<<"--inspect <absolute DLL> | --render [--media] [--audible]\n";
    }catch(...){result=1;}
    CoUninitialize();return result;
}
