#pragma once

// The format-neutral core: what a value is, where bytes go, how a type says what its fields
// are, and how failures are reported. Knows about no format in particular.

#include <nonstd/serial/emitter.hpp>
#include <nonstd/serial/error.hpp>
#include <nonstd/serial/kind.hpp>
#include <nonstd/serial/limits.hpp>
#include <nonstd/serial/reflect.hpp>
#include <nonstd/serial/serializer.hpp>
#include <nonstd/serial/sink.hpp>
#include <nonstd/serial/visitor.hpp>

/**
 * The package is named serpent; the namespace is nonstd, so these headers drop into a
 * firmware's lib/common/include/nonstd unchanged. The alias lets you write either.
 */
namespace serpent = nonstd;
