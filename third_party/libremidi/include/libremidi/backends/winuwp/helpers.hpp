#pragma once
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#include <libremidi/detail/midi_api.hpp>

#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unknwn.h>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Midi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

// cfgmgr32 for extended device info (BusReportedDeviceDesc, etc.)
#include <devpropdef.h>
#include <cfgmgr32.h>

NAMESPACE_LIBREMIDI
{

// DEVPKEYs for cfgmgr32 (defined inline to avoid initguid.h issues)
inline const DEVPROPKEY k_DEVPKEY_Device_BusReportedDeviceDesc =
    {{0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};
inline const DEVPROPKEY k_DEVPKEY_Device_FriendlyName =
    {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

inline void winrt_init()
{
  // init_apartment should only be called on the threads we own.
  // Since we're the library we don't own the threads we are called from,
  // so we should not perform this initialization ourselves.
  // winrt::init_apartment();
}

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Devices::Midi;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Storage::Streams;

// Helper function to allow waiting for aynchronous operation completion
// from the thread in STA. The only benefit from it compared to the
// get() function from winrt is that we avoid an assertion if waiting
// from the STA thread.
template <typename T>
LIBREMIDI_STATIC auto get(T const& async)
{
  if (async.Status() != AsyncStatus::Completed)
  {
    slim_mutex m;
    slim_condition_variable cv;
    bool completed = false;

    async.Completed([&](auto&&, auto&&) {
      {
        slim_lock_guard const guard(m);
        completed = true;
      }

      cv.notify_one();
    });

    slim_lock_guard guard(m);
    cv.wait(m, [&] { return completed; });
  }

  return async.GetResults();
}

// ============================================================================
// Extended device info via cfgmgr32
// ============================================================================

struct winuwp_device_info
{
    std::string device_name;
    uint8_t transport_type{0};
};

// Get string property from device node
inline std::string cfgmgr_get_string_property(DEVINST devInst, const DEVPROPKEY* propKey)
{
    DEVPROPTYPE propType = 0;
    ULONG bufferSize = 0;

    if (CM_Get_DevNode_PropertyW(devInst, propKey, &propType, nullptr, &bufferSize, 0) != CR_BUFFER_SMALL)
        return "";
    if (bufferSize == 0 || bufferSize > 4096)
        return "";

    std::vector<BYTE> buffer(bufferSize);
    if (CM_Get_DevNode_PropertyW(devInst, propKey, &propType, buffer.data(), &bufferSize, 0) != CR_SUCCESS)
        return "";
    if (propType != DEVPROP_TYPE_STRING)
        return "";

    const wchar_t* wstr = reinterpret_cast<const wchar_t*>(buffer.data());
    if (!wstr || wstr[0] == L'\0')
        return "";

    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return "";

    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    return result;
}

// Get device instance ID string
inline std::string cfgmgr_get_instance_id(DEVINST devInst)
{
    if (devInst == 0)
        return "";

    ULONG bufferSize = 0;
    if (CM_Get_Device_ID_Size(&bufferSize, devInst, 0) != CR_SUCCESS || bufferSize == 0)
        return "";

    bufferSize++;
    std::vector<wchar_t> buffer(bufferSize);
    if (CM_Get_Device_IDW(devInst, buffer.data(), bufferSize, 0) != CR_SUCCESS)
        return "";

    int len = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return "";

    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, buffer.data(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

// Walk up device tree to find USB parent and get BusReportedDeviceDesc
inline winuwp_device_info cfgmgr_get_usb_parent_info(DEVINST devInst)
{
    winuwp_device_info info;
    DEVINST current = devInst;

    for (int depth = 0; depth < 10 && current != 0; depth++)
    {
        std::string instanceId = cfgmgr_get_instance_id(current);

        // USB device without interface suffix (MI_xx)?
        if (instanceId.find("USB\\VID_") == 0 && instanceId.find("&MI_") == std::string::npos)
        {
            info.device_name = cfgmgr_get_string_property(current, &k_DEVPKEY_Device_BusReportedDeviceDesc);
            if (info.device_name.empty())
                info.device_name = cfgmgr_get_string_property(current, &k_DEVPKEY_Device_FriendlyName);
            info.transport_type = transport_type::hardware | transport_type::usb;
            return info;
        }

        // Bluetooth device?
        if (instanceId.find("BTHENUM\\") == 0 || instanceId.find("BTH\\") == 0)
        {
            info.device_name = cfgmgr_get_string_property(current, &k_DEVPKEY_Device_FriendlyName);
            info.transport_type = transport_type::hardware | transport_type::bluetooth;
            return info;
        }

        // Move to parent
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, current, 0) != CR_SUCCESS)
            break;
        current = parent;
    }

    return info;
}

// Convert WinRT device ID to PnP instance ID
// WinRT: \\?\SWD#MMDEVAPI#MIDII_xxx#{guid}
// PnP:   SWD\MMDEVAPI\MIDII_xxx
inline std::wstring winrt_id_to_pnp_instance_id(const std::string& winrtId)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, winrtId.c_str(), -1, nullptr, 0);
    if (wlen <= 1)
        return {};

    std::wstring wide(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, winrtId.c_str(), -1, &wide[0], wlen);

    // Skip "\\?\" prefix
    size_t pos = 0;
    if (wide.length() > 4 && wide[0] == L'\\' && wide[1] == L'\\' && wide[2] == L'?' && wide[3] == L'\\')
        pos = 4;

    // Find end before "#{" (GUID suffix)
    size_t guidPos = wide.find(L"#{", pos);
    if (guidPos == std::wstring::npos)
        guidPos = wide.length();

    std::wstring result = wide.substr(pos, guidPos - pos);

    // Replace # with backslash
    for (size_t i = 0; i < result.length(); i++)
        if (result[i] == L'#')
            result[i] = L'\\';

    return result;
}

// Get device info from WinRT port ID
inline winuwp_device_info get_device_info_from_port_id(const std::string& portId)
{
    winuwp_device_info info;

    std::wstring instanceId = winrt_id_to_pnp_instance_id(portId);
    if (instanceId.empty())
        return info;

    DEVINST devInst = 0;
    if (CM_Locate_DevNodeW(&devInst, const_cast<wchar_t*>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return info;

    info = cfgmgr_get_usb_parent_info(devInst);

    // Check for software synth
    if (info.device_name.empty())
    {
        std::string instIdUtf8 = cfgmgr_get_instance_id(devInst);
        if (instIdUtf8.find("MICROSOFTGSWAVETABLESYNTH") != std::string::npos)
        {
            info.device_name = "Microsoft GS Wavetable Synth";
            info.transport_type = transport_type::software;
        }
    }

    return info;
}

}
