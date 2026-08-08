// Standalone stress harness for libremidi_flutter's Apple implementation.
//
// Drives the real C API (apple/Classes/libremidi_flutter.cpp) without Flutter,
// Dart or physical MIDI hardware. It creates its own virtual CoreMIDI source
// with MIDISourceCreate, pushes traffic into it with MIDIReceived, and hammers
// the teardown paths concurrently. Build it with -fsanitize=address (or
// thread) — the point is to make the sanitizer, not a human, decide whether the
// documented residual races actually trigger.
//
// Two scenarios:
//
//   midiin   Dense MIDI + large SysEx while an input is opened and closed in a
//            tight loop. Targets the window described in LrmMidiIn::~LrmMidiIn:
//            a delivery that starts while libremidi is closing the port.
//
//   observer A hotplug storm (virtual endpoints created and disposed) while
//            observers with hotplug callbacks are created and freed. Targets
//            the CoreMIDI run-loop teardown in LrmObserver::~LrmObserver and
//            the port-vector lifetime.
//
// Usage: ./stress_libremidi <midiin|observer|all> [seconds]

#include "libremidi_flutter.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_sent{0};
std::atomic<uint64_t> g_received{0};
std::atomic<uint64_t> g_bytes_received{0};
std::atomic<uint64_t> g_open_cycles{0};
std::atomic<uint64_t> g_open_failures{0};
std::atomic<uint64_t> g_observer_cycles{0};
// Every observer emits one setupChanged when its initial enumeration is done,
// so a total event count says nothing about whether a hotplug storm actually
// happened. Count device add/remove events separately.
std::atomic<uint64_t> g_hotplug_events{0};
std::atomic<uint64_t> g_hotplug_device_events{0};
std::atomic<uint64_t> g_endpoints_created{0};
std::atomic<uint64_t> g_endpoint_failures{0};
std::atomic<int> g_last_endpoint_error{0};

// Stands in for the Dart NativeCallable listener: read every byte (so a
// use-after-free on the handed-over buffer is an ASan read error, not a silent
// success) and return the allocation through lrm_free, exactly as the Dart side
// does in _onMidiMessage.
void on_midi_message(void* /*context*/, const uint8_t* data, size_t length,
                     int64_t /*timestamp*/) {
  volatile uint8_t checksum = 0;
  for (size_t i = 0; i < length; ++i) {
    checksum = static_cast<uint8_t>(checksum + data[i]);
  }
  (void)checksum;
  g_received.fetch_add(1, std::memory_order_relaxed);
  g_bytes_received.fetch_add(length, std::memory_order_relaxed);
  lrm_free(const_cast<uint8_t*>(data));
}

void on_hotplug(void* /*context*/, int32_t event_type) {
  g_hotplug_events.fetch_add(1, std::memory_order_relaxed);
  // 0..3 are input/output added/removed; 4 is the initial setupChanged.
  if (event_type >= 0 && event_type <= 3) {
    g_hotplug_device_events.fetch_add(1, std::memory_order_relaxed);
  }
}

