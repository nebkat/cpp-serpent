#pragma once

// The containers a tree builder has open, kept on the heap rather than in its calls.
//
// A format's builder moves one cursor through a document and keeps here what each container
// still owes and what it has gathered so far, so the stack it takes is the same however deep the
// document goes. What a format's bytes mean stays in its own builder; this is the part every
// format's has in common.
//
// A template on the tree so that a format's header need not include value.hpp.

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace serpent::detail {

/**
 * The containers open, and what they have gathered. `Placing` is for a format whose every
 * container gives its count first: one that gives a small count has its run sized when it opens
 * and its children put straight into that, with nothing gathered. Off for a format that would
 * not use it, which then compiles none of it.
 */
template<typename Tree, bool Placing = false>
class tree_stack {
protected:
    using member = std::pair<Tree, Tree>;
    using array_type = typename Tree::array_type;
    using object_type = typename Tree::object_type;

    /** A container still open: what is left of it, and where its children start being gathered. */
    struct gathering_frame {
        std::uint64_t remaining = 0;
        std::size_t start = 0;
        bool object = false;
        bool unbounded = false;
    };

    /** Or, when it gave a small count, the run its children go straight into. */
    struct placing_frame : gathering_frame {
        array_type *items = nullptr;
        object_type *entries = nullptr;
        bool placed = false;
    };

    using frame = std::conditional_t<Placing, placing_frame, gathering_frame>;

    // One frame a level, for the containers open from the outermost down.
    std::vector<frame> frames;

    // The members and elements of the containers open that are gathered: used as a stack, each
    // container's after its parent's, cut back when it closes, and moved into a run sized exactly
    // to it. A member is gathered with its name when the name is read, and its value put in place
    // when that is complete, which for a container is when it closes.
    std::vector<member> gathered_members;
    std::vector<Tree> gathered_elements;

    // The most children a container is taken at its word for, its run sized before any is read.
    // A count is the document's to claim, so beyond this one is gathered for as it is read.
    static constexpr std::uint64_t placed_most = 64;

    tree_stack() = default;
    tree_stack(const tree_stack &) = delete;
    tree_stack &operator=(const tree_stack &) = delete;

    ~tree_stack() {
        // What a document that broke partway left open.
        if constexpr (Placing) {
            for (const auto &open : this->frames) {
                array_type::release(open.items);
                object_type::release(open.entries);
            }
        }
    }

    /** Opens a container, `remaining` children long unless it is `unbounded`. */
    void open(bool object, std::uint64_t remaining, bool unbounded) {
        // Room for a few levels at the first, so a document as deep as most are allocates once.
        if (this->frames.capacity() == 0) this->frames.reserve(8);
        frame opened {};
        opened.remaining = remaining;
        opened.object = object;
        opened.unbounded = unbounded;
        if constexpr (Placing) {
            if (!unbounded && remaining <= placed_most) {
                opened.placed = true;
                const auto room = static_cast<std::uint32_t>(remaining);
                if (object) opened.entries = object_type::reserved(room);
                else opened.items = array_type::reserved(room);
                this->frames.push_back(opened);
                return;
            }
        }
        opened.start = object ? this->gathered_members.size() : this->gathered_elements.size();
        this->frames.push_back(opened);
    }

    /** Takes the name of the member whose value comes next. */
    void name(Tree text) {
        if constexpr (Placing) {
            if (auto &top = this->frames.back(); top.placed) {
                object_type::place_back(top.entries, member { std::move(text), Tree {} });
                return;
            }
        }
        this->gathered_members.emplace_back(std::move(text), Tree {});
    }

    /** Puts a completed child where the container on top keeps it. */
    void deliver(Tree held) {
        auto &top = this->frames.back();
        if constexpr (Placing) {
            if (top.placed) {
                if (top.object) (*top.entries)[top.entries->size() - 1].second = std::move(held);
                else array_type::place_back(top.items, std::move(held));
                return;
            }
        }
        if (top.object) this->gathered_members.back().second = std::move(held);
        else this->gathered_elements.push_back(std::move(held));
    }

    /** The members gathered since `start`, moved into a run of their own with one per name. */
    Tree members_from(std::size_t start) {
        const auto gathered = std::span { this->gathered_members }.subspan(start);
        auto *block = object_type::reserved(static_cast<std::uint32_t>(gathered.size()));
        object_type::place_all(block, gathered);
        this->gathered_members.resize(start);
        Tree held { block };
        held.coalesce_members();
        return held;
    }

    /** The elements gathered since `start`, moved into a run of their own. */
    Tree elements_from(std::size_t start) {
        const auto gathered = std::span { this->gathered_elements }.subspan(start);
        auto *block = array_type::reserved(static_cast<std::uint32_t>(gathered.size()));
        array_type::place_all(block, gathered);
        this->gathered_elements.resize(start);
        return Tree { block };
    }

    /** Closes the container on top, answering it. */
    Tree close() {
        const frame top = this->frames.back();
        this->frames.pop_back();
        if constexpr (Placing) {
            if (top.placed && !top.object) return Tree { top.items };
            if (top.placed) {
                Tree held { top.entries };
                held.coalesce_members();
                return held;
            }
        }
        return top.object ? this->members_from(top.start) : this->elements_from(top.start);
    }
};

} // namespace serpent::detail
