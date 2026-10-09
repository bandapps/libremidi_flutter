// Replaces upstream libremidi's backends/android/observer.hpp: fills in all
// port information and reports hotplug. See helpers.hpp for the Java side.

#pragma once
#include <libremidi/backends/android/config.hpp>
#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/detail/observer.hpp>

#include <mutex>
#include <set>

NAMESPACE_LIBREMIDI
{
namespace android
{
class observer final
    : public libremidi::observer_api
    , public error_handler
{
public:
  explicit observer(libremidi::observer_configuration&& conf, observer_configuration)
      : configuration{std::move(conf)}
  {
    if (!configuration.has_callbacks())
      return;

    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      cached_inputs = get_input_ports();
      cached_outputs = get_output_ports();
      if (configuration.notify_in_constructor)
      {
        if (configuration.input_added)
          for (const auto& p : cached_inputs)
            configuration.input_added(p);
        if (configuration.output_added)
          for (const auto& p : cached_outputs)
            configuration.output_added(p);
      }
    }
    hotplug_id = register_hotplug([this] { on_devices_changed(); });
  }

  ~observer()
  {
    // Waits for a running notification, so nothing below is touched afterwards.
    if (hotplug_id >= 0)
      unregister_hotplug(hotplug_id);
  }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  std::vector<libremidi::input_port> get_input_ports() const noexcept override
  {
    return to_ports<libremidi::input_port>(list_ports(true));
  }

  std::vector<libremidi::output_port> get_output_ports() const noexcept override
  {
    return to_ports<libremidi::output_port>(list_ports(false));
  }

private:
  template <typename Port>
  static std::vector<Port> to_ports(const std::vector<port_desc>& descs)
  {
    std::vector<Port> ports;
    ports.reserve(descs.size());
    for (const auto& d : descs)
    {
      Port port;
      port.api = libremidi::API::ANDROID_AMIDI;
      port.port = make_port_handle(d.device_id, d.port_index);

      // Virtual devices (MidiDeviceService apps) often declare only
      // manufacturer and product — Google's own sample does — which used to
      // leave the port named just " Port 1".
      std::string name = d.name;
      if (name.empty())
        name = d.product;
      if (name.empty())
        name = d.manufacturer;
      if (name.empty())
        name = "MIDI Device";
      // Port number distinguishes the ports of one device (1-based for display).
      name += " Port " + std::to_string(d.port_index + 1);

      port.port_name = name;
      port.display_name = name;
      port.device_name = d.product.empty() ? name : d.product;
      port.manufacturer = d.manufacturer;
      port.product = d.product;
      port.serial = d.serial;
      port.type = transport_for(d.type);
      ports.push_back(std::move(port));
    }
    return ports;
  }

  static libremidi::transport_type transport_for(int32_t android_type)
  {
    using t = libremidi::transport_type;
    switch (android_type)
    {
      case 1: // MidiDeviceInfo.TYPE_USB
        return static_cast<t>(static_cast<int>(t::hardware) | static_cast<int>(t::usb));
      case 2: // TYPE_VIRTUAL
        return t::software;
      case 3: // TYPE_BLUETOOTH
        return static_cast<t>(static_cast<int>(t::hardware) | static_cast<int>(t::bluetooth));
      default:
        return t::unknown;
    }
  }

  // Ports are matched by handle (device id + port index). Matching by name
  // merged identical devices, so removing one of two identical controllers
  // went unreported. A reconnected device has a new id and is reported as
  // removed and added, which is what happened.
  template <typename Port>
  static port_handle port_key(const Port& p)
  {
    return p.port;
  }

  template <typename Port, typename Callback>
  static void report_changes(
      std::vector<Port>& cache, std::vector<Port> current, const Callback& added,
      const Callback& removed)
  {
    std::set<port_handle> before, after;
    for (const auto& p : cache)
      before.insert(port_key(p));
    for (const auto& p : current)
      after.insert(port_key(p));

    if (removed)
      for (const auto& p : cache)
        if (!after.count(port_key(p)))
          removed(p);
    if (added)
      for (const auto& p : current)
        if (!before.count(port_key(p)))
          added(p);
    cache = std::move(current);
  }

  // Runs on the MIDI thread.
  void on_devices_changed()
  {
    std::lock_guard<std::mutex> lock(cache_mutex);
    report_changes(
        cached_inputs, get_input_ports(), configuration.input_added, configuration.input_removed);
    report_changes(
        cached_outputs, get_output_ports(), configuration.output_added,
        configuration.output_removed);
  }

  libremidi::observer_configuration configuration;
  std::mutex cache_mutex;
  std::vector<libremidi::input_port> cached_inputs;
  std::vector<libremidi::output_port> cached_outputs;
  int hotplug_id = -1;
};
}
}
