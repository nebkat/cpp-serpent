#pragma once

// JSON: the reader and the writer.
//
// Rendering an existing BJData document as JSON lives in <nonstd/bjdata/json.hpp>, which is
// the only header that has to know about both formats.

#include <nonstd/json/reader.hpp>
#include <nonstd/json/writer.hpp>
#include <nonstd/serial/error.hpp>
#include <nonstd/serial/kind.hpp>
#include <nonstd/serial/serializer.hpp>
#include <nonstd/serial/sink.hpp>
