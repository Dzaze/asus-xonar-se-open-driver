// usb-descriptor-dump : lecture seule des descripteurs USB d'un périphérique, sans pilote
// particulier, en interrogeant les hubs comme le fait USBView de Microsoft.
//
// Principe : on ouvre chaque contrôleur hôte, on obtient son hub racine, puis on
// parcourt les ports (récursivement pour les hubs externes). Pour le périphérique
// ciblé, on envoie des requêtes GET_DESCRIPTOR via IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,
// qui est une opération de lecture pure : rien n'est écrit dans le périphérique.
//
// Le numéro de série est volontairement rédigé (seule sa longueur est affichée).

#include <windows.h>
#include <winioctl.h> // CTL_CODE, requis par usbioctl.h quand WIN32_LEAN_AND_MEAN est défini
#include <initguid.h>
#include <usbiodef.h>
#include <usb.h>
#include <usbioctl.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Sortie texte : console + fichier optionnel (même contenu, UTF-8).
// ---------------------------------------------------------------------------
class Printer {
public:
    void line(const std::string& s) {
        buffer_ += s;
        buffer_ += '\n';
        std::fputs(s.c_str(), stdout);
        std::fputc('\n', stdout);
    }
    template <class... A>
    void fmt(std::format_string<A...> f, A&&... a) {
        line(std::format(f, std::forward<A>(a)...));
    }
    bool saveTo(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;
        out.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        return static_cast<bool>(out);
    }

private:
    std::string buffer_;
};

std::string utf16ToUtf8(const wchar_t* s, int lengthChars) {
    if (lengthChars <= 0) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, s, lengthChars, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, lengthChars, out.data(), needed, nullptr, nullptr);
    return out;
}

std::string utf16ToUtf8(const std::wstring& s) {
    return utf16ToUtf8(s.c_str(), static_cast<int>(s.size()));
}

std::string lastErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string text = n ? utf16ToUtf8(msg, static_cast<int>(n)) : std::string{};
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return std::format("erreur {} ({})", err, text);
}

std::string hexDump(const std::uint8_t* data, std::size_t size, const std::string& indent) {
    std::string out;
    for (std::size_t off = 0; off < size; off += 16) {
        out += indent + std::format("{:04X}  ", off);
        for (std::size_t i = 0; i < 16; ++i) {
            if (off + i < size) out += std::format("{:02X} ", data[off + i]);
            else out += "   ";
            if (i == 7) out += ' ';
        }
        out += '\n';
    }
    if (!out.empty()) out.pop_back();
    return out;
}

std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// ---------------------------------------------------------------------------
// Tables de noms (USB Audio Class 2.0, HID).
// ---------------------------------------------------------------------------
const char* terminalTypeName(std::uint16_t t) {
    switch (t) {
    case 0x0100: return "USB Undefined";
    case 0x0101: return "USB Streaming";
    case 0x01FF: return "USB vendor specific";
    case 0x0200: return "Input Undefined";
    case 0x0201: return "Microphone";
    case 0x0202: return "Desktop microphone";
    case 0x0203: return "Personal microphone";
    case 0x0204: return "Omni-directional microphone";
    case 0x0205: return "Microphone array";
    case 0x0206: return "Processing microphone array";
    case 0x0300: return "Output Undefined";
    case 0x0301: return "Speaker";
    case 0x0302: return "Headphones";
    case 0x0303: return "Head Mounted Display Audio";
    case 0x0304: return "Desktop speaker";
    case 0x0305: return "Room speaker";
    case 0x0306: return "Communication speaker";
    case 0x0307: return "Low frequency effects speaker";
    case 0x0400: return "Bi-directional Undefined";
    case 0x0401: return "Handset";
    case 0x0402: return "Headset";
    case 0x0403: return "Speakerphone, no echo reduction";
    case 0x0404: return "Echo-suppressing speakerphone";
    case 0x0405: return "Echo-canceling speakerphone";
    case 0x0500: return "Telephony Undefined";
    case 0x0501: return "Phone line";
    case 0x0502: return "Telephone";
    case 0x0503: return "Down Line Phone";
    case 0x0504: return "Telephony (reserved 0x0504)";
    case 0x0505: return "Telephony (reserved 0x0505)";
    case 0x0600: return "External Undefined";
    case 0x0601: return "Analog connector";
    case 0x0602: return "Digital audio interface";
    case 0x0603: return "Line connector";
    case 0x0604: return "Legacy audio connector";
    case 0x0605: return "S/PDIF interface";
    case 0x0606: return "1394 DA stream";
    case 0x0607: return "1394 DV stream soundtrack";
    case 0x0608: return "ADAT Lightpipe";
    case 0x0609: return "TDIF";
    case 0x060A: return "MADI";
    case 0x0700: return "Embedded Undefined";
    case 0x0701: return "Level Calibration Noise Source";
    case 0x0702: return "Equalization Noise";
    case 0x0703: return "CD player";
    case 0x0704: return "DAT";
    case 0x0705: return "DCC";
    case 0x0706: return "Compressed Audio Player";
    case 0x0707: return "Analog Tape";
    case 0x0708: return "Phonograph";
    case 0x0709: return "VCR Audio";
    case 0x070A: return "Video Disc Audio";
    case 0x070B: return "DVD Audio";
    case 0x070C: return "TV Tuner Audio";
    case 0x070D: return "Satellite Receiver Audio";
    case 0x070E: return "Cable Tuner Audio";
    case 0x070F: return "DSS Audio";
    case 0x0710: return "Radio Receiver";
    case 0x0711: return "Radio Transmitter";
    case 0x0712: return "Multi-track Recorder";
    case 0x0713: return "Synthesizer";
    case 0x0714: return "Piano";
    case 0x0715: return "Guitar";
    case 0x0716: return "Drums/Rhythm";
    case 0x0717: return "Other Musical Instrument";
    default: return "inconnu";
    }
}

std::string channelConfigText(std::uint32_t bm) {
    static const char* names[32] = {
        "FL", "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR", "TC",
        "TFL", "TFC", "TFR", "TBL", "TBC", "TBR", "TFLC", "TFRC", "LLFE", "RLFE", "TSL", "TSR",
        "BottomC", "BLC", "BRC", "bit27", "bit28", "bit29", "bit30", "RawData"};
    if (bm == 0) return "aucun (positions non spécifiées)";
    std::string s;
    for (int i = 0; i < 32; ++i) {
        if (bm & (1u << i)) {
            if (!s.empty()) s += ' ';
            s += names[i];
        }
    }
    return s;
}

const char* twoBitControl(std::uint32_t v) {
    switch (v & 3) {
    case 0: return "absent";
    case 1: return "RO";
    case 3: return "RW";
    default: return "réservé";
    }
}

std::string controlsText(std::uint32_t bm, const char* const* names, int count) {
    std::string s;
    for (int i = 0; i < count; ++i) {
        const std::uint32_t v = (bm >> (2 * i)) & 3;
        if (v == 0) continue;
        if (!s.empty()) s += ", ";
        s += std::format("{}={}", names[i], twoBitControl(v));
    }
    return s.empty() ? "aucun" : s;
}

