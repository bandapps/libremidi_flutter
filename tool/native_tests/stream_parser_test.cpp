// Host test for the Android input's MIDI stream parser
// (src/libremidi/backends/android/stream_parser.hpp). Run: ./run.sh
//
// Each case is a byte stream cut into packets the way android.media.midi may
// deliver it, and the messages that must come out.

#include <libremidi/backends/android/stream_parser.hpp>

#include <cstdio>
#include <vector>

using bytes = std::vector<uint8_t>;

static int failures = 0;

static void check(const char* name, std::vector<bytes> packets, std::vector<bytes> expected)
{
  lrm::midi_stream_parser parser;
  std::vector<bytes> got;
  for (const auto& p : packets)
    parser.feed(p.data(), p.size(), [&](const uint8_t* d, size_t n) { got.emplace_back(d, d + n); });
  if (got != expected)
  {
    ++failures;
    std::printf("FAIL %s\n  got:", name);
    for (auto& m : got)
    {
      std::printf(" [");
      for (auto b : m) std::printf(" %02X", b);
      std::printf(" ]");
    }
    std::printf("\n");
  }
  else
  {
    std::printf("PASS %s\n", name);
  }
}

int main()
{
  check("whole message", {{0x90, 0x3C, 0x64}}, {{0x90, 0x3C, 0x64}});
  check("status and data in separate packets", {{0x90}, {0x3C, 0x64}}, {{0x90, 0x3C, 0x64}});
  check("split between data bytes", {{0x90, 0x3C}, {0x64}}, {{0x90, 0x3C, 0x64}});
  check("two messages in one packet", {{0x90, 0x3C, 0x64, 0x80, 0x3C, 0x00}},
        {{0x90, 0x3C, 0x64}, {0x80, 0x3C, 0x00}});
  check("running status", {{0x90, 0x3C, 0x64, 0x3E, 0x64}, {0x40, 0x64}},
        {{0x90, 0x3C, 0x64}, {0x90, 0x3E, 0x64}, {0x90, 0x40, 0x64}});
  check("real-time byte inside a message", {{0x90, 0x3C, 0xF8, 0x64}},
        {{0xF8}, {0x90, 0x3C, 0x64}});
  check("program change, 1 data byte, running status", {{0xC1, 0x05, 0x06}},
        {{0xC1, 0x05}, {0xC1, 0x06}});
  check("SysEx across packets with real-time inside", {{0xF0, 0x7D, 0x01}, {0xF8, 0x02, 0xF7}},
        {{0xF8}, {0xF0, 0x7D, 0x01, 0x02, 0xF7}});
  check("SysEx cut off by a status byte is dropped", {{0xF0, 0x7D, 0x01, 0x90, 0x3C, 0x64}},
        {{0x90, 0x3C, 0x64}});
  check("system common cancels running status", {{0x90, 0x3C, 0x64, 0xF3, 0x01, 0x3E, 0x64}},
        {{0x90, 0x3C, 0x64}, {0xF3, 0x01}});
  check("tune request has no data", {{0xF6}}, {{0xF6}});
  check("data before any status is ignored", {{0x3C, 0x64, 0x90, 0x3C, 0x64}}, {{0x90, 0x3C, 0x64}});
  return failures ? 1 : 0;
}
