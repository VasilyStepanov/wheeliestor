/**
 * Copyright (C) 2026, Vasily Stepanov (vasily.stepanov@gmail.com)
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WHEELIE_PROTOCOL_ERROR_HPP
#define WHEELIE_PROTOCOL_ERROR_HPP

#include <stdexcept>

namespace wheelie {

// A request or a response that breaks the protocol; nothing that follows it on
// the connection can be trusted.
class ProtocolError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

} // namespace wheelie

#endif // WHEELIE_PROTOCOL_ERROR_HPP