std::string formatsType1Text(std::uint32_t bm) {
    std::string s;
    auto add = [&](const char* n) { if (!s.empty()) s += ' '; s += n; };
    if (bm & 0x01) add("PCM");
    if (bm & 0x02) add("PCM8");
    if (bm & 0x04) add("IEEE_FLOAT");
    if (bm & 0x08) add("ALAW");
    if (bm & 0x10) add("MULAW");
    if (bm & 0x80000000u) add("TYPE_I_RAW_DATA");
    return s.empty() ? std::format("0x{:08X}", bm) : s;
}

std::string interfaceClassText(std::uint8_t cls, std::uint8_t sub, std::uint8_t proto) {
    switch (cls) {
    case 0x01: {
        const char* subName = sub == 0x01 ? "AudioControl" : sub == 0x02 ? "AudioStreaming" : sub == 0x03 ? "MIDIStreaming" : "sous-classe inconnue";
        const char* protoName = proto == 0x00 ? "UAC1" : proto == 0x20 ? "UAC2" : proto == 0x30 ? "UAC3" : "protocole inconnu";
        return std::format("Audio / {} / {}", subName, protoName);
    }
    case 0x03: return std::format("HID (sous-classe {}, protocole {})", sub, proto);
    case 0x08: return "Mass Storage";
    case 0x09: return "Hub";
    case 0x0E: return "Video";
    case 0xFE: return "Application specific";
    case 0xFF: return "Vendor specific";
    default: return std::format("classe 0x{:02X}", cls);
    }
}

const char* speedText(UCHAR speed) {
    switch (speed) {
    case UsbLowSpeed: return "Low Speed (1,5 Mb/s)";
    case UsbFullSpeed: return "Full Speed (12 Mb/s)";
    case UsbHighSpeed: return "High Speed (480 Mb/s)";
    case UsbSuperSpeed: return "SuperSpeed (5 Gb/s)";
    default: return "vitesse inconnue";
    }
}

const char* hidUsagePageName(USAGE page) {
    switch (page) {
    case 0x01: return "Generic Desktop";
    case 0x02: return "Simulation";
    case 0x07: return "Keyboard/Keypad";
    case 0x08: return "LED";
    case 0x09: return "Button";
    case 0x0B: return "Telephony";
    case 0x0C: return "Consumer";
    case 0x0D: return "Digitizer";
    default: return (page >= 0xFF00) ? "Vendor-defined" : "autre";
    }
}

const char* hidUsageName(USAGE page, USAGE usage) {
    if (page == 0x0C) {
        switch (usage) {
        case 0x01: return "Consumer Control";
        case 0x30: return "Power";
        case 0xB0: return "Play";
        case 0xB1: return "Pause";
        case 0xB3: return "Fast Forward";
        case 0xB4: return "Rewind";
        case 0xB5: return "Scan Next Track";
        case 0xB6: return "Scan Previous Track";
        case 0xB7: return "Stop";
        case 0xCD: return "Play/Pause";
        case 0xE0: return "Volume";
        case 0xE2: return "Mute";
        case 0xE9: return "Volume Increment";
        case 0xEA: return "Volume Decrement";
        default: return "";
        }
    }
    if (page == 0x0B) {
        switch (usage) {
        case 0x01: return "Phone";
        case 0x05: return "Headset";
        case 0x20: return "Hook Switch";
        case 0x21: return "Flash";
        case 0x2F: return "Phone Mute";
        default: return "";
        }
    }
    if (page == 0x01) {
        switch (usage) {
        case 0x80: return "System Control";
        case 0x81: return "System Power Down";
        case 0x82: return "System Sleep";
        case 0x83: return "System Wake Up";
        default: return "";
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Options de ligne de commande.
// ---------------------------------------------------------------------------
struct Options {
    std::uint16_t vid = 0x0B05;
    std::uint16_t pid = 0x189D;
    bool all = false;
    bool hid = true;
    std::string outPath;
    bool help = false;
};

void printHelp() {
    std::puts(
        "usb-descriptor-dump : lecture seule des descripteurs USB d'un périphérique (style USBView).\n"
        "\n"
        "Usage : usb-descriptor-dump [--vid 0x0B05] [--pid 0x189D] [--all] [--no-hid] [--out fichier.txt]\n"
        "\n"
        "  --vid <hex>     Identifiant fabricant cible (défaut 0x0B05, ASUS)\n"
        "  --pid <hex>     Identifiant produit cible (défaut 0x189D, Xonar SE)\n"
        "  --all           Lister en résumé tous les périphériques USB présents\n"
        "  --no-hid        Ne pas interroger les collections HID via hid.dll\n"
        "  --out <fichier> Écrire aussi la sortie dans ce fichier (UTF-8)\n"
        "  --help          Cette aide\n"
        "\n"
        "Codes de retour : 0 périphérique cible trouvé, 2 non trouvé, 1 erreur.\n"
        "L'outil n'écrit jamais dans les périphériques : seules des requêtes GET_DESCRIPTOR sont émises.");
}

bool parseHex(const std::string& s, std::uint16_t& out) {
    try {
        const unsigned long v = std::stoul(s, nullptr, 16);
        if (v > 0xFFFF) return false;
        out = static_cast<std::uint16_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<Options> parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--help" || a == "-h") o.help = true;
        else if (a == "--all") o.all = true;
        else if (a == "--no-hid") o.hid = false;
        else if (a == "--out") { if (!next(v)) return std::nullopt; o.outPath = v; }
        else if (a == "--vid") { if (!next(v) || !parseHex(v, o.vid)) return std::nullopt; }
        else if (a == "--pid") { if (!next(v) || !parseHex(v, o.pid)) return std::nullopt; }
        else return std::nullopt;
    }
    return o;
}

// ---------------------------------------------------------------------------
// Accès aux hubs.
// ---------------------------------------------------------------------------
struct HubHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HubHandle(const std::wstring& path) {
        h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    ~HubHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    HubHandle(const HubHandle&) = delete;
    HubHandle& operator=(const HubHandle&) = delete;
};

// Requête GET_DESCRIPTOR relayée par le hub. bmRequest 0x80 = device, 0x81 = interface.
std::optional<std::vector<std::uint8_t>> getDescriptor(HANDLE hub, ULONG port, std::uint8_t bmRequest,
                                                       std::uint8_t type, std::uint8_t index,
                                                       std::uint16_t wIndex, std::uint16_t length, DWORD* err = nullptr) {
    const std::size_t size = sizeof(USB_DESCRIPTOR_REQUEST) + length;
    std::vector<std::uint8_t> buf(size, 0);
    auto* req = reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(buf.data());
    req->ConnectionIndex = port;
    req->SetupPacket.bmRequest = bmRequest;
    req->SetupPacket.bRequest = USB_REQUEST_GET_DESCRIPTOR;
    req->SetupPacket.wValue = static_cast<USHORT>((type << 8) | index);
    req->SetupPacket.wIndex = wIndex;
    req->SetupPacket.wLength = length;
    DWORD returned = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, buf.data(), static_cast<DWORD>(size),
                         buf.data(), static_cast<DWORD>(size), &returned, nullptr)) {
        if (err) *err = GetLastError();
        return std::nullopt;
    }
    if (returned < sizeof(USB_DESCRIPTOR_REQUEST)) return std::vector<std::uint8_t>{};
    const std::size_t got = returned - sizeof(USB_DESCRIPTOR_REQUEST);
    return std::vector<std::uint8_t>(req->Data, req->Data + got);
}

std::string getStringDescriptor(HANDLE hub, ULONG port, std::uint8_t index) {
    if (index == 0) return "";
    auto d = getDescriptor(hub, port, 0x80, USB_STRING_DESCRIPTOR_TYPE, index, 0x0409, 255);
    if (!d || d->size() < 2 || (*d)[1] != USB_STRING_DESCRIPTOR_TYPE) return "(indisponible)";
    const std::size_t len = (*d)[0];
    if (len < 2 || len > d->size()) return "(indisponible)";
    const int chars = static_cast<int>((len - 2) / 2);
    return utf16ToUtf8(reinterpret_cast<const wchar_t*>(d->data() + 2), chars);
}

std::optional<std::wstring> getConnectionName(HANDLE hub, ULONG port) {
    USB_NODE_CONNECTION_NAME probe{};
    probe.ConnectionIndex = port;
    DWORD returned = 0;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_NAME, &probe, sizeof(probe), &probe, sizeof(probe), &returned, nullptr))
        return std::nullopt;
    std::vector<std::uint8_t> buf(probe.ActualLength + sizeof(USB_NODE_CONNECTION_NAME), 0);
    auto* name = reinterpret_cast<USB_NODE_CONNECTION_NAME*>(buf.data());
    name->ConnectionIndex = port;
    if (!DeviceIoControl(hub, IOCTL_USB_GET_NODE_CONNECTION_NAME, buf.data(), static_cast<DWORD>(buf.size()),
                         buf.data(), static_cast<DWORD>(buf.size()), &returned, nullptr))
        return std::nullopt;
    return std::wstring(name->NodeName);
}