// A virtual CoreMIDI source. To everyone else on the system this looks like a
// MIDI input port they can open — including our own observer.
class VirtualSource {
 public:
  bool create(const std::string& name) {
    name_ = name;
    CFStringRef client_name =
        CFStringCreateWithCString(nullptr, ("stress-" + name).c_str(),
                                  kCFStringEncodingUTF8);
    OSStatus status = MIDIClientCreate(client_name, nullptr, nullptr, &client_);
    CFRelease(client_name);
    if (status != noErr) {
      g_endpoint_failures.fetch_add(1, std::memory_order_relaxed);
      g_last_endpoint_error.store((int)status, std::memory_order_relaxed);
      return false;
    }

    CFStringRef endpoint_name =
        CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
    status = MIDISourceCreate(client_, endpoint_name, &endpoint_);
    CFRelease(endpoint_name);
    if (status != noErr) {
      g_endpoint_failures.fetch_add(1, std::memory_order_relaxed);
      g_last_endpoint_error.store((int)status, std::memory_order_relaxed);
      return false;
    }
    g_endpoints_created.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void destroy() {
    if (endpoint_) {
      MIDIEndpointDispose(endpoint_);
      endpoint_ = 0;
    }
    if (client_) {
      MIDIClientDispose(client_);
      client_ = 0;
    }
  }

  void send(const uint8_t* bytes, size_t length) const {
    if (!endpoint_) return;
    // Big enough for the largest SysEx this harness generates.
    std::vector<uint8_t> storage(length + 512);
    auto* list = reinterpret_cast<MIDIPacketList*>(storage.data());
    MIDIPacket* packet = MIDIPacketListInit(list);
    packet = MIDIPacketListAdd(list, storage.size(), packet, 0, length, bytes);
    if (!packet) return;  // did not fit — skip this one
    MIDIReceived(endpoint_, list);
    g_sent.fetch_add(1, std::memory_order_relaxed);
  }

  const std::string& name() const { return name_; }

 private:
  MIDIClientRef client_ = 0;
  MIDIEndpointRef endpoint_ = 0;
  std::string name_;
};

// Creates and disposes virtual endpoints on a single, long-lived MIDI client.
//
// An earlier version created a fresh MIDIClient per endpoint. CoreMIDI starts
// refusing that after a few dozen clients (984 of 1112 attempts failed in a
// 30s run), so the "hotplug storm" mostly did not happen and the scenario
// silently degraded into plain observer churn. One client, many endpoints.
class EndpointChurn {
 public:
  bool open() {
    OSStatus status =
        MIDIClientCreate(CFSTR("lrm_stress_churn"), nullptr, nullptr, &client_);
    if (status != noErr) {
      g_last_endpoint_error.store((int)status, std::memory_order_relaxed);
      return false;
    }
    return true;
  }

  void close() {
    if (client_) {
      MIDIClientDispose(client_);
      client_ = 0;
    }
  }

  // Publishes one endpoint, holds it, then removes it — one add and one remove
  // notification for every observer currently listening.
  void cycle(int generation, std::chrono::milliseconds visible) {
    const std::string name = "lrm_stress_hotplug_" + std::to_string(generation);
    CFStringRef cf =
        CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
    MIDIEndpointRef endpoint = 0;
    OSStatus status = MIDISourceCreate(client_, cf, &endpoint);
    CFRelease(cf);

    if (status != noErr || endpoint == 0) {
      g_endpoint_failures.fetch_add(1, std::memory_order_relaxed);
      g_last_endpoint_error.store((int)status, std::memory_order_relaxed);
      return;
    }
    g_endpoints_created.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(visible);
    MIDIEndpointDispose(endpoint);
  }

