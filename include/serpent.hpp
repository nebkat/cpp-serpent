#pragma once

// The format-neutral core: what a value is, where bytes go, how a type says what its fields
// are, and how failures are reported. Knows about no format in particular.

#include <serpent/emitter.hpp>
#include <serpent/error.hpp>
#include <serpent/kind.hpp>
#include <serpent/limits.hpp>
#include <serpent/reflect.hpp>
#include <serpent/serializer.hpp>
#include <serpent/sink.hpp>
#include <serpent/visitor.hpp>