std::optional<std::wstring> getRootHubName(HANDLE controller) {
    USB_ROOT_HUB_NAME probe{};
    DWORD returned = 0;
    if (!DeviceIoControl(controller, IOCTL_USB_GET_ROOT_HUB_NAME, nullptr, 0, &probe, sizeof(probe), &returned, nullptr))
        return std::nullopt;
    std::vector<std::uint8_t> buf(probe.ActualLength + sizeof(USB_ROOT_HUB_NAME), 0);
    auto* name = reinterpret_cast<USB_ROOT_HUB_NAME*>(buf.data());
    if (!DeviceIoControl(controller, IOCTL_USB_GET_ROOT_HUB_NAME, nullptr, 0, buf.data(), static_cast<DWORD>(buf.size()), &returned, nullptr))
        return std::nullopt;
    return std::wstring(name->RootHubName);
}

struct DeviceInterfaceEntry {
    std::wstring path;
    std::string description;
};

std::vector<DeviceInterfaceEntry> enumerateInterfaces(const GUID& guid) {
    std::vector<DeviceInterfaceEntry> out;
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd)) break;
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;
        std::vector<std::uint8_t> buf(needed, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dev{};
        dev.cbSize = sizeof(dev);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, needed, nullptr, &dev)) continue;
        DeviceInterfaceEntry e;
        e.path = detail->DevicePath;
        wchar_t desc[512] = {};
        if (SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<PBYTE>(desc), sizeof(desc), nullptr))
            e.description = utf16ToUtf8(desc, static_cast<int>(wcslen(desc)));
        out.push_back(std::move(e));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// ---------------------------------------------------------------------------
// Décodage des descripteurs.
// ---------------------------------------------------------------------------
struct ParseContext {
    std::uint8_t ifClass = 0;
    std::uint8_t ifSubClass = 0;
    std::uint8_t ifProtocol = 0;
    std::uint8_t ifNumber = 0;
    std::uint8_t ifAlt = 0;
    std::vector<std::uint8_t> hidInterfaces; // numéros d'interface HID rencontrés
};

void printDeviceDescriptor(Printer& p, const USB_DEVICE_DESCRIPTOR& d, const std::string& ind) {
    p.fmt("{}bcdUSB             : {:X}.{:02X}", ind, d.bcdUSB >> 8, d.bcdUSB & 0xFF);
    p.fmt("{}bDeviceClass       : 0x{:02X} / 0x{:02X} / 0x{:02X}{}", ind, d.bDeviceClass, d.bDeviceSubClass, d.bDeviceProtocol,
          d.bDeviceClass == 0 ? "  (classe définie par interface)" : d.bDeviceClass == 0xEF ? "  (Miscellaneous, IAD)" : "");
    p.fmt("{}bMaxPacketSize0    : {}", ind, d.bMaxPacketSize0);
    p.fmt("{}idVendor           : 0x{:04X}", ind, d.idVendor);
    p.fmt("{}idProduct          : 0x{:04X}", ind, d.idProduct);
    p.fmt("{}bcdDevice          : {:X}.{:02X}", ind, d.bcdDevice >> 8, d.bcdDevice & 0xFF);
    p.fmt("{}iManufacturer      : {}", ind, d.iManufacturer);
    p.fmt("{}iProduct           : {}", ind, d.iProduct);
    p.fmt("{}iSerialNumber      : {}", ind, d.iSerialNumber);
    p.fmt("{}bNumConfigurations : {}", ind, d.bNumConfigurations);
}

