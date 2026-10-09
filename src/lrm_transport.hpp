// Shared by both native copies (src/ and apple/Classes/).

#pragma once
#include <libremidi/types.hpp>

// libremidi's transport_type is a set of flags, not a single value: WinMIDI
// reports a loopback endpoint as software | loopback. Comparing with == made
// such ports count as physical, so isVirtual was false for them.
constexpr bool lrm_is_virtual_transport(libremidi::transport_type type) {
    return (static_cast<unsigned>(type)
            & (static_cast<unsigned>(libremidi::transport_type::software)
               | static_cast<unsigned>(libremidi::transport_type::loopback))) != 0;
}

// Checked by every build, on every platform.
static_assert(lrm_is_virtual_transport(libremidi::transport_type::software));
static_assert(lrm_is_virtual_transport(libremidi::transport_type::loopback));
static_assert(lrm_is_virtual_transport(static_cast<libremidi::transport_type>(
    libremidi::transport_type::software | libremidi::transport_type::loopback)));
static_assert(!lrm_is_virtual_transport(libremidi::transport_type::unknown));
static_assert(!lrm_is_virtual_transport(static_cast<libremidi::transport_type>(
    libremidi::transport_type::hardware | libremidi::transport_type::usb)));
static_assert(!lrm_is_virtual_transport(static_cast<libremidi::transport_type>(
    libremidi::transport_type::hardware | libremidi::transport_type::bluetooth)));
