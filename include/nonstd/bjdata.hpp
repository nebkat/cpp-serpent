#pragma once

// BJData: the reader, the writer, and the document-level helpers.

#include <nonstd/bjdata/document.hpp>
#include <nonstd/bjdata/marker.hpp>
#include <nonstd/bjdata/view.hpp>
#include <nonstd/bjdata/writer.hpp>
#include <nonstd/serial/error.hpp>
#include <nonstd/serial/kind.hpp>
#include <nonstd/serial/serializer.hpp>
#include <nonstd/serial/sink.hpp>

/**
 * The package is named serpent; the namespace is nonstd, so these headers drop into a
 * firmware's lib/common/include/nonstd unchanged. The alias lets you write either.
 */
namespace serpent = nonstd;