void printAudioControlCs(Printer& p, const std::uint8_t* d, std::size_t len, const std::string& ind, bool uac2) {
    const std::uint8_t sub = len > 2 ? d[2] : 0;
    if (!uac2) {
        p.fmt("{}CS_INTERFACE (AudioControl UAC1) sous-type 0x{:02X} : non décodé en détail", ind, sub);
        p.line(hexDump(d, len, ind + "  "));
        return;
    }
    static const char* inTermCtrl[] = {"CopyProtect", "Connector", "Overload", "Cluster", "Underflow", "Overflow"};
    static const char* outTermCtrl[] = {"CopyProtect", "Connector", "Overload", "Underflow", "Overflow"};
    static const char* fuCtrl[] = {"Mute", "Volume", "Bass", "Mid", "Treble", "GraphicEQ", "AGC", "Delay", "BassBoost",
                                   "Loudness", "InputGain", "InputGainPad", "PhaseInverter", "Underflow", "Overflow"};
    static const char* clkCtrl[] = {"Frequency", "Validity"};
    switch (sub) {
    case 0x01:
        if (len >= 9) {
            p.fmt("{}AC HEADER : bcdADC={:X}.{:02X} bCategory=0x{:02X} wTotalLength={} bmControls=0x{:02X} (Latency={})",
                  ind, d[4], d[3], d[5], le16(d + 6), d[8], twoBitControl(d[8]));
        }
        break;
    case 0x02:
        if (len >= 17) {
            p.fmt("{}INPUT_TERMINAL : bTerminalID={} wTerminalType=0x{:04X} ({}) bAssocTerminal={} bCSourceID={}",
                  ind, d[3], le16(d + 4), terminalTypeName(le16(d + 4)), d[6], d[7]);
            p.fmt("{}  bNrChannels={} bmChannelConfig=0x{:08X} [{}] iChannelNames={} iTerminal={}",
                  ind, d[8], le32(d + 9), channelConfigText(le32(d + 9)), d[13], d[16]);
            p.fmt("{}  bmControls=0x{:04X} : {}", ind, le16(d + 14), controlsText(le16(d + 14), inTermCtrl, 6));
        }
        break;
    case 0x03:
        if (len >= 12) {
            p.fmt("{}OUTPUT_TERMINAL : bTerminalID={} wTerminalType=0x{:04X} ({}) bAssocTerminal={} bSourceID={} bCSourceID={} iTerminal={}",
                  ind, d[3], le16(d + 4), terminalTypeName(le16(d + 4)), d[6], d[7], d[8], d[11]);
            p.fmt("{}  bmControls=0x{:04X} : {}", ind, le16(d + 9), controlsText(le16(d + 9), outTermCtrl, 5));
        }
        break;
    case 0x04:
        if (len >= 5) {
            const std::uint8_t pins = d[4];
            std::string src;
            for (std::uint8_t i = 0; i < pins && 5u + i < len; ++i) src += std::format("{} ", d[5 + i]);
            p.fmt("{}MIXER_UNIT : bUnitID={} bNrInPins={} baSourceID=[{}]", ind, d[3], pins, src);
            const std::size_t o = 5 + pins;
            if (o + 6 <= len) {
                p.fmt("{}  bNrChannels={} bmChannelConfig=0x{:08X} [{}] iChannelNames={}", ind, d[o], le32(d + o + 1),
                      channelConfigText(le32(d + o + 1)), d[o + 5]);
            }
        }
        break;
    case 0x05:
        if (len >= 5) {
            const std::uint8_t pins = d[4];
            std::string src;
            for (std::uint8_t i = 0; i < pins && 5u + i < len; ++i) src += std::format("{} ", d[5 + i]);
            const std::size_t o = 5 + pins;
            p.fmt("{}SELECTOR_UNIT : bUnitID={} bNrInPins={} baSourceID=[{}] bmControls=0x{:02X} (Selector={}) iSelector={}",
                  ind, d[3], pins, src, o < len ? d[o] : 0, twoBitControl(o < len ? d[o] : 0), o + 1 < len ? d[o + 1] : 0);
        }
        break;
    case 0x06:
        if (len >= 6) {
            const std::size_t channels = (len - 6) / 4;
            p.fmt("{}FEATURE_UNIT : bUnitID={} bSourceID={} iFeature={} ({} jeux de contrôles : maître + {} canaux)",
                  ind, d[3], d[4], d[len - 1], channels, channels > 0 ? channels - 1 : 0);
            for (std::size_t c = 0; c < channels; ++c) {
                const std::uint32_t bm = le32(d + 5 + 4 * c);
                p.fmt("{}  bmaControls[{}]{} = 0x{:08X} : {}", ind, c, c == 0 ? " (maître)" : "", bm, controlsText(bm, fuCtrl, 15));
            }
        }
        break;
    case 0x07: p.fmt("{}EFFECT_UNIT : bUnitID={} (détail non décodé)", ind, len > 3 ? d[3] : 0); p.line(hexDump(d, len, ind + "  ")); break;
    case 0x08: p.fmt("{}PROCESSING_UNIT : bUnitID={} wProcessType=0x{:04X} (détail non décodé)", ind, len > 3 ? d[3] : 0, len >= 6 ? le16(d + 4) : 0); p.line(hexDump(d, len, ind + "  ")); break;
    case 0x09: p.fmt("{}EXTENSION_UNIT : bUnitID={} wExtensionCode=0x{:04X} (détail non décodé)", ind, len > 3 ? d[3] : 0, len >= 6 ? le16(d + 4) : 0); p.line(hexDump(d, len, ind + "  ")); break;
    case 0x0A:
        if (len >= 8) {
            const std::uint8_t attr = d[4];
            const char* type = (attr & 3) == 0 ? "horloge externe" : (attr & 3) == 1 ? "interne fixe" : (attr & 3) == 2 ? "interne variable" : "interne programmable";
            p.fmt("{}CLOCK_SOURCE : bClockID={} bmAttributes=0x{:02X} ({}{}) bmControls=0x{:02X} : {} bAssocTerminal={} iClockSource={}",
                  ind, d[3], attr, type, (attr & 4) ? ", synchronisée SOF" : "", d[5], controlsText(d[5], clkCtrl, 2), d[6], d[7]);
        }
        break;
    case 0x0B:
        if (len >= 5) {
            const std::uint8_t pins = d[4];
            std::string src;
            for (std::uint8_t i = 0; i < pins && 5u + i < len; ++i) src += std::format("{} ", d[5 + i]);
            const std::size_t o = 5 + pins;
            p.fmt("{}CLOCK_SELECTOR : bClockID={} bNrInPins={} baCSourceID=[{}] bmControls=0x{:02X} (Selector={}) iClockSelector={}",
                  ind, d[3], pins, src, o < len ? d[o] : 0, twoBitControl(o < len ? d[o] : 0), o + 1 < len ? d[o + 1] : 0);
        }
        break;
    case 0x0C:
        if (len >= 7) {
            p.fmt("{}CLOCK_MULTIPLIER : bClockID={} bCSourceID={} bmControls=0x{:02X} iClockMultiplier={}", ind, d[3], d[4], d[5], d[6]);
        }
        break;
    case 0x0D: p.fmt("{}SAMPLE_RATE_CONVERTER : bUnitID={}", ind, len > 3 ? d[3] : 0); p.line(hexDump(d, len, ind + "  ")); break;
    default:
        p.fmt("{}CS_INTERFACE (AudioControl) sous-type inconnu 0x{:02X}", ind, sub);
        p.line(hexDump(d, len, ind + "  "));
        break;
    }
}

void printAudioStreamingCs(Printer& p, const std::uint8_t* d, std::size_t len, const std::string& ind, bool uac2) {
    const std::uint8_t sub = len > 2 ? d[2] : 0;
    if (!uac2) {
        p.fmt("{}CS_INTERFACE (AudioStreaming UAC1) sous-type 0x{:02X} : non décodé en détail", ind, sub);
        p.line(hexDump(d, len, ind + "  "));
        return;
    }
    static const char* asCtrl[] = {"ActiveAltSetting", "ValidAltSettings"};
    switch (sub) {
    case 0x01:
        if (len >= 16) {
            const std::uint8_t ft = d[5];
            p.fmt("{}AS_GENERAL : bTerminalLink={} bmControls=0x{:02X} : {} bFormatType={} ({})",
                  ind, d[3], d[4], controlsText(d[4], asCtrl, 2), ft,
                  ft == 1 ? "Type I PCM" : ft == 2 ? "Type II" : ft == 3 ? "Type III" : ft == 4 ? "Type IV" : "inconnu");
            p.fmt("{}  bmFormats=0x{:08X} [{}] bNrChannels={} bmChannelConfig=0x{:08X} [{}] iChannelNames={}",
                  ind, le32(d + 6), ft == 1 ? formatsType1Text(le32(d + 6)) : "(type non I)", d[10], le32(d + 11),
                  channelConfigText(le32(d + 11)), d[15]);
        }
        break;
    case 0x02:
        if (len >= 6 && d[3] == 1) {
            p.fmt("{}FORMAT_TYPE I : bSubslotSize={} octet(s) bBitResolution={} bits", ind, d[4], d[5]);
        } else {
            p.fmt("{}FORMAT_TYPE : bFormatType={} (détail non décodé)", ind, len > 3 ? d[3] : 0);
            p.line(hexDump(d, len, ind + "  "));
        }
        break;
    default:
        p.fmt("{}CS_INTERFACE (AudioStreaming) sous-type inconnu 0x{:02X}", ind, sub);
        p.line(hexDump(d, len, ind + "  "));
        break;
    }
}

