// MIDI 1.0 byte-stream parser for the Android input.
//
// android.media.midi delivers a byte stream, not messages: a packet may end in
// the middle of a message, use running status, or carry real-time bytes in the
// middle of another message. libremidi's decoder assumes every packet holds
// whole messages (only SysEx may span packets), so a note split as [90] [3C 64]
// came out as a bogus message and the rest was dropped. This parser keeps state
// across packets and hands out complete messages.
//
// No Android dependencies, so it is tested on the host
// (tool/native_tests/stream_parser_test.cpp).

#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lrm
{
class midi_stream_parser
{
public:
  // Feeds bytes; calls emit(const uint8_t* data, size_t size) for every complete
  // message, real-time bytes (F8-FF) at once wherever they appear.
  template <typename Emit>
  void feed(const uint8_t* bytes, size_t count, Emit&& emit)
  {
    for (size_t i = 0; i < count; ++i)
    {
      const uint8_t b = bytes[i];

      if (b >= 0xF8)
      {
        emit(&b, 1);  // real-time: never interrupts the message being built
        continue;
      }

      if (b & 0x80)
      {
        if (in_sysex_)
        {
          in_sysex_ = false;
          if (b == 0xF7)
          {
            msg_.push_back(b);
            emit(msg_.data(), msg_.size());
            msg_.clear();
            continue;
          }
          // Any other status byte ends an unterminated SysEx; it is dropped.
          msg_.clear();
        }
        if (b == 0xF0)
        {
          msg_.assign(1, b);
          in_sysex_ = true;
          running_ = 0;
          continue;
        }
        if (b == 0xF7)
          continue;  // stray end of SysEx

        msg_.assign(1, b);
        expected_ = data_bytes(b);
        running_ = b < 0xF0 ? b : 0;  // system common cancels running status
        if (expected_ == 0)
        {
          emit(msg_.data(), msg_.size());
          msg_.clear();
        }
        continue;
      }

      // Data byte.
      if (in_sysex_)
      {
        if (msg_.size() < kMaxSysex)
          msg_.push_back(b);
        else
        {
          // Runaway SysEx without an end: give up on it.
          msg_.clear();
          in_sysex_ = false;
        }
        continue;
      }
      if (msg_.empty())
      {
        if (!running_)
          continue;  // data without a status byte: nothing to attach it to
        msg_.assign(1, running_);
        expected_ = data_bytes(running_);
      }
      msg_.push_back(b);
      if (msg_.size() == 1 + expected_)
      {
        emit(msg_.data(), msg_.size());
        msg_.clear();
      }
    }
  }

private:
  static size_t data_bytes(uint8_t status)
  {
    if (status < 0xC0)
      return 2;  // note off/on, poly pressure, control change
    if (status < 0xE0)
      return 1;  // program change, channel pressure
    if (status < 0xF0)
      return 2;  // pitch bend
    switch (status)
    {
      case 0xF1:
      case 0xF3:
        return 1;  // MTC quarter frame, song select
      case 0xF2:
        return 2;  // song position
      default:
        return 0;  // tune request and undefined system common
    }
  }

  static constexpr size_t kMaxSysex = 1 << 20;

  std::vector<uint8_t> msg_;
  size_t expected_ = 0;
  uint8_t running_ = 0;
  bool in_sysex_ = false;
};
}
