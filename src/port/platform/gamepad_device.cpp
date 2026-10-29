#include "gamepad_device.hpp"
#include "gamepad_simulation.hpp"

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <thread>
#include <vector>

namespace omega {
namespace {
struct SonyHandle {
    HANDLE file{INVALID_HANDLE_VALUE};
    SonyPad kind{};
    bool bluetooth{};
    bool writable{};
    DWORD input_length{};
    DWORD output_length{};
    std::string tag;   // " [simulated <alias>]" (132win12)
    ~SonyHandle(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
};

// Every present Sony controller gamepad collection (usage page 1, usage 4/5)
// whose device path is not in `known`. Paths that are not Sony gamepads are
// added to `known` so later scans skip them.
std::vector<std::pair<std::wstring,std::unique_ptr<SonyHandle>>> open_new_sony_pads(std::set<std::wstring>& known){
    std::vector<std::pair<std::wstring,std::unique_ptr<SonyHandle>>> opened;
    GUID hid_guid{};
    HidD_GetHidGuid(&hid_guid);
    const HDEVINFO set=SetupDiGetClassDevsW(&hid_guid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE)return opened;
    SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof iface;
    for(DWORD i=0;SetupDiEnumDeviceInterfaces(set,nullptr,&hid_guid,i,&iface);++i){
        DWORD size=0;
        SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&size,nullptr);
        if(size<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))continue;
        std::vector<std::uint8_t> storage(size);
        auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        detail->cbSize=sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,size,nullptr,nullptr))continue;
        const std::wstring path=detail->DevicePath;
        if(known.contains(path))continue;
        HANDLE file=CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        bool writable=file!=INVALID_HANDLE_VALUE;
        if(file==INVALID_HANDLE_VALUE)
            file=CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(file==INVALID_HANDLE_VALUE)continue;   // retried on the next scan (e.g. held exclusively)
        auto handle=std::make_unique<SonyHandle>();handle->file=file;handle->writable=writable;
        HIDD_ATTRIBUTES attributes{};attributes.Size=sizeof attributes;
        if(!HidD_GetAttributes(file,&attributes)){known.insert(path);continue;}
        const auto kind=sony_pad_kind(attributes.VendorID,attributes.ProductID);
        if(!kind){known.insert(path);continue;}
        PHIDP_PREPARSED_DATA preparsed{};
        if(!HidD_GetPreparsedData(file,&preparsed))continue;
        HIDP_CAPS caps{};
        const bool caps_ok=HidP_GetCaps(preparsed,&caps)==HIDP_STATUS_SUCCESS;
        HidD_FreePreparsedData(preparsed);
        if(!caps_ok)continue;
        if(caps.UsagePage!=1||(caps.Usage!=4&&caps.Usage!=5)||caps.InputReportByteLength<10){known.insert(path);continue;}
        handle->kind=*kind;
        handle->input_length=caps.InputReportByteLength;
        handle->output_length=caps.OutputReportByteLength;
        // USB DualShock 4 / DualSense collections have 64-byte input reports;
        // Bluetooth ones are larger (547 / 78).
        handle->bluetooth=*kind!=SonyPad::dualshock3&&caps.InputReportByteLength!=64;
        if(*kind==SonyPad::dualshock3&&caps.FeatureReportByteLength>0){
            // Feature report 0xf2 starts the DualShock 3's input reports over USB.
            std::vector<std::uint8_t> feature(caps.FeatureReportByteLength);feature[0]=0xf2;
            HidD_GetFeature(file,feature.data(),static_cast<ULONG>(feature.size()));
        }
        opened.emplace_back(path,std::move(handle));
    }
    SetupDiDestroyDeviceInfoList(set);
    return opened;
}

