#include "fstream_loader.hpp"

#include <algorithm>
#include <stdexcept>

namespace omega {
namespace {

// The block's data: +90 when set, else +88 (003eb240 / 003ebd0c / 003ebb04).
std::uint32_t data(const GameSoundResource& r) { return r.word_90 ? r.word_90 : r.word_88; }

void clear_block(GameSoundResource& r) {
    r.word_88 = 0;
    r.word_90 = 0;
    r.word_8c = 0;
    r.loaded_94 = 0;
}

[[noreturn]] void stop(const char* address, const char* what) {
    throw std::runtime_error(std::string("fstream loader: ") + address + " is not ported (" + what + ")");
}

}

FstreamRequest* FstreamLoader::create() {
    // 0036b180(0, 0x18, "fstream.cc", 0x24d): +c, +10, +0, +8 zero, state 6 then 0, +14 zero; 003eb310 push_back.
    auto& r = owned_.emplace_back(std::make_unique<FstreamRequest>());
    r->id = next_id_++;
    list_.push_back(r.get());
    return r.get();
}

void FstreamLoader::erase(FstreamRequest* request) {
    list_.remove(request);
}

void FstreamLoader::destroy(FstreamRequest* request) {
    erase(request);
    host_.free_request_36b130(request->id);
    // The EE frees the 0x18 record but 0x55a87c may still point at it (003ea6a0 skips 003ebad0 for the
    // current resource; 003eaa70 does not look at 0x55a87c). The next 003eb340 then reads the stale
    // record, so it is kept until it is neither listed nor current.
    owned_.remove_if([this](const std::unique_ptr<FstreamRequest>& r) {
        return r.get() != current_ && std::find(list_.begin(), list_.end(), r.get()) == list_.end();
    });
}

FstreamRequest* FstreamLoader::find(const GameSoundResource& resource, std::uint8_t state) {
    // 003ea910: the first request in state `state` whose resource name compares equal (00128e80) and whose
    // HOG (+98) matches when both are set. A request without a resource (only between 003ebad0 and its
    // erase) is not a candidate.
    for (auto* r : list_) {
        if (r->state_4 != state || !r->resource_8) continue;
        if (resource.name_4 != r->resource_8->name_4) continue;
        const auto a = resource.hog_98, b = r->resource_8->hog_98;
        if (a == 0 || b == 0 || a == b) return r;
    }
    return nullptr;
}

bool FstreamLoader::alloc_block(GameSoundResource& b, std::uint32_t size) {
    if (b.word_88) {
        b.loaded_94 = 1;
        return true;
    }
    const std::uint32_t total = size + 0x7f;
    b.word_88 = host_.alloc_36b180(total); // 0036b180(1, size + 0x7f, "fstream.cc", 0x37)
    if (!b.word_88) return false;
    b.word_90 = b.word_88 % 64 ? (b.word_88 + 0x7f) & ~0x7fU : 0; // 0011a668 (__moddi3)
    b.word_8c = total;
    b.loaded_94 = 0;
    return true;
}

void FstreamLoader::free_block(FstreamRequest& r, bool notify) {
    auto& b = *r.resource_8;
    if (notify && data(b)) {
        host_.callback(r.callback_c, b, r.arg_10, 0);
        if (b.word_88) {
            host_.free_36b130(b.word_88); // 0036b130(1, +88, "fstream.cc", 0x56)
            clear_block(b);
        }
    }
    if (r.copy_14) stop("003ebbd4", "freeing the loader's resource copy");
    r.callback_c = 0;
    r.arg_10 = 0;
    r.time_0 = 0;
    r.resource_8 = nullptr;
    r.state_4 = 6;
    r.copy_14 = 0;
}

bool FstreamLoader::start(FstreamRequest& r) {
    auto& b = *r.resource_8;
    host_.member_index_36def0(b);
    const auto size = host_.size_36de50(b);
    if (!alloc_block(b, size)) return false;
    r.state_4 = 1;
    if (!b.loaded_94) {
        host_.read_36d950(b, data(b), size);
        return true;
    }
    if ((r.state_4 == 1 || r.state_4 == 3) && data(b)) {
        b.loaded_94 = 1;
        r.state_4 = 3;
        host_.callback(r.callback_c, b, r.arg_10, 1);
    }
    return false;
}

bool FstreamLoader::done(FstreamRequest& c) {
    if (reading_ != 1) return false;
    if (c.state_4 == 3 || c.state_4 == 6) return true;
    if (!c.resource_8) return false;
    const auto p = host_.poll_36d8e0(*c.resource_8);
    if (p == 0) return false;
    if (p == -1) c.state_4 = 4;
    else if (c.state_4 != 2) c.state_4 = 3;
    return true;
}

std::int32_t FstreamLoader::load(GameSoundResource& resource, std::uint32_t callback, std::uint32_t arg,
                                 std::uint8_t sync, bool copy, bool allow_queued) {
    if (sync == 1) {
        if (current_) {
            // 003eaec8: busy-waits on the current request; without the reading flag it never ends.
            if (reading_ != 1) stop("003eaec8", "the synchronous wait on a current request without the reading flag");
            while (!done(*current_)) {}
            current_ = nullptr;
            reading_ = 0;
        }
        purge(true);
    }
    if (!allow_queued && find(resource, 0)) return 0;
    bool cached = false;
    auto* r = find(resource, 5);
    if (r) cached = r->resource_8->loaded_94 != 0;
    else r = find(resource, 3);
    if (!r) r = create();
    if (copy) stop("003eb108", "the 0x9c resource copy (0036e5d0 / 0036dfb0)");
    r->resource_8 = &resource;
    if (callback) r->callback_c = callback;
    if (arg) r->arg_10 = arg;
    r->state_4 = 0;
    if (sync == 1 || cached) {
        auto& b = *r->resource_8;
        host_.member_index_36def0(b);
        const auto size = host_.size_36de50(b);
        if (!alloc_block(b, size)) return 1;
        r->state_4 = 1;
        if (!b.loaded_94) host_.read_36d990(b, data(b), size);
        if ((r->state_4 == 1 || r->state_4 == 3) && data(b)) {
            b.loaded_94 = 1;
            r->state_4 = 3;
            host_.callback(r->callback_c, b, r->arg_10, 1);
        }
    }
    return 1;
}

std::int32_t FstreamLoader::release(GameSoundResource& resource, std::uint32_t callback, std::uint32_t arg,
                                    std::uint8_t now) {
    bool free_now = now == 1;
    auto* r = find(resource, 3);
    if (!r) {
        r = find(resource, 0);
        if (r) free_now = true;
    }
    if (!r) r = find(resource, 5);
    if (!r) {
        r = find(resource, 1);
        if (r) r->state_4 = 2;
    }
    if (!r) return 0;
    if (r->state_4 != 2) {
        if (callback) {
            r->resource_8 = &resource;
            r->callback_c = callback;
            if (arg) r->arg_10 = arg;
        }
        r->state_4 = 5;
        r->time_0 = host_.frame_55a818();
        if (free_now) {
            free_block(*r, true);
            destroy(r);
        }
    }
    while (auto* x = find(resource, 3)) destroy(x);
    while (auto* x = find(resource, 0)) destroy(x);
    if (auto* x = find(resource, 1)) x->state_4 = 2;
    return 1;
}

void FstreamLoader::purge(bool force) {
    for (auto it = list_.begin(); it != list_.end();) {
        auto* r = *it++;
        const auto* res = r->resource_8;
        if (!res || !res->word_8c || r->state_4 != 5) continue;
        if (!force && static_cast<std::int32_t>(host_.frame_55a818() - r->time_0) < 3) continue;
        if (!current_ || current_->resource_8 != res) free_block(*r, true);
        destroy(r);
    }
}

std::int32_t FstreamLoader::service() {
    if (!started_) started_ = 1;
    if (host_.heap_534cdc() == -1) return 0;
    purge(false);
    if (!current_) {
        for (auto* r : list_) {
            if (r->state_4 == 0 && start(*r)) {
                current_ = r;
                reading_ = 1;
                break;
            }
        }
    } else if (done(*current_)) {
        auto* c = current_;
        if (c->state_4 == 2) {
            c->state_4 = 5;
            c->time_0 = host_.frame_55a818();
        } else if (c->state_4 == 4) {
            c->state_4 = 0;
            auto& b = *c->resource_8;
            if (b.word_88) {
                host_.free_36b130(b.word_88);
                clear_block(b);
            }
            erase(c);
            list_.push_back(c); // 003eb310: retried from the back
        } else if (c->state_4 == 1 || c->state_4 == 3) {
            auto& b = *c->resource_8;
            if (data(b)) {
                b.loaded_94 = 1;
                c->state_4 = 3;
                host_.callback(c->callback_c, b, c->arg_10, 1);
            }
        }
        current_ = nullptr;
        reading_ = 0;
    }
    return static_cast<std::int32_t>(std::count_if(list_.begin(), list_.end(), [](const FstreamRequest* r) {
        return r->state_4 == 0;
    }));
}

}
