#pragma once
#include <libremidi/config.hpp>

#include <string>

NAMESPACE_LIBREMIDI
{
namespace android
{
struct input_configuration
{
  std::string client_name = "libremidi client";
};

struct output_configuration
{
  std::string client_name = "libremidi client";
};

struct observer_configuration
{
  std::string client_name = "libremidi client";
};
}
}