void printEndpoint(Printer& p, const std::uint8_t* d, std::size_t len, const std::string& ind) {
    if (len < 7) return;
    const std::uint8_t addr = d[2];
    const std::uint8_t attr = d[3];
    const std::uint16_t maxPacket = le16(d + 4);
    const char* type = (attr & 3) == 0 ? "Control" : (attr & 3) == 1 ? "Isochronous" : (attr & 3) == 2 ? "Bulk" : "Interrupt";
    std::string extra;
    if ((attr & 3) == 1) {
        const std::uint8_t sync = (attr >> 2) & 3;
        const std::uint8_t usage = (attr >> 4) & 3;
        extra = std::format(", sync={}, usage={}",
                            sync == 0 ? "none" : sync == 1 ? "asynchronous" : sync == 2 ? "adaptive" : "synchronous",
                            usage == 0 ? "data" : usage == 1 ? "feedback" : usage == 2 ? "implicit feedback data" : "réservé");
    }
    p.fmt("{}ENDPOINT : 0x{:02X} ({} {}) type={}{} wMaxPacketSize={} (base {}, x{} par microtrame) bInterval={}",
          ind, addr, (addr & 0x80) ? "IN" : "OUT", addr & 0x0F, type, extra, maxPacket, maxPacket & 0x7FF,
          ((maxPacket >> 11) & 3) + 1, d[6]);
}

void printCsEndpoint(Printer& p, const std::uint8_t* d, std::size_t len, const std::string& ind) {
    if (len >= 8 && d[2] == 0x01) {
        static const char* epCtrl[] = {"Pitch", "DataOverrun", "DataUnderrun"};
        const char* units = d[5] == 0 ? "non défini" : d[5] == 1 ? "millisecondes" : d[5] == 2 ? "échantillons PCM" : "réservé";
        p.fmt("{}CS_ENDPOINT EP_GENERAL : bmAttributes=0x{:02X}{} bmControls=0x{:02X} : {} bLockDelayUnits={} ({}) wLockDelay={}",
              ind, d[3], (d[3] & 0x80) ? " (MaxPacketsOnly)" : "", d[4], controlsText(d[4], epCtrl, 3), d[5], units, le16(d + 6));
    } else {
        p.fmt("{}CS_ENDPOINT sous-type 0x{:02X}", ind, len > 2 ? d[2] : 0);
        p.line(hexDump(d, len, ind + "  "));
    }
}

void printHidDescriptor(Printer& p, const std::uint8_t* d, std::size_t len, const std::string& ind) {
    if (len < 9) return;
    p.fmt("{}HID DESCRIPTOR : bcdHID={:X}.{:02X} bCountryCode={} bNumDescriptors={}", ind, d[3], d[2], d[4], d[5]);
    for (std::size_t i = 6; i + 3 <= len; i += 3) {
        p.fmt("{}  bDescriptorType=0x{:02X} ({}) wDescriptorLength={}", ind, d[i],
              d[i] == 0x22 ? "Report" : d[i] == 0x23 ? "Physical" : "autre", le16(d + i + 1));
    }
}

// Décodage des items courts d'un descripteur de rapport HID.
void printHidReportDescriptor(Printer& p, const std::uint8_t* d, std::size_t len, const std::string& baseInd) {
    static const char* mainTags[16] = {"", "", "", "", "", "", "", "", "Input", "Output", "Collection", "Feature", "End Collection", "", "", ""};
    static const char* globalTags[16] = {"Usage Page", "Logical Minimum", "Logical Maximum", "Physical Minimum", "Physical Maximum",
                                         "Unit Exponent", "Unit", "Report Size", "Report ID", "Report Count", "Push", "Pop", "", "", "", ""};
    static const char* localTags[16] = {"Usage", "Usage Minimum", "Usage Maximum", "Designator Index", "Designator Minimum",
                                        "Designator Maximum", "", "String Index", "String Minimum", "String Maximum", "Delimiter", "", "", "", "", ""};
    static const char* collections[7] = {"Physical", "Application", "Logical", "Report", "Named Array", "Usage Switch", "Usage Modifier"};
    int depth = 0;
    std::uint16_t usagePage = 0;
    std::size_t i = 0;
    while (i < len) {
        const std::uint8_t prefix = d[i];
        if (prefix == 0xFE) { // item long
            const std::size_t dataSize = i + 1 < len ? d[i + 1] : 0;
            p.fmt("{}{}Item long (taille {})", baseInd, std::string(static_cast<std::size_t>(depth) * 2, ' '), dataSize);
            i += 3 + dataSize;
            continue;
        }
        std::size_t size = prefix & 3;
        if (size == 3) size = 4;
        const std::uint8_t type = (prefix >> 2) & 3;
        const std::uint8_t tag = prefix >> 4;
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < size && i + 1 + k < len; ++k) value |= static_cast<std::uint32_t>(d[i + 1 + k]) << (8 * k);
        std::string ind = baseInd + std::string(static_cast<std::size_t>(depth) * 2, ' ');
        if (type == 0) {
            if (tag == 0x0C) {
                depth = depth > 0 ? depth - 1 : 0;
                ind = baseInd + std::string(static_cast<std::size_t>(depth) * 2, ' ');
                p.fmt("{}End Collection", ind);
            } else if (tag == 0x0A) {
                p.fmt("{}Collection ({})", ind, value < 7 ? collections[value] : "vendor/réservé");
                ++depth;
            } else if (tag == 8 || tag == 9 || tag == 0x0B) {
                p.fmt("{}{} (0x{:02X}) : {}, {}, {}{}", ind, mainTags[tag], value,
                      (value & 1) ? "Constant" : "Data", (value & 2) ? "Variable" : "Array", (value & 4) ? "Relative" : "Absolute",
                      (value & 0x40) ? ", Null state" : "");
            } else {
                p.fmt("{}Main tag 0x{:X} = 0x{:X}", ind, tag, value);
            }
        } else if (type == 1) {
            if (tag == 0) {
                usagePage = static_cast<std::uint16_t>(value);
                p.fmt("{}Usage Page (0x{:02X} {})", ind, value, hidUsagePageName(usagePage));
            } else {
                p.fmt("{}{} ({})", ind, globalTags[tag][0] ? globalTags[tag] : "Global réservé", static_cast<std::int32_t>(value));
            }
        } else if (type == 2) {
            if (tag <= 2) {
                const char* name = hidUsageName(usagePage, static_cast<USAGE>(value));
                p.fmt("{}{} (0x{:02X}{}{})", ind, localTags[tag], value, name[0] ? " " : "", name);
            } else {
                p.fmt("{}{} ({})", ind, localTags[tag][0] ? localTags[tag] : "Local réservé", value);
            }
        } else {
            p.fmt("{}Item réservé 0x{:02X}", ind, prefix);
        }
        i += 1 + size;
    }
}