// Checkpoint 132win12 (port-only tests): the simulation's Sony pads (named pipes standing for HID device paths)
// not in `known`, as open_new_sony_pads opens real ones.
std::vector<std::pair<std::wstring,std::unique_ptr<SonyHandle>>> open_simulated_sony_pads(GamepadSimulation& sim,const std::set<std::wstring>& known){
    std::vector<std::pair<std::wstring,std::unique_ptr<SonyHandle>>> opened;
    for(const auto& d:sim.sony_devices()){
        if(known.contains(d.path))continue;
        const HANDLE file=CreateFileW(d.path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(file==INVALID_HANDLE_VALUE)continue;
        DWORD mode=PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(file,&mode,nullptr,nullptr);
        auto handle=std::make_unique<SonyHandle>();
        handle->file=file;handle->writable=true;handle->kind=d.kind;handle->bluetooth=d.bluetooth;
        handle->input_length=static_cast<DWORD>(d.input_length);handle->output_length=static_cast<DWORD>(d.output_length);
        handle->tag=" [simulated "+d.alias+"]";
        opened.emplace_back(d.path,std::move(handle));
    }
    return opened;
}

// A press or a stick pushed well off centre: marks the pad the player is using,
// so an idle (e.g. virtual ViGEm) pad listed first does not shadow the real one.
bool engaged(const Ds2Pad& pad){
    const auto off=[](std::uint8_t v){return v<0x80-0x40||v>0x80+0x40;};
    return pad.held!=0||off(pad.left_x)||off(pad.left_y)||off(pad.right_x)||off(pad.right_y);
}

struct XInputState {DWORD packet;XInputGamepad pad;};
static_assert(sizeof(XInputGamepad)==12,"XINPUT_GAMEPAD layout");
static_assert(sizeof(XInputVibration)==4,"XINPUT_VIBRATION layout");
}  // namespace

struct GamepadDevice::Impl {
    // One open Sony pad with its own reader thread.
    struct SonyPadSlot {
        std::wstring path;
        std::unique_ptr<SonyHandle> handle;
        std::optional<Reading> reading;                       // guarded by Impl::mutex
        std::chrono::steady_clock::time_point last_engaged{}; // guarded by Impl::mutex
        std::uint64_t order{};
        std::atomic<bool> done{false};
        HANDLE vibration_changed{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
        std::thread reader;
        ~SonyPadSlot(){if(reader.joinable())reader.join();CloseHandle(vibration_changed);}
    };

    std::mutex mutex;
    std::vector<std::unique_ptr<SonyPadSlot>> pads;   // guarded by mutex
    const SonyPadSlot* active{};                      // guarded by mutex: the pad read() returns
    HANDLE stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::uint16_t vibration{};   // small << 8 | large (guarded by mutex)
    std::uint64_t next_order{};
    std::thread scanner;

    using GetState=DWORD(WINAPI*)(DWORD,XInputState*);
    using SetState=DWORD(WINAPI*)(DWORD,XInputVibration*);
    GetState xinput_get{};
    SetState xinput_set{};
    int xinput_slot{-1};
    std::optional<std::uint16_t> xinput_vibration_sent;
    std::chrono::steady_clock::time_point xinput_next_scan{};

    GamepadSimulation* sim{};   // 132win12 (port-only tests)

    explicit Impl(GamepadSimulation* simulation):sim(simulation){
        if(!sim)for(const auto* name:{L"xinput1_4.dll",L"xinput9_1_0.dll",L"xinput1_3.dll"})
            if(const auto module=LoadLibraryW(name)){
                xinput_get=reinterpret_cast<GetState>(GetProcAddress(module,"XInputGetState"));
                xinput_set=reinterpret_cast<SetState>(GetProcAddress(module,"XInputSetState"));
                if(xinput_get)break;
            }
        scanner=std::thread([this]{scan();});
    }
    ~Impl(){
        SetEvent(stop);
        if(scanner.joinable())scanner.join();
        {std::lock_guard lock(mutex);active=nullptr;}
        pads.clear();   // joins the readers (they watch `stop`)
        if(xinput_slot>=0&&xinput_vibration_sent.value_or(0)!=0){XInputVibration off{};set_xinput(static_cast<DWORD>(xinput_slot),off);}
        CloseHandle(stop);
    }

