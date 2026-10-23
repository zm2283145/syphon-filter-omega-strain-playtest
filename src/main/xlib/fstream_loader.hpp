#pragma once

// fstream.cc's asynchronous resource loader (checkpoint 119o): the singleton
// 003eba50 (0055a870: the request list 00139070, +c the current request,
// +14 the started byte; 00493990 the reading flag), the load 003eae60, the
// release 003eaa60 (-> 003eaa70 without a callback), the per-frame service
// 003eb340 (00167c40 at 00167dd0), the purge 003ea6a0, the find 003ea910,
// the start 003ebc90, the block allocation 003ebdd0 and the block release
// 003ebad0. The resources are 0036e5d0's records (GameSoundResource: the
// block +88 allocation, +8c size, +90 aligned data, +94 loaded).
// The resource services 0036def0 / 0036de50 / 0036d950 / 0036d990 /
// 0036d8e0, the allocator 0036b180 / 0036b130 (mode 1: the data heap), the
// frame counter 0055a818 and *(00534cdc) (heap 1's +14, -1 until the heap
// exists) are the host's. The request and list-node allocations (mode 0)
// are host objects that do not fail. The 0x9c resource copy (t1 != 0,
// 003eb108) throws naming its address.

#include "game_sound.hpp"

#include <cstdint>
#include <list>
#include <memory>

namespace omega {

// A 0x18-byte request (003eb09c). `id` stands for its address (creation order).
struct FstreamRequest {
    std::uint32_t id{};
    std::uint32_t time_0{};             // *(0055a818) when it became state 5
    std::uint8_t state_4{};             // 0 queued, 1 reading, 2 released while reading, 3 loaded, 4 error, 5 released, 6 reset
    GameSoundResource* resource_8{};
    std::uint32_t callback_c{};         // callback(resource +88, arg, loaded)
    std::uint32_t arg_10{};
    std::uint8_t copy_14{};             // +8 is the loader's own 0x9c copy
};

class FstreamHost {
public:
    virtual ~FstreamHost() = default;
    virtual void member_index_36def0(GameSoundResource& resource) = 0;       // the HOG member index (+84)
    virtual std::uint32_t size_36de50(GameSoundResource& resource) = 0;      // the member's byte size
    virtual std::uint32_t alloc_36b180(std::uint32_t size) = 0;              // mode 1 ("fstream.cc"); 0 on failure
    virtual void free_36b130(std::uint32_t address) = 0;                     // mode 1
    virtual void free_request_36b130(std::uint32_t id) = 0;                  // mode 0: a request
    // 0036d950 (asynchronous) / 0036d990 (synchronous) reads into dest.
    virtual void read_36d950(GameSoundResource& resource, std::uint32_t dest, std::uint32_t size) = 0;
    virtual void read_36d990(GameSoundResource& resource, std::uint32_t dest, std::uint32_t size) = 0;
    virtual std::int32_t poll_36d8e0(GameSoundResource& resource) = 0;      // 1 done, 0 busy, -1 error
    virtual void callback(std::uint32_t function, GameSoundResource& resource, std::uint32_t arg, std::uint32_t loaded) = 0;
    virtual std::uint32_t frame_55a818() = 0;
    virtual std::int32_t heap_534cdc() = 0;
};

class FstreamLoader {
public:
    explicit FstreamLoader(FstreamHost& host) : host_(host) {}

    // 003eae60(loader, res, cb, arg, t0 sync, t1 copy, t2 allow duplicate queue): 0 when already queued.
    std::int32_t load(GameSoundResource& resource, std::uint32_t callback, std::uint32_t arg, std::uint8_t sync,
                      bool copy, bool allow_queued);
    // 003eaa70(loader, res, cb, arg, t0); 003eaa60 passes cb = arg = 0.
    std::int32_t release(GameSoundResource& resource, std::uint32_t callback, std::uint32_t arg, std::uint8_t now);
    std::int32_t release(GameSoundResource& resource, std::uint8_t now) { return release(resource, 0, 0, now); } // 003eaa60
    std::int32_t service();                     // 003eb340: the count of queued (state 0) requests
    void purge(bool force);                     // 003ea6a0
    FstreamRequest* find(const GameSoundResource& resource, std::uint8_t state); // 003ea910

    const std::list<FstreamRequest*>& requests() const { return list_; }
    const FstreamRequest* current() const { return current_; }
    std::uint8_t reading_493990() const { return reading_; }
    std::uint8_t started_14() const { return started_; }

private:
    bool start(FstreamRequest& request);                                   // 003ebc90
    bool alloc_block(GameSoundResource& resource, std::uint32_t size);     // 003ebdd0
    void free_block(FstreamRequest& request, bool notify);                 // 003ebad0
    bool done(FstreamRequest& current);                                     // the inline poll of 003eb340 / 003eae60
    void erase(FstreamRequest* request);                                   // 00138ab0 (by value)
    void destroy(FstreamRequest* request);                                 // erase + 0036b130(0, request)
    FstreamRequest* create();                                              // 0036b180(0, 0x18) + 003eb310
    FstreamHost& host_;
    std::list<FstreamRequest*> list_;
    std::list<std::unique_ptr<FstreamRequest>> owned_;
    FstreamRequest* current_{};
    std::uint8_t reading_{};
    std::uint8_t started_{};
    std::uint32_t next_id_{};
};

}
