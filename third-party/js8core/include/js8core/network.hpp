#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace js8core {

struct Endpoint {
  std::string host;
  std::uint16_t port = 0;
};

struct Datagram {
  Endpoint destination;
  std::vector<std::byte> payload;
};

using DatagramHandler = std::function<void(Endpoint const& from, std::span<const std::byte> payload)>;
using NetworkErrorHandler = std::function<void(std::string_view message)>;

class UdpChannel {
public:
  virtual ~UdpChannel() = default;
  virtual bool bind(Endpoint const& listen_on) = 0;
  virtual bool send(Datagram const& datagram) = 0;
  virtual void set_handlers(DatagramHandler on_receive, NetworkErrorHandler on_error) = 0;
  virtual void close() = 0;
};

}  // namespace js8core
