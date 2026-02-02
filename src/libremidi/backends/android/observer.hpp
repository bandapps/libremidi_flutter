// Patched version of libremidi/backends/android/observer.hpp
// Populates all port info fields (manufacturer, product, serial, etc.)
// Adds hotplug support via MidiManager.DeviceCallback

#pragma once
#include <libremidi/backends/android/helpers.hpp>
#include <libremidi/backends/android/config.hpp>
#include <libremidi/detail/observer.hpp>
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
  struct
      : libremidi::observer_configuration
      , libremidi::android::observer_configuration
  {
  } configuration;

  explicit observer(
      libremidi::observer_configuration&& conf, libremidi::android::observer_configuration aconf)
      : configuration{std::move(conf), std::move(aconf)}
  {
    if (!context::client_name.empty() && context::client_name != configuration.client_name)
    {
      LOGW("Android backend only supports one client name per process");
    }
    context::client_name = std::string(configuration.client_name);

    // Initialize hotplug if callbacks are set
    if (configuration.has_callbacks())
    {
      auto env = context::get_thread_env();
      if (env)
      {
        auto ctx = context::get_context(env);
        if (ctx)
        {
          midi_manager = context::get_midi_manager(env, ctx);
          if (midi_manager)
          {
            // Keep a global ref to the midi manager
            midi_manager = env->NewGlobalRef(midi_manager);

            // Register for device callbacks
            device_callback = context::register_device_callback(env, midi_manager, this);

            // Set up the static callback handlers
            android::set_hotplug_observer(this, &observer::on_device_added_static, &observer::on_device_removed_static);
          }
        }
      }

      // Notify of existing ports if requested
      if (configuration.notify_in_constructor)
      {
        if (configuration.input_added)
          for (auto& p : get_input_ports())
            configuration.input_added(p);

        if (configuration.output_added)
          for (auto& p : get_output_ports())
            configuration.output_added(p);
      }

      // Initialize port cache for diff-based notifications
      cached_inputs = get_input_ports();
      cached_outputs = get_output_ports();
    }
  }

  ~observer()
  {
    auto env = context::get_thread_env();
    if (env)
    {
      // Unregister device callback
      if (midi_manager && device_callback)
      {
        android::clear_hotplug_observer();
        context::unregister_device_callback(env, midi_manager, device_callback);
        device_callback = nullptr;
      }

      if (midi_manager)
      {
        env->DeleteGlobalRef(midi_manager);
        midi_manager = nullptr;
      }

      context::cleanup_devices(env);
    }
  }

  libremidi::API get_current_api() const noexcept override
  {
    return libremidi::API::ANDROID_AMIDI;
  }

  std::vector<libremidi::input_port> get_input_ports() const noexcept override
  {
    auto env = context::get_thread_env();
    if (!env)
      return {};

    auto ctx = context::get_context(env);
    if (!ctx)
      return {};

    context::refresh_midi_devices(env, ctx, false);

    std::vector<libremidi::input_port> ports;
    for (size_t i = 0; i < context::midi_ports.size(); ++i)
    {
      libremidi::input_port port;
      port.api = libremidi::API::ANDROID_AMIDI;
      port.port = i;

      // Populate all port info fields
      std::string name = context::port_name(env, i);
      std::string manufacturer = context::port_manufacturer(env, i);
      std::string product = context::port_product(env, i);
      std::string serial = context::port_serial(env, i);
      int android_type = context::port_type(env, i);

      // Map Android device type to libremidi transport_type
      // Android: TYPE_USB=1, TYPE_VIRTUAL=2, TYPE_BLUETOOTH=3
      libremidi::transport_type type = libremidi::transport_type::unknown;
      switch (android_type) {
        case 1: // TYPE_USB
          type = static_cast<libremidi::transport_type>(
              static_cast<int>(libremidi::transport_type::hardware) |
              static_cast<int>(libremidi::transport_type::usb));
          break;
        case 2: // TYPE_VIRTUAL
          type = libremidi::transport_type::software;
          break;
        case 3: // TYPE_BLUETOOTH
          type = static_cast<libremidi::transport_type>(
              static_cast<int>(libremidi::transport_type::hardware) |
              static_cast<int>(libremidi::transport_type::bluetooth));
          break;
      }

      // Use product as device_name if available (like macOS uses Model)
      // Otherwise fall back to name
      port.port_name = name;
      port.device_name = product.empty() ? name : product;
      port.display_name = name;
      port.manufacturer = manufacturer;
      port.product = product;
      port.serial = serial;
      port.type = type;

      ports.push_back(std::move(port));
    }

    return ports;
  }

  std::vector<libremidi::output_port> get_output_ports() const noexcept override
  {
    auto env = context::get_thread_env();
    if (!env)
      return {};

    auto ctx = context::get_context(env);
    if (!ctx)
      return {};

    context::refresh_midi_devices(env, ctx, true);

    std::vector<libremidi::output_port> ports;
    for (size_t i = 0; i < context::midi_ports.size(); ++i)
    {
      libremidi::output_port port;
      port.api = libremidi::API::ANDROID_AMIDI;
      port.port = i;

      // Populate all port info fields
      std::string name = context::port_name(env, i);
      std::string manufacturer = context::port_manufacturer(env, i);
      std::string product = context::port_product(env, i);
      std::string serial = context::port_serial(env, i);
      int android_type = context::port_type(env, i);

      // Map Android device type to libremidi transport_type
      // Android: TYPE_USB=1, TYPE_VIRTUAL=2, TYPE_BLUETOOTH=3
      libremidi::transport_type type = libremidi::transport_type::unknown;
      switch (android_type) {
        case 1: // TYPE_USB
          type = static_cast<libremidi::transport_type>(
              static_cast<int>(libremidi::transport_type::hardware) |
              static_cast<int>(libremidi::transport_type::usb));
          break;
        case 2: // TYPE_VIRTUAL
          type = libremidi::transport_type::software;
          break;
        case 3: // TYPE_BLUETOOTH
          type = static_cast<libremidi::transport_type>(
              static_cast<int>(libremidi::transport_type::hardware) |
              static_cast<int>(libremidi::transport_type::bluetooth));
          break;
      }

      // Use product as device_name if available (like macOS uses Model)
      // Otherwise fall back to name
      port.port_name = name;
      port.device_name = product.empty() ? name : product;
      port.display_name = name;
      port.manufacturer = manufacturer;
      port.product = product;
      port.serial = serial;
      port.type = type;

      ports.push_back(std::move(port));
    }

    return ports;
  }