 private:
  MIDIClientRef client_ = 0;
};

// Finds our virtual source in the observer's input list and returns its
// port_id, or 0 if it has not shown up (yet).
uint64_t find_port_id(LrmObserver* observer, const std::string& name) {
  lrm_observer_refresh(observer);
  const int32_t count = lrm_observer_get_input_count(observer);
  LrmPortInfo info{};
  for (int32_t i = 0; i < count; ++i) {
    if (lrm_observer_get_input(observer, i, &info) != LRM_OK) continue;
    if (name == info.display_name || name == info.port_name) {
      return info.port_id;
    }
  }
  return 0;
}

void traffic_thread(const VirtualSource& source) {
  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> sysex_size(256, 2048);
  std::vector<uint8_t> sysex;
  uint8_t note = 0;

  while (!g_stop.load(std::memory_order_relaxed)) {
    // A burst of short channel messages...
    for (int i = 0; i < 32; ++i) {
      const uint8_t note_on[3] = {0x90, static_cast<uint8_t>(note & 0x7F), 100};
      const uint8_t note_off[3] = {0x80, static_cast<uint8_t>(note & 0x7F), 0};
      source.send(note_on, sizeof(note_on));
      source.send(note_off, sizeof(note_off));
      ++note;
    }

    // ...and one large SysEx, which is the case where the per-message malloc
    // in the native callback is expensive enough to widen the race window.
    const int payload = sysex_size(rng);
    sysex.clear();
    sysex.reserve(payload + 3);
    sysex.push_back(0xF0);
    sysex.push_back(0x7D);  // non-commercial / test manufacturer id
    for (int i = 0; i < payload; ++i) {
      sysex.push_back(static_cast<uint8_t>(i & 0x7F));
    }
    sysex.push_back(0xF7);
    source.send(sysex.data(), sysex.size());
  }
}

int run_midi_in_scenario(int seconds) {
  std::printf("[midiin] creating virtual source...\n");
  VirtualSource source;
  if (!source.create("lrm_stress_source")) return 1;

  // Give CoreMIDI a moment to publish the new endpoint.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  LrmObserver* observer = lrm_observer_new();
  if (!observer) {
    std::fprintf(stderr, "[midiin] lrm_observer_new failed\n");
    source.destroy();
    return 1;
  }

  // Be generous here: right after the observer scenario's hotplug storm,
  // CoreMIDI can take several seconds to publish a newly created endpoint.
  uint64_t port_id = 0;
  for (int attempt = 0; attempt < 100 && port_id == 0; ++attempt) {
    port_id = find_port_id(observer, source.name());
    if (port_id == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (port_id == 0) {
    std::fprintf(stderr,
                 "[midiin] our virtual source never appeared in the port list "
                 "within 10s — CoreMIDI may still be settling after a previous "
                 "run; retry this scenario on its own\n");
    lrm_observer_free(observer);
    source.destroy();
    return 1;
  }
  std::printf("[midiin] found port_id=%llu, running %ds...\n",
              (unsigned long long)port_id, seconds);

  std::thread traffic(traffic_thread, std::cref(source));

  // Open and close the input in a tight loop with jitter, so that close()
  // lands at every possible point relative to an in-flight delivery.
  std::mt19937 rng(99);
  std::uniform_int_distribution<int> hold_us(0, 3000);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    LrmMidiIn* input = lrm_midi_in_open_by_id(observer, port_id,
                                              on_midi_message, nullptr,
                                              /*sysex=*/true, /*timing=*/true,
                                              /*sensing=*/true);
    if (!input) {
      g_open_failures.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(hold_us(rng)));
    lrm_midi_in_close(input);
    g_open_cycles.fetch_add(1, std::memory_order_relaxed);
  }

  g_stop.store(true, std::memory_order_relaxed);
  traffic.join();
  lrm_observer_free(observer);
  source.destroy();

  std::printf(
      "[midiin] open/close cycles=%llu (failed opens=%llu), sent=%llu, "
      "delivered=%llu (%llu bytes)\n",
      (unsigned long long)g_open_cycles.load(),
      (unsigned long long)g_open_failures.load(),
      (unsigned long long)g_sent.load(),
      (unsigned long long)g_received.load(),
      (unsigned long long)g_bytes_received.load());
  return 0;
}

int run_observer_scenario(int seconds) {
  std::printf("[observer] running %ds of hotplug storm + observer churn...\n",
              seconds);
  g_stop.store(false, std::memory_order_relaxed);

  // Hotplug storm: endpoints appearing and disappearing produce real CoreMIDI
  // setup notifications, which is what the observer's dedicated run-loop
  // thread has to survive being torn down under.
  EndpointChurn churn_client;
  if (!churn_client.open()) {
    std::fprintf(stderr, "[observer] could not open the churn MIDI client\n");
    return 1;
  }
  std::thread churn([&churn_client] {
    int generation = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
      churn_client.cycle(generation++, std::chrono::milliseconds(20));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  // Hold-time distribution matters more than raw cycle count here. Observer
  // construction returns immediately and initialises on its own thread in a
  // few milliseconds; the interesting window is the handful of microseconds
  // between that initialisation finishing and the run loop being entered,
  // because a stop request landing exactly there is the one a naive
  // CFRunLoopStop loses. Most cycles therefore use microsecond jitter that
  // straddles the end of initialisation; the rest use longer holds to also
  // cover teardown of a fully settled observer.
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> hold_us(0, 8000);
  std::uniform_int_distribution<int> hold_ms(0, 60);
  std::uniform_int_distribution<int> pick(0, 9);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    // The hotplug constructor returns immediately and initialises on its own
    // thread; freeing it while that initialisation is still running is exactly
    // the case that used to hang forever.
    LrmObserver* observer = lrm_observer_new_with_callbacks(on_hotplug, nullptr);
    if (!observer) {
      std::fprintf(stderr, "[observer] lrm_observer_new_with_callbacks failed\n");
      break;
    }
    const int bucket = pick(rng);
    if (bucket < 7) {
      // Teardown race: dispose around the end of initialisation.
      std::this_thread::sleep_for(std::chrono::microseconds(hold_us(rng)));
    } else if (bucket < 9) {
      std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms(rng)));
    } else {
      // Long enough to actually outlive an endpoint appearing and disappearing,
      // so the run really does exercise hotplug delivery and not just churn.
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    lrm_observer_get_input_count(observer);
    lrm_observer_refresh(observer);
    lrm_observer_free(observer);
    g_observer_cycles.fetch_add(1, std::memory_order_relaxed);
  }

  g_stop.store(true, std::memory_order_relaxed);
  churn.join();
  churn_client.close();

  const uint64_t created = g_endpoints_created.load();
  const uint64_t failed = g_endpoint_failures.load();
  std::printf(
      "[observer] create/free cycles=%llu, endpoints created=%llu "
      "(failed=%llu, last error=%d), hotplug events=%llu of which device "
      "add/remove=%llu\n",
      (unsigned long long)g_observer_cycles.load(),
      (unsigned long long)created, (unsigned long long)failed,
      g_last_endpoint_error.load(), (unsigned long long)g_hotplug_events.load(),
      (unsigned long long)g_hotplug_device_events.load());

  // A run in which CoreMIDI refused to create endpoints exercised observer
  // teardown without any hotplug traffic — that is not the scenario this claims
  // to test, so do not report success for it. Without this check a completely
  // degraded run still printed "harness finished" and exited 0.
  if (created == 0) {
    std::fprintf(stderr,
                 "[observer] FAILED: no virtual endpoint could be created "
                 "(last error=%d) — the hotplug storm never happened, so this "
                 "run proves nothing\n",
                 g_last_endpoint_error.load());
    return 1;
  }
  if (g_hotplug_device_events.load() == 0) {
    std::fprintf(stderr,
                 "[observer] FAILED: endpoints were created but no add/remove "
                 "event ever reached an observer — hotplug delivery is broken "
                 "or the observers were too short-lived to see one\n");
    return 1;
  }
  return 0;
}


// ---------------------------------------------------------------------------
// sendtest: hotplug -> open by id -> send, end to end.
//
// Reproduces "start the app, plug the device in, press send" without USB
// hardware: a virtual CoreMIDI destination is published *after* the observer
// exists, so it arrives as a hotplug event exactly like a device being plugged
// in. Its read proc records what actually reaches the device, which is the part
// a return code of LRM_OK does not prove.
// ---------------------------------------------------------------------------

std::atomic<uint64_t> g_target_received_bytes{0};
std::atomic<uint64_t> g_target_received_packets{0};

void target_read_proc(const MIDIPacketList* list, void* /*ref*/, void* /*src*/)
{
  const MIDIPacket* packet = &list->packet[0];
  for (unsigned i = 0; i < list->numPackets; ++i)
  {
    g_target_received_packets.fetch_add(1, std::memory_order_relaxed);
    g_target_received_bytes.fetch_add(packet->length, std::memory_order_relaxed);
    packet = MIDIPacketNext(packet);
  }
}

int run_send_scenario(int /*seconds*/)
{
  std::printf("[sendtest] creating observer with hotplug (app start)...\n");
  LrmObserver* observer = lrm_observer_new_with_callbacks(on_hotplug, nullptr);
  if (!observer)
  {
    std::fprintf(stderr, "[sendtest] observer creation failed\n");
    return 1;
  }
  // Apple initialises on a background thread; give it the moment the app would.
  std::this_thread::sleep_for(std::chrono::seconds(1));

  std::printf("[sendtest] publishing a virtual destination (device plugged in)...\n");
  MIDIClientRef client = 0;
  MIDIEndpointRef destination = 0;
  if (MIDIClientCreate(CFSTR("lrm_send_client"), nullptr, nullptr, &client) != noErr)
  {
    std::fprintf(stderr, "[sendtest] MIDIClientCreate failed\n");
    lrm_observer_free(observer);
    return 1;
  }
  if (MIDIDestinationCreate(client, CFSTR("lrm_send_target"), target_read_proc, nullptr,
                            &destination)
      != noErr)
  {
    std::fprintf(stderr, "[sendtest] MIDIDestinationCreate failed\n");
    MIDIClientDispose(client);
    lrm_observer_free(observer);
    return 1;
  }

  // Wait for it to show up in the observer's output list, as the UI would after
  // a hotplug event.
  uint64_t port_id = 0;
  LrmPortInfo info{};
  for (int attempt = 0; attempt < 100 && port_id == 0; ++attempt)
  {
    lrm_observer_refresh(observer);
    const int32_t count = lrm_observer_get_output_count(observer);
    for (int32_t i = 0; i < count; ++i)
    {
      if (lrm_observer_get_output(observer, i, &info) != LRM_OK)
        continue;
      if (std::string("lrm_send_target") == info.display_name
          || std::string("lrm_send_target") == info.port_name)
      {
        port_id = info.port_id;
        break;
      }
    }
    if (port_id == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (port_id == 0)
  {
    std::fprintf(stderr,
                 "[sendtest] FAILED: the new destination never appeared in the "
                 "observer's output list (hotplug events seen: %llu)\n",
                 (unsigned long long)g_hotplug_events.load());
    MIDIEndpointDispose(destination);
    MIDIClientDispose(client);
    lrm_observer_free(observer);
    return 1;
  }
  std::printf("[sendtest] found port_id=%llu after %llu hotplug event(s)\n",
              (unsigned long long)port_id,
              (unsigned long long)g_hotplug_events.load());

  LrmMidiOut* out = lrm_midi_out_open_by_id(observer, port_id);
  if (!out)
  {
    std::fprintf(stderr, "[sendtest] FAILED: lrm_midi_out_open_by_id returned null\n");
    MIDIEndpointDispose(destination);
    MIDIClientDispose(client);
    lrm_observer_free(observer);
    return 1;
  }
  std::printf("[sendtest] opened; connected=%d\n", (int)lrm_midi_out_is_connected(out));

  const uint8_t note_on[3] = {0x90, 60, 100};
  const uint8_t note_off[3] = {0x80, 60, 0};
  int send_errors = 0;
  for (int i = 0; i < 10; ++i)
  {
    if (lrm_midi_out_send(out, note_on, sizeof(note_on)) != LRM_OK)
      ++send_errors;
    if (lrm_midi_out_send(out, note_off, sizeof(note_off)) != LRM_OK)
      ++send_errors;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  const uint64_t packets = g_target_received_packets.load();
  std::printf("[sendtest] send errors=%d, packets that reached the device=%llu (%llu bytes)\n",
              send_errors, (unsigned long long)packets,
              (unsigned long long)g_target_received_bytes.load());

  lrm_midi_out_close(out);
  MIDIEndpointDispose(destination);
  MIDIClientDispose(client);
  lrm_observer_free(observer);

  if (send_errors != 0 || packets == 0)
  {
    std::fprintf(stderr,
                 "[sendtest] FAILED: nothing arrived at the device\n");
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "all";
  const int seconds = argc > 2 ? std::atoi(argv[2]) : 20;

  std::printf("libremidi_flutter stress harness — version reported: %s\n",
              lrm_get_version());

  // Every scenario here talks to the real CoreMIDI server: it publishes virtual
  // endpoints, opens and closes ports in loops, and pushes traffic. On a
  // machine with MIDI hardware attached this has repeatedly left a connected
  // USB device needing a physical re-plug — including from the scenarios that
  // only publish a single endpoint, which I had wrongly assumed were harmless.
  //
  // So none of it runs by accident. Set the variable only on a machine whose
  // MIDI devices you are willing to re-plug.
  // Refuse outright while physical MIDI hardware is attached. The environment
  // variable only guards against starting this by accident; it does not make
  // the run safe. Driving CoreMIDI this hard has repeatedly left a connected
  // USB device needing a physical re-plug, so check instead of trusting
  // whoever set the variable to have unplugged everything first.
  LrmObserver* probe = lrm_observer_new();
  if (!probe) {
    // Fail closed. A CoreMIDI server that is already unhappy is exactly when
    // the observer cannot be created *and* when hardware would go undetected —
    // running anyway would hammer the case this check exists to prevent.
    std::fprintf(stderr,
                 "refusing to run: could not enumerate MIDI ports, so this "
                 "cannot confirm that no hardware is attached.\n");
    return 2;
  }
  {
    LrmPortInfo info{};
    std::string hardware;
    const int32_t ins = lrm_observer_get_input_count(probe);
    for (int32_t i = 0; i < ins; ++i) {
      if (lrm_observer_get_input(probe, i, &info) == LRM_OK && !info.is_virtual)
        hardware += std::string("\n    input:  ") + info.display_name;
    }
    const int32_t outs = lrm_observer_get_output_count(probe);
    for (int32_t i = 0; i < outs; ++i) {
      if (lrm_observer_get_output(probe, i, &info) == LRM_OK && !info.is_virtual)
        hardware += std::string("\n    output: ") + info.display_name;
    }
    lrm_observer_free(probe);

    if (!hardware.empty()) {
      std::fprintf(stderr,
                   "refusing to run: physical MIDI hardware is attached and this "
                   "harness can leave it needing a re-plug:%s\n\nUnplug it, or run "
                   "this on a machine with no MIDI hardware.\n",
                   hardware.c_str());
      return 2;
    }
  }

  if (!std::getenv("LRM_STRESS_ALLOW_MIDI_DISRUPTION")) {
    std::fprintf(stderr,
                 "refusing to run: this harness drives the live CoreMIDI server "
                 "and can disturb attached MIDI hardware until it is\n"
                 "physically re-plugged. Run it on a machine with no MIDI "
                 "devices connected:\n\n"
                 "    LRM_STRESS_ALLOW_MIDI_DISRUPTION=1 %s %s\n",
                 argv[0], mode.c_str());
    return 2;
  }

  int result = 0;
  if (mode == "midiin" || mode == "all") {
    result |= run_midi_in_scenario(seconds);
  }
  if (mode == "all") {
    // Let CoreMIDI settle: the scenarios create and destroy endpoints, and a
    // back-to-back start otherwise races the daemon's own bookkeeping.
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  if (mode == "observer" || mode == "all") {
    result |= run_observer_scenario(seconds);
  }
  if (mode == "sendtest" || mode == "all") {
    result |= run_send_scenario(seconds);
  }
  if (mode != "midiin" && mode != "observer" && mode != "sendtest"
      && mode != "all") {
    std::fprintf(stderr, "usage: %s <midiin|observer|sendtest|all> [seconds]\n", argv[0]);
    return 2;
  }

  std::printf("%s\n", result == 0 ? "harness finished" : "harness FAILED");
  return result;
}