    // Opens newly connected Sony pads and drops lost ones, once a second.
    void scan(){
        std::set<std::wstring> rejected;
        while(WaitForSingleObject(stop,0)!=WAIT_OBJECT_0){
            std::set<std::wstring> known=rejected;
            {
                std::lock_guard lock(mutex);
                std::erase_if(pads,[&](const std::unique_ptr<SonyPadSlot>& p){
                    if(!p->done)return false;
                    if(active==p.get())active=nullptr;
                    return true;
                });
                for(const auto& p:pads)known.insert(p->path);
                if(!active)active=pick_active_locked();
            }
            // Paths added to `known` without being opened are not Sony gamepads. Checkpoint 132win12: only those
            // are rejected; the open pads' paths were rejected too before, so a pad re-plugged at the same device
            // path (the same USB port, a Bluetooth re-pair) was never reopened.
            const std::set<std::wstring> before=known;
            auto opened=sim?open_simulated_sony_pads(*sim,known):open_new_sony_pads(known);
            for(const auto& path:known)if(!before.contains(path))rejected.insert(path);
            for(auto& [path,handle]:opened){
                auto slot=std::make_unique<SonyPadSlot>();
                slot->path=path;slot->handle=std::move(handle);
                SonyPadSlot* raw=slot.get();
                {
                    std::lock_guard lock(mutex);
                    slot->order=next_order++;
                    pads.push_back(std::move(slot));
                }
                raw->reader=std::thread([this,raw]{read_until_lost(*raw);raw->done=true;if(sim)sim->device_lost(raw->path);});
            }
            // Once a second; a simulated connection (132win12) wakes the scan at once.
            const HANDLE waits[2]{stop,sim?static_cast<HANDLE>(sim->rescan_event()):nullptr};
            WaitForMultipleObjects(sim?2:1,waits,FALSE,1000);
        }
    }

    // The most recently engaged pad, else the first opened one.
    const SonyPadSlot* pick_active_locked() const{
        const SonyPadSlot* best{};
        for(const auto& p:pads){
            if(p->done||!p->reading)continue;
            if(!best||p->last_engaged>best->last_engaged||(p->last_engaged==best->last_engaged&&p->order<best->order))best=p.get();
        }
        return best;
    }

    void read_until_lost(SonyPadSlot& slot){
        SonyHandle& pad=*slot.handle;
        const std::string name=std::string(sony_pad_name(pad.kind))+(pad.bluetooth?" (Bluetooth":" (USB")+
            (pad.writable?")":", vibration unavailable)")+pad.tag;
        std::vector<std::uint8_t> buffer(pad.input_length);
        // Bluetooth pads in their simple mode report only on change: an
        // opened pad reads as connected and released until the first report.
        {std::lock_guard lock(mutex);slot.reading=Reading{Ds2Pad{},name};if(!active)active=&slot;}
        if(sim)sim->device_opened(slot.path);
        OVERLAPPED overlapped{};overlapped.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        OVERLAPPED written{};written.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        // A reconnected pad takes the current motor state.
        SetEvent(slot.vibration_changed);
        for(bool lost=false;!lost;){
            ResetEvent(overlapped.hEvent);
            if(!ReadFile(pad.file,buffer.data(),static_cast<DWORD>(buffer.size()),nullptr,&overlapped)&&GetLastError()!=ERROR_IO_PENDING)break;
            // Wired Sony pads continuously queue input reports. Give a pending motor update priority over the
            // completed read or the input event can win every wait and starve vibration indefinitely.
            const HANDLE waits[3]{stop,slot.vibration_changed,overlapped.hEvent};
            DWORD length=0;
            for(;;){
                const DWORD woke=WaitForMultipleObjects(3,waits,FALSE,INFINITE);
                if(woke==WAIT_OBJECT_0+1){write_vibration(slot,written,false);continue;}
                if(woke==WAIT_OBJECT_0){
                    CancelIoEx(pad.file,&overlapped);
                    GetOverlappedResult(pad.file,&overlapped,&length,TRUE);
                    write_vibration(slot,written,true);   // the motors off on exit
                    lost=true;
                }
                break;
            }
            if(lost)break;
            if(!GetOverlappedResult(pad.file,&overlapped,&length,FALSE))break;
            if(const auto state=ds2_from_sony_report(pad.kind,pad.bluetooth,std::span(buffer.data(),length))){
                const SonyPadSlot* was{};
                const SonyPadSlot* now{};
                {
                    std::lock_guard lock(mutex);
                    slot.reading=Reading{*state,name};
                    if(engaged(*state)){
                        slot.last_engaged=std::chrono::steady_clock::now();
                        was=active;active=&slot;now=active;
                    }
                }
                // The motors follow the active pad: stop the old one, start this one.
                if(was&&was!=now){
                    std::lock_guard lock(mutex);
                    for(const auto& p:pads)if(p.get()==was&&!p->done)SetEvent(p->vibration_changed);
                    SetEvent(slot.vibration_changed);
                }
                if(sim)sim->device_report(slot.path);
            }
        }
        {std::lock_guard lock(mutex);slot.reading.reset();if(active==&slot){active=nullptr;active=pick_active_locked();}}
        CloseHandle(written.hEvent);
        CloseHandle(overlapped.hEvent);
    }