void printConfiguration(Printer& p, const std::vector<std::uint8_t>& cfg, ParseContext& ctx, const std::string& ind) {
    if (cfg.size() < 9) {
        p.fmt("{}Descripteur de configuration trop court ({} octets)", ind, cfg.size());
        return;
    }
    const std::uint8_t* c = cfg.data();
    p.fmt("{}CONFIGURATION : wTotalLength={} bNumInterfaces={} bConfigurationValue={} iConfiguration={} bmAttributes=0x{:02X} ({}{}) bMaxPower={} mA",
          ind, le16(c + 2), c[4], c[5], c[6], c[7], (c[7] & 0x40) ? "auto-alimenté" : "alimenté par le bus",
          (c[7] & 0x20) ? ", remote wakeup" : "", c[8] * 2);

    std::size_t off = c[0];
    const std::string ind2 = ind + "  ";
    const std::string ind3 = ind + "    ";
    while (off + 2 <= cfg.size()) {
        const std::uint8_t len = cfg[off];
        const std::uint8_t type = cfg[off + 1];
        if (len < 2 || off + len > cfg.size()) {
            p.fmt("{}Descripteur malformé à l'offset 0x{:04X} (bLength={})", ind2, off, len);
            break;
        }
        const std::uint8_t* d = cfg.data() + off;
        switch (type) {
        case 0x0B: // interface association
            if (len >= 8) {
                p.fmt("{}INTERFACE_ASSOCIATION : bFirstInterface={} bInterfaceCount={} bFunctionClass=0x{:02X}/0x{:02X}/0x{:02X} ({}) iFunction={}",
                      ind2, d[2], d[3], d[4], d[5], d[6], interfaceClassText(d[4], d[5], d[6]), d[7]);
            }
            break;
        case USB_INTERFACE_DESCRIPTOR_TYPE:
            if (len >= 9) {
                ctx.ifNumber = d[2];
                ctx.ifAlt = d[3];
                ctx.ifClass = d[5];
                ctx.ifSubClass = d[6];
                ctx.ifProtocol = d[7];
                p.fmt("{}INTERFACE {} altsetting {} : bNumEndpoints={} classe 0x{:02X}/0x{:02X}/0x{:02X} ({}) iInterface={}",
                      ind2, d[2], d[3], d[4], d[5], d[6], d[7], interfaceClassText(d[5], d[6], d[7]), d[8]);
                if (d[5] == 0x03) {
                    bool known = false;
                    for (auto n : ctx.hidInterfaces) known = known || n == d[2];
                    if (!known) ctx.hidInterfaces.push_back(d[2]);
                }
            }
            break;
        case USB_ENDPOINT_DESCRIPTOR_TYPE:
            printEndpoint(p, d, len, ind3);
            break;
        case 0x21:
            if (ctx.ifClass == 0x03) printHidDescriptor(p, d, len, ind3);
            else { p.fmt("{}Descripteur 0x21 hors interface HID", ind3); p.line(hexDump(d, len, ind3 + "  ")); }
            break;
        case 0x24:
            if (ctx.ifClass == 0x01 && ctx.ifSubClass == 0x01) printAudioControlCs(p, d, len, ind3, ctx.ifProtocol == 0x20);
            else if (ctx.ifClass == 0x01 && ctx.ifSubClass == 0x02) printAudioStreamingCs(p, d, len, ind3, ctx.ifProtocol == 0x20);
            else { p.fmt("{}CS_INTERFACE (classe 0x{:02X}) sous-type 0x{:02X}", ind3, ctx.ifClass, d[2]); p.line(hexDump(d, len, ind3 + "  ")); }
            break;
        case 0x25:
            if (ctx.ifClass == 0x01) printCsEndpoint(p, d, len, ind3);
            else { p.fmt("{}CS_ENDPOINT (classe 0x{:02X})", ind3, ctx.ifClass); p.line(hexDump(d, len, ind3 + "  ")); }
            break;
        case 0x30: // SuperSpeed endpoint companion
            if (len >= 6) p.fmt("{}SS_ENDPOINT_COMPANION : bMaxBurst={} bmAttributes=0x{:02X} wBytesPerInterval={}", ind3, d[2], d[3], le16(d + 4));
            break;
        default:
            p.fmt("{}Descripteur type 0x{:02X} longueur {}", ind2, type, len);
            p.line(hexDump(d, len, ind2 + "  "));
            break;
        }
        off += len;
    }
}

void printBos(Printer& p, const std::vector<std::uint8_t>& bos, const std::string& ind) {
    if (bos.size() < 5) return;
    p.fmt("{}BOS : wTotalLength={} bNumDeviceCaps={}", ind, le16(bos.data() + 2), bos[4]);
    std::size_t off = bos[0];
    while (off + 3 <= bos.size()) {
        const std::uint8_t len = bos[off];
        if (len < 3 || off + len > bos.size()) break;
        const std::uint8_t cap = bos[off + 2];
        const char* name = cap == 1 ? "Wireless USB" : cap == 2 ? "USB 2.0 Extension" : cap == 3 ? "SuperSpeed USB" : cap == 4 ? "Container ID" : cap == 5 ? "Platform" : "autre";
        p.fmt("{}  Device capability 0x{:02X} ({}) longueur {}", ind, cap, name, len);
        p.line(hexDump(bos.data() + off, len, ind + "    "));
        off += len;
    }
}

