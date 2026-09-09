// Carrying an already-encoded document inside another one, without materialising it.
//
// A dispatcher that wraps a handler's reply in an envelope never needs to look inside that
// reply - it only needs to put it somewhere. Decoding it into an object graph so it can be
// encoded again is the expensive way to do nothing. This holds the bytes instead, splices them
// when the destination is the format they are already in, and transcodes when it is not.

#include <serpent/bjdata.hpp>
#include <serpent/bjdata/json.hpp>
#include <serpent/json.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace bjdata = serpent::bjdata;
namespace json = serpent::json;

/** A BJData document to be placed inside another, whatever format that one is written in. */
struct fragment {
    std::vector<std::byte> bytes;

    // Into BJData the bytes go through untouched. Into JSON they are walked and transcribed.
    // One overload per writer is what to_json is for: neither the type nor its author has to
    // ask what is being written.
    template<bjdata::writer_options Options>
    friend void to_json(bjdata::basic_writer<Options> &out, const fragment &value) {
        bjdata::write_value(out, bjdata::view::over(value.bytes));
    }

    friend void to_json(json::writer &out, const fragment &value) {
        json::write_value(out, bjdata::view::over(value.bytes));
    }
};

struct[[= serpent::serializable {}]] failure {
    std::string error;
    int code = 0;
    fragment info; // whatever the thrower had to say, already encoded
};

struct[[= serpent::serializable {}]] argument_detail {
    std::string message;
    std::vector<int> offending;
};

int main() {
    // Built once, at the point that knows what it means.
    const failure response {
        .error = "invalid_argument",
        .code = 22,
        .info = { bjdata::encode(argument_detail { "bad argument", { 3, 7 } }) },
    };

    // The same value, in both formats, with the payload never decoded on the way.
    std::printf("%s\n", json::encode(response).c_str());

    const auto binary = bjdata::encode(response);
    std::printf("%s\n", json::encode(bjdata::view::over(binary)).c_str());

    // And it is a real part of the document, not an opaque blob: it reads back through the
    // envelope like anything else.
    const auto nested = bjdata::view::over(binary)["info"];
    const auto message = nested["message"].as_string().value_or("");
    if (message != "bad argument") return 1;
    if (nested["offending"][1].as_int<int>().value_or(0) != 7) return 1;

    std::printf("\nthe payload was spliced into %zu bytes of BJData and transcribed into JSON,\n"
                "and never became an object graph in either direction\n",
            binary.size());
}
