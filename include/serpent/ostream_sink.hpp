#pragma once

// Kept out of sink.hpp so that <ostream> stays out of firmware translation units.

#include <serpent/sink.hpp>

#include <ostream>
#include <span>

#include <cstddef>

namespace serpent {

/** Writes straight to a std::ostream, which is what app::fs::save_json's ofstream needs. */
class ostream_sink {
    std::ostream *target = nullptr;

public:
    explicit ostream_sink(std::ostream &target) noexcept: target(&target) {}

    bool write(std::span<const std::byte> bytes) {
        this->target->write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(*this->target);
    }
};

}// namespace serpent