// ---------------------------------------------------------------------------
// Vidage complet d'un périphérique trouvé sur un port.
// ---------------------------------------------------------------------------
void dumpDevice(Printer& p, HANDLE hub, ULONG port, const USB_NODE_CONNECTION_INFORMATION_EX& info, const std::string& where) {
    const std::string ind = "  ";
    p.line("");
    p.line("================================================================================");
    p.fmt("PÉRIPHÉRIQUE CIBLE : {} port {}", where, port);
    p.line("================================================================================");
    p.fmt("{}État de connexion  : {}  Vitesse : {}  Adresse : {}  Configuration courante : {}  Pipes ouverts : {}",
          ind, info.ConnectionStatus == DeviceConnected ? "connecté" : "autre", speedText(info.Speed), info.DeviceAddress,
          info.CurrentConfigurationValue, info.NumberOfOpenPipes);
    p.line("");
    p.fmt("{}--- Descripteur de périphérique ---", ind);
    printDeviceDescriptor(p, info.DeviceDescriptor, ind);

    p.fmt("{}Chaîne iManufacturer : \"{}\"", ind, getStringDescriptor(hub, port, info.DeviceDescriptor.iManufacturer));
    p.fmt("{}Chaîne iProduct      : \"{}\"", ind, getStringDescriptor(hub, port, info.DeviceDescriptor.iProduct));
    if (info.DeviceDescriptor.iSerialNumber) {
        const std::string serial = getStringDescriptor(hub, port, info.DeviceDescriptor.iSerialNumber);
        p.fmt("{}Chaîne iSerialNumber : [rédigée, {} caractères]", ind, serial.size());
    } else {
        p.fmt("{}Chaîne iSerialNumber : absente", ind);
    }

    // Pipes ouverts : informations sur les endpoints effectivement actifs.
    for (ULONG i = 0; i < info.NumberOfOpenPipes && i < 30; ++i) {
        const auto& ep = info.PipeList[i].EndpointDescriptor;
        p.fmt("{}Pipe ouvert {} : endpoint 0x{:02X} attributs 0x{:02X} wMaxPacketSize={} bInterval={}", ind, i,
              ep.bEndpointAddress, ep.bmAttributes, ep.wMaxPacketSize, ep.bInterval);
    }

    for (std::uint8_t cfgIndex = 0; cfgIndex < info.DeviceDescriptor.bNumConfigurations; ++cfgIndex) {
        p.line("");
        p.fmt("{}--- Descripteur de configuration index {} ---", ind, cfgIndex);
        DWORD err = 0;
        auto head = getDescriptor(hub, port, 0x80, USB_CONFIGURATION_DESCRIPTOR_TYPE, cfgIndex, 0, 9, &err);
        if (!head || head->size() < 9) {
            p.fmt("{}Lecture impossible : {}", ind, lastErrorText(err));
            continue;
        }
        const std::uint16_t total = le16(head->data() + 2);
        auto full = getDescriptor(hub, port, 0x80, USB_CONFIGURATION_DESCRIPTOR_TYPE, cfgIndex, 0, total, &err);
        if (!full || full->size() < 9) {
            p.fmt("{}Lecture complète impossible ({} octets attendus) : {}", ind, total, lastErrorText(err));
            continue;
        }
        if (full->size() != total) p.fmt("{}Avertissement : {} octets reçus sur {} annoncés", ind, full->size(), total);

        ParseContext ctx;
        printConfiguration(p, *full, ctx, ind);

        p.line("");
        p.fmt("{}Dump hexadécimal complet de la configuration ({} octets) :", ind, full->size());
        p.line(hexDump(full->data(), full->size(), ind + "  "));

        // Descripteurs de rapport HID : requête destinée à l'interface (bmRequest 0x81).
        for (std::uint8_t ifNum : ctx.hidInterfaces) {
            p.line("");
            p.fmt("{}--- Descripteur de rapport HID, interface {} ---", ind, ifNum);
            DWORD hidErr = 0;
            auto rep = getDescriptor(hub, port, 0x81, 0x22, 0, ifNum, 1024, &hidErr);
            if (!rep || rep->empty()) {
                p.fmt("{}Non lisible via le hub : {}. Voir la section HID (hid.dll) ci-dessous.", ind, lastErrorText(hidErr));
            } else {
                p.fmt("{}{} octets reçus :", ind, rep->size());
                p.line(hexDump(rep->data(), rep->size(), ind + "  "));
                p.fmt("{}Décodage des items :", ind);
                printHidReportDescriptor(p, rep->data(), rep->size(), ind + "  ");
            }
        }
    }

    if (info.DeviceDescriptor.bcdUSB >= 0x0201) {
        p.line("");
        p.fmt("{}--- Descripteur BOS ---", ind);
        DWORD err = 0;
        auto head = getDescriptor(hub, port, 0x80, 0x0F, 0, 0, 5, &err);
        if (head && head->size() >= 5) {
            const std::uint16_t total = le16(head->data() + 2);
            auto full = getDescriptor(hub, port, 0x80, 0x0F, 0, 0, total, &err);
            if (full && full->size() >= 5) printBos(p, *full, ind);
            else p.fmt("{}Lecture complète impossible : {}", ind, lastErrorText(err));
        } else {
            p.fmt("{}Non disponible : {}", ind, lastErrorText(err));
        }
    }

    p.line("");
    p.fmt("{}Note : en USB Audio Class 2.0, les fréquences d'échantillonnage ne figurent pas dans les descripteurs.", ind);
    p.fmt("{}Elles s'obtiennent par une requête de classe RANGE sur le Clock Source, hors de portée de cet outil (lecture par hub).", ind);
}

// ---------------------------------------------------------------------------
// Parcours de l'arbre USB.
// ---------------------------------------------------------------------------
struct WalkState {
    const Options* opt = nullptr;
    Printer* p = nullptr;
    int found = 0;
    int devices = 0;
};

void walkHub(const std::wstring& hubPath, const std::string& label, int depth, WalkState& st) {
    HubHandle hub(hubPath);
    const std::string ind(static_cast<std::size_t>(depth) * 2, ' ');
    if (!hub.ok()) {
        st.p->fmt("{}{} : ouverture impossible ({})", ind, label, lastErrorText(GetLastError()));
        return;
    }
    USB_NODE_INFORMATION node{};
    DWORD returned = 0;
    if (!DeviceIoControl(hub.h, IOCTL_USB_GET_NODE_INFORMATION, &node, sizeof(node), &node, sizeof(node), &returned, nullptr)) {
        st.p->fmt("{}{} : IOCTL_USB_GET_NODE_INFORMATION a échoué ({})", ind, label, lastErrorText(GetLastError()));
        return;
    }
    const UCHAR ports = node.u.HubInformation.HubDescriptor.bNumberOfPorts;
    if (st.opt->all) st.p->fmt("{}{} : {} ports", ind, label, ports);

    const std::size_t size = sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + 30 * sizeof(USB_PIPE_INFO);
    for (ULONG port = 1; port <= ports; ++port) {
        std::vector<std::uint8_t> buf(size, 0);
        auto* info = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(buf.data());
        info->ConnectionIndex = port;
        if (!DeviceIoControl(hub.h, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, buf.data(), static_cast<DWORD>(size),
                             buf.data(), static_cast<DWORD>(size), &returned, nullptr)) {
            if (st.opt->all) st.p->fmt("{}  port {} : information indisponible ({})", ind, port, lastErrorText(GetLastError()));
            continue;
        }
        if (info->ConnectionStatus == NoDeviceConnected) continue;
        ++st.devices;

        const auto& dd = info->DeviceDescriptor;
        const bool match = dd.idVendor == st.opt->vid && dd.idProduct == st.opt->pid;
        if (st.opt->all) {
            const std::string product = info->ConnectionStatus == DeviceConnected ? getStringDescriptor(hub.h, port, dd.iProduct) : "";
            st.p->fmt("{}  port {} : VID 0x{:04X} PID 0x{:04X} {}{} \"{}\" classe 0x{:02X}{}", ind, port, dd.idVendor, dd.idProduct,
                      speedText(info->Speed), info->DeviceIsHub ? " [hub]" : "", product, dd.bDeviceClass, match ? "  <== CIBLE" : "");
        }
        if (match && info->ConnectionStatus == DeviceConnected) {
            ++st.found;
            dumpDevice(*st.p, hub.h, port, *info, label);
        }
        if (info->DeviceIsHub) {
            auto name = getConnectionName(hub.h, port);
            if (name) walkHub(L"\\\\.\\" + *name, std::format("{} / hub externe port {}", label, port), depth + 1, st);
        }
    }
}