private:
  jobject midi_manager = nullptr;
  jobject device_callback = nullptr;

  // Port cache for diff-based notifications
  std::vector<libremidi::input_port> cached_inputs;
  std::vector<libremidi::output_port> cached_outputs;

  // Generate unique key for port identification (indices are not stable on Android)
  template<typename PortType>
  static std::string port_key(const PortType& p) {
    return p.port_name + "|" + p.manufacturer + "|" + p.product + "|" + p.serial;
  }

  // Update cache and return only the newly added ports
  template<typename PortType>
  std::vector<PortType> diff_added(const std::vector<PortType>& current, std::vector<PortType>& cache) {
    std::vector<PortType> added;
    std::set<std::string> cached_keys;
    for (const auto& p : cache)
      cached_keys.insert(port_key(p));

    for (const auto& p : current) {
      if (cached_keys.find(port_key(p)) == cached_keys.end())
        added.push_back(p);
    }

    cache = current;
    return added;
  }

  // Update cache and return only the removed ports
  template<typename PortType>
  std::vector<PortType> diff_removed(const std::vector<PortType>& current, std::vector<PortType>& cache) {
    std::vector<PortType> removed;
    std::set<std::string> current_keys;
    for (const auto& p : current)
      current_keys.insert(port_key(p));

    for (const auto& p : cache) {
      if (current_keys.find(port_key(p)) == current_keys.end())
        removed.push_back(p);
    }

    cache = current;
    return removed;
  }

  // Static callbacks that forward to instance methods
  static void on_device_added_static(void* observer_ptr)
  {
    auto* self = static_cast<observer*>(observer_ptr);
    if (self)
      self->on_device_added();
  }

  static void on_device_removed_static(void* observer_ptr)
  {
    auto* self = static_cast<observer*>(observer_ptr);
    if (self)
      self->on_device_removed();
  }

  void on_device_added()
  {
    LOGI("on_device_added called");
    // Use diff to only notify about actually new ports
    if (configuration.input_added)
    {
      auto added = diff_added(get_input_ports(), cached_inputs);
      LOGI("  %zu new input ports", added.size());
      for (auto& p : added)
        configuration.input_added(p);
    }
    if (configuration.output_added)
    {
      auto added = diff_added(get_output_ports(), cached_outputs);
      LOGI("  %zu new output ports", added.size());
      for (auto& p : added)
        configuration.output_added(p);
    }
  }

  void on_device_removed()
  {
    LOGI("on_device_removed called");
    // Use diff to only notify about actually removed ports
    if (configuration.input_removed)
    {
      auto removed = diff_removed(get_input_ports(), cached_inputs);
      LOGI("  %zu removed input ports", removed.size());
      for (auto& p : removed)
        configuration.input_removed(p);
    }
    if (configuration.output_removed)
    {
      auto removed = diff_removed(get_output_ports(), cached_outputs);
      LOGI("  %zu removed output ports", removed.size());
      for (auto& p : removed)
        configuration.output_removed(p);
    }
  }
};
}
}
