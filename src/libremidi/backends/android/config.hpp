// Replaces upstream libremidi's backends/android/config.hpp.
//
// Upstream carries a client_name here, held as a std::string_view: the
// set_client_name() helper assigns it from a temporary std::string, which left
// it dangling. Android has no notion of a client name, so there is nothing to
// configure at all.

#pragma once
#include <libremidi/config.hpp>

NAMESPACE_LIBREMIDI
{
namespace android
{
struct input_configuration
{
};

struct output_configuration
{
};

struct observer_configuration
{
};
}
}