// ---------------------------------------------------------------------------
// Section HID via hid.dll (aucune écriture : ouverture sans droit d'accès).
// ---------------------------------------------------------------------------
void dumpHidCollections(Printer& p, const Options& opt) {
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    const auto entries = enumerateInterfaces(hidGuid);
    int matched = 0;
    for (const auto& e : entries) {
        HANDLE h = CreateFileW(e.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{};
        attr.Size = sizeof(attr);
        if (!HidD_GetAttributes(h, &attr) || attr.VendorID != opt.vid || attr.ProductID != opt.pid) {
            CloseHandle(h);
            continue;
        }
        ++matched;
        p.line("");
        p.line("--------------------------------------------------------------------------------");
        p.fmt("COLLECTION HID {} : {}", matched, e.description);
        p.fmt("  Chemin : {}", utf16ToUtf8(e.path));
        p.fmt("  VID 0x{:04X} PID 0x{:04X} version 0x{:04X}", attr.VendorID, attr.ProductID, attr.VersionNumber);
        wchar_t text[256] = {};
        if (HidD_GetProductString(h, text, sizeof(text))) p.fmt("  Produit : \"{}\"", utf16ToUtf8(text, static_cast<int>(wcslen(text))));
        PHIDP_PREPARSED_DATA pre = nullptr;
        if (!HidD_GetPreparsedData(h, &pre)) {
            p.fmt("  HidD_GetPreparsedData a échoué ({})", lastErrorText(GetLastError()));
            CloseHandle(h);
            continue;
        }
        HIDP_CAPS caps{};
        if (HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS) {
            const char* uname = hidUsageName(caps.UsagePage, caps.Usage);
            p.fmt("  Usage top-level : page 0x{:02X} ({}) usage 0x{:02X}{}{}", caps.UsagePage, hidUsagePageName(caps.UsagePage),
                  caps.Usage, uname[0] ? " " : "", uname);
            p.fmt("  Tailles de rapport (octets, ID compris) : Input={} Output={} Feature={}", caps.InputReportByteLength,
                  caps.OutputReportByteLength, caps.FeatureReportByteLength);
            p.fmt("  Collections liées : {}  Nombre de data indices : Input={} Output={} Feature={}", caps.NumberLinkCollectionNodes,
                  caps.NumberInputDataIndices, caps.NumberOutputDataIndices, caps.NumberFeatureDataIndices);

            struct Kind { HIDP_REPORT_TYPE type; const char* name; USHORT buttons; USHORT values; };
            const Kind kinds[] = {
                {HidP_Input, "Input", caps.NumberInputButtonCaps, caps.NumberInputValueCaps},
                {HidP_Output, "Output", caps.NumberOutputButtonCaps, caps.NumberOutputValueCaps},
                {HidP_Feature, "Feature", caps.NumberFeatureButtonCaps, caps.NumberFeatureValueCaps},
            };
            for (const auto& k : kinds) {
                if (k.buttons) {
                    std::vector<HIDP_BUTTON_CAPS> bc(k.buttons);
                    USHORT n = k.buttons;
                    if (HidP_GetButtonCaps(k.type, bc.data(), &n, pre) == HIDP_STATUS_SUCCESS) {
                        for (USHORT i = 0; i < n; ++i) {
                            const auto& b = bc[i];
                            if (b.IsRange) {
                                p.fmt("  {} boutons : report ID {} page 0x{:02X} ({}) usages 0x{:02X}..0x{:02X}{}{} ({} bits de données)", k.name, b.ReportID,
                                      b.UsagePage, hidUsagePageName(b.UsagePage), b.Range.UsageMin, b.Range.UsageMax,
                                      hidUsageName(b.UsagePage, b.Range.UsageMin)[0] ? " dès " : "", hidUsageName(b.UsagePage, b.Range.UsageMin),
                                      b.Range.DataIndexMax - b.Range.DataIndexMin + 1);
                            } else {
                                const char* nm = hidUsageName(b.UsagePage, b.NotRange.Usage);
                                p.fmt("  {} bouton : report ID {} page 0x{:02X} ({}) usage 0x{:02X}{}{}", k.name, b.ReportID, b.UsagePage,
                                      hidUsagePageName(b.UsagePage), b.NotRange.Usage, nm[0] ? " " : "", nm);
                            }
                        }
                    }
                }
                if (k.values) {
                    std::vector<HIDP_VALUE_CAPS> vc(k.values);
                    USHORT n = k.values;
                    if (HidP_GetValueCaps(k.type, vc.data(), &n, pre) == HIDP_STATUS_SUCCESS) {
                        for (USHORT i = 0; i < n; ++i) {
                            const auto& v = vc[i];
                            std::string usage = v.IsRange ? std::format("usages 0x{:02X}..0x{:02X}", v.Range.UsageMin, v.Range.UsageMax)
                                                          : std::format("usage 0x{:02X} {}", v.NotRange.Usage, hidUsageName(v.UsagePage, v.NotRange.Usage));
                            p.fmt("  {} valeur : report ID {} page 0x{:02X} ({}) {} bits={} count={} logique [{}..{}]", k.name, v.ReportID, v.UsagePage,
                                  hidUsagePageName(v.UsagePage), usage, v.BitSize, v.ReportCount, v.LogicalMin, v.LogicalMax);
                        }
                    }
                }
            }
        } else {
            p.line("  HidP_GetCaps a échoué");
        }
        HidD_FreePreparsedData(pre);
        CloseHandle(h);
    }
    if (matched == 0) p.fmt("Aucune collection HID avec VID 0x{:04X} PID 0x{:04X}.", opt.vid, opt.pid);
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    auto parsed = parseArgs(argc, argv);
    if (!parsed) {
        std::fputs("Arguments invalides. Utilisez --help.\n", stderr);
        return 1;
    }
    const Options opt = *parsed;
    if (opt.help) {
        printHelp();
        return 0;
    }

    Printer p;
    p.line("usb-descriptor-dump : lecture seule des descripteurs USB (aucune écriture vers les périphériques)");
    p.fmt("Cible : VID 0x{:04X} PID 0x{:04X}{}", opt.vid, opt.pid, opt.all ? "  (mode --all : résumé de tous les périphériques)" : "");
    p.line("");

    const auto controllers = enumerateInterfaces(GUID_DEVINTERFACE_USB_HOST_CONTROLLER);
    if (controllers.empty()) {
        p.line("Aucun contrôleur hôte USB trouvé.");
        if (!opt.outPath.empty()) p.saveTo(opt.outPath);
        return 1;
    }

    WalkState st;
    st.opt = &opt;
    st.p = &p;
    int index = 0;
    for (const auto& c : controllers) {
        ++index;
        HANDLE hc = CreateFileW(c.path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hc == INVALID_HANDLE_VALUE) {
            p.fmt("Contrôleur {} ({}) : ouverture impossible ({})", index, c.description, lastErrorText(GetLastError()));
            continue;
        }
        auto root = getRootHubName(hc);
        CloseHandle(hc);
        if (!root) {
            p.fmt("Contrôleur {} ({}) : hub racine introuvable", index, c.description);
            continue;
        }
        if (opt.all) p.fmt("Contrôleur {} : {}", index, c.description);
        walkHub(L"\\\\.\\" + *root, std::format("contrôleur {} ({}) hub racine", index, c.description), opt.all ? 1 : 0, st);
    }

    p.line("");
    p.fmt("Périphériques USB présents parcourus : {}. Correspondances VID/PID cible : {}.", st.devices, st.found);

    if (opt.hid) {
        p.line("");
        p.line("################################################################################");
        p.line("SECTION HID (hid.dll, ouverture sans droit d'accès, lecture des capacités uniquement)");
        p.line("################################################################################");
        dumpHidCollections(p, opt);
    }

    if (!opt.outPath.empty()) {
        if (p.saveTo(opt.outPath)) std::printf("\nSortie écrite dans : %s\n", opt.outPath.c_str());
        else std::fprintf(stderr, "\nImpossible d'écrire le fichier : %s\n", opt.outPath.c_str());
    }
    return st.found > 0 ? 0 : 2;
}