    void write_vibration(SonyPadSlot& slot,OVERLAPPED& written,bool off){
        SonyHandle& pad=*slot.handle;
        if(!pad.writable)return;
        std::uint16_t v=0;
        if(!off){std::lock_guard lock(mutex);if(active==&slot)v=vibration;}
        auto report=sony_vibration_report(pad.kind,pad.bluetooth,pad.output_length,(v>>8)!=0,static_cast<std::uint8_t>(v));
        if(report.empty())return;
        ResetEvent(written.hEvent);
        if(!WriteFile(pad.file,report.data(),static_cast<DWORD>(report.size()),nullptr,&written)&&GetLastError()!=ERROR_IO_PENDING)return;
        DWORD done=0;
        if(WaitForSingleObject(written.hEvent,250)!=WAIT_OBJECT_0)CancelIoEx(pad.file,&written);
        GetOverlappedResult(pad.file,&written,&done,TRUE);
    }

    // XInputGetState / XInputSetState, or the simulation's table (132win12).
    DWORD get_xinput(DWORD slot,XInputState& state){
        if(sim){unsigned long packet=0;const auto r=sim->xinput_get(slot,state.pad,packet);state.packet=packet;return r;}
        return xinput_get?xinput_get(slot,&state):ERROR_DEVICE_NOT_CONNECTED;
    }
    DWORD set_xinput(DWORD slot,XInputVibration motors){
        if(sim)return sim->xinput_set(slot,motors);
        return xinput_set?xinput_set(slot,&motors):ERROR_DEVICE_NOT_CONNECTED;
    }
    std::string xinput_name(int slot) const{return "XInput pad "+std::to_string(slot)+(sim?" [simulated xinput"+std::to_string(slot)+"]":"");}

    std::optional<Reading> read_xinput(){
        if(!xinput_get&&!sim)return std::nullopt;
        XInputState state{};
        if(xinput_slot>=0){
            if(get_xinput(static_cast<DWORD>(xinput_slot),state)==ERROR_SUCCESS)
                return Reading{ds2_from_xinput(state.pad),xinput_name(xinput_slot)};
            xinput_slot=-1;
            xinput_vibration_sent.reset();   // 132win12: a reconnected pad takes the motors again
        }
        // Polling absent slots is slow; look for a newly connected pad once a second (the simulation: every read).
        const auto now=std::chrono::steady_clock::now();
        if(!sim&&now<xinput_next_scan)return std::nullopt;
        xinput_next_scan=now+std::chrono::seconds(1);
        for(DWORD slot=0;slot<4;++slot)
            if(get_xinput(slot,state)==ERROR_SUCCESS){
                xinput_slot=static_cast<int>(slot);
                return Reading{ds2_from_xinput(state.pad),xinput_name(xinput_slot)};
            }
        return std::nullopt;
    }
};

GamepadDevice::GamepadDevice(GamepadSimulation* simulation):impl_(std::make_unique<Impl>(simulation)){}
GamepadDevice::~GamepadDevice()=default;

std::optional<GamepadDevice::Reading> GamepadDevice::read(){
    {
        std::lock_guard lock(impl_->mutex);
        if(impl_->active&&impl_->active->reading)return impl_->active->reading;
    }
    return impl_->read_xinput();
}

void GamepadDevice::set_vibration(bool small_on,std::uint8_t large){
    const auto v=static_cast<std::uint16_t>((small_on?0x100U:0U)|large);
    bool sony=false;
    {
        std::lock_guard lock(impl_->mutex);
        sony=impl_->active!=nullptr;
        if(impl_->vibration!=v){
            impl_->vibration=v;
            for(const auto& p:impl_->pads)if(p.get()==impl_->active)SetEvent(p->vibration_changed);
        }
    }
    if(sony||(!impl_->xinput_set&&!impl_->sim)||impl_->xinput_slot<0){impl_->xinput_vibration_sent.reset();return;}
    if(impl_->xinput_vibration_sent==v)return;
    if(impl_->set_xinput(static_cast<DWORD>(impl_->xinput_slot),xinput_vibration(small_on,large))==ERROR_SUCCESS)impl_->xinput_vibration_sent=v;
}
}  // namespace omega
