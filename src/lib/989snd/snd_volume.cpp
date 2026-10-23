#include "snd_volume.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace omega::snd989 {
namespace {

// The compiler's division by a constant: hi(x * magic) + x, arithmetic shift, minus the sign of x.
std::int32_t magic_div(std::int32_t x, std::uint32_t magic, int shift) {
    const auto product = static_cast<std::int64_t>(x) * static_cast<std::int32_t>(magic);
    const auto hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32);
    const auto sum = static_cast<std::int32_t>(hi + static_cast<std::uint32_t>(x));
    return (sum >> shift) - (x >> 31);
}

std::int32_t mul(std::int32_t a, std::int32_t b) {  // mult / mflo
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(a) * b));
}

std::int32_t div127(std::int32_t x) { return magic_div(x, 0x81020409U, 6); }
std::int32_t div3fff(std::int32_t x) { return magic_div(x, 0x80020009U, 13); }

std::int32_t sdiv(std::int32_t a, std::int32_t b) {  // div / mflo (b != 0: the IRX breaks on 0 first)
    if (b == -1) return static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(a));
    return a / b;
}

std::int32_t mag(std::int32_t x) { return x < 0 ? static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(x)) : x; }

std::int32_t wrap_pan(std::int64_t pan) {
    auto p = static_cast<std::int32_t>(((pan % 360) + 360) % 360);
    return p < 270 ? p + 90 : p - 270;
}

void volume_tail(const VolumeState& s, std::int32_t t0, std::int32_t pan, std::array<std::int16_t, 2>& out) {
    if (t0 == 0) {
        out = {0, 0};
        return;
    }
    if (s.play_mode == 1) {
        out = {static_cast<std::int16_t>(t0), static_cast<std::int16_t>(t0)};
        return;
    }
    const auto p = wrap_pan(pan);
    const auto& table = pan_table();
    const std::int32_t o0 = out[0], o1 = out[1];
    std::int32_t l = 0, r = 0;
    if (p < 180) {
        l = div3fff(mul(t0, table[std::size_t(p)][0]));
        r = div3fff(mul(t0, table[std::size_t(p)][1]));
        const bool n0 = o0 < 0, n1 = o1 < 0;
        if (n0 && n1) {
            l = -l;
            r = -r;
        } else if (n0 != n1) {
            const std::int32_t a0 = o0 < 0 ? -o0 : o0, a1 = o1 < 0 ? -o1 : o1;
            const std::int32_t sign = a1 < a0 ? o0 / a0 : o1 / a1;
            l = mul(l, sign);
            r = mul(r, sign);
        }
    } else {
        const auto q = std::size_t(p - 180);
        r = div3fff(mul(t0, table[q][0]));
        l = div3fff(mul(t0, table[q][1]));
        if (s.play_mode != 2) {
            if (o0 >= 0 && o1 >= 0) {
                if (r < l) r = -r; else l = -l;
            } else if (o0 >= 0) {
                r = -r;
            } else if (o1 >= 0) {
                l = -l;
            } else {
                if (l < r) r = -r; else l = -l;
            }
        }
    }
    out = {static_cast<std::int16_t>(l), static_cast<std::int16_t>(r)};
}

void need_group(std::int32_t group, std::size_t size, const char* where) {
    if (group < 0 || std::size_t(group) >= size)
        throw std::runtime_error(std::string("989snd ") + where + ": volume group " + std::to_string(group) + " outside its table");
}

}

const std::array<std::array<std::int16_t, 2>, 181>& pan_table() {
    static const auto table = [] {
        std::array<std::array<std::int16_t, 2>, 181> t{};
        const double pi = 3.14159265358979323846;
        for (int k = 0; k < 180; ++k) {
            const double a = k * pi / 360.0;
            t[std::size_t(k)] = {static_cast<std::int16_t>(16383.0 * std::cos(a)), static_cast<std::int16_t>(16383.0 * std::sin(a))};
        }
        t[180] = {16383, 0};
        return t;
    }();
    return table;
}

VolumeState::VolumeState() {
    master.fill(0x400);
    duck_reset(*this);
}

void duck_reset(VolumeState& s) {
    for (auto& d : s.duckers) d.source = -1;
    s.duck.fill(0x10000);
}

std::int16_t adjust_vol_to_group(const VolumeState& s, std::int32_t vol, std::int32_t group) {
    std::int32_t v = static_cast<std::int16_t>(vol);
    if (group >= 15) return static_cast<std::int16_t>(v);
    need_group(group, s.duck.size(), "1ae88");
    if (v >= 0x7fff) v = 0x7ffe;
    auto m = mul(s.master[std::size_t(group)], s.duck[std::size_t(group)]);
    if (m < 0) m += 0xffff;
    m >>= 16;
    auto x = mul(v, m);
    if (x < 0) x += 0x3ff;
    x >>= 10;
    const std::int32_t sign = x >= 0 ? 1 : -1;
    const auto q = magic_div(mul(x, x), 0x80020009U, 14);
    return static_cast<std::int16_t>(mul(q, sign));
}

MasterResult set_master_volume(VolumeState& s, std::int32_t group, std::int32_t vol) {
    if (vol > 0x400) vol = 0x400;
    if (vol < 0) vol = 0;
    MasterResult r;
    if (group == 15) {
        r.posted_53 = true;
        return r;
    }
    need_group(group, s.master.size(), "1a408");
    s.master[std::size_t(group)] = vol;
    if (group != 16) {
        r.refresh_mask = 1U << (group & 31);
        return r;
    }
    auto x = mul(vol, 0x3fff);
    if (x < 0) x += 0x3ff;
    r.spu_master = (x >> 10) & 0xffff;
    return r;
}

void set_ducker(VolumeState& s, std::uint32_t which, const std::array<std::int32_t, 6>* def) {
    if (which >= 4) return;
    auto& d = s.duckers[which];
    if (!def) {
        d.source = -1;
        return;
    }
    d = {(*def)[0], (*def)[1], (*def)[2], (*def)[3], (*def)[4], (*def)[5]};
    const auto range = static_cast<std::int32_t>(0x10000U - static_cast<std::uint32_t>(d.full));
    const auto step = [&](std::int32_t time) {
        auto ticks = mul(time, 30) >> 16;
        if (ticks == 0) ticks = 1;
        auto v = static_cast<std::int32_t>(static_cast<std::int64_t>(range) / ticks);
        return v == 0 ? 1 : v;
    };
    d.attack = step(d.attack);
    d.release = step(d.release);
    d.current = 0x10000;
}

std::uint32_t voice_group_bit(std::int8_t group) { return 1U << (static_cast<std::uint32_t>(group) & 31); }

std::uint32_t duck_tick(VolumeState& s, std::uint32_t active_groups) {
    std::array<std::int32_t, 15> duck;
    duck.fill(0x10000);
    for (auto& d : s.duckers) {
        if (d.source == -1) continue;
        if (active_groups & (1U << (static_cast<std::uint32_t>(d.source) & 31))) {
            if (d.current != d.full) {
                d.current = static_cast<std::int32_t>(static_cast<std::uint32_t>(d.current) - static_cast<std::uint32_t>(d.attack));
                if (d.current < d.full) d.current = d.full;
            }
        } else {
            if (d.current == 0x10000) continue;
            d.current = static_cast<std::int32_t>(static_cast<std::uint32_t>(d.current) + static_cast<std::uint32_t>(d.release));
            if (0x10000U < static_cast<std::uint32_t>(d.current)) d.current = 0x10000;
        }
        const auto mask = static_cast<std::uint32_t>(d.targets);
        for (std::size_t j = 0; j < duck.size(); ++j)
            if ((mask & (1U << j)) && d.current < duck[j]) duck[j] = d.current;
    }
    std::uint32_t changed = 0;
    for (std::size_t j = 0; j < duck.size(); ++j) {
        if (s.duck[j] == duck[j]) continue;
        s.duck[j] = duck[j];
        changed |= 1U << j;
    }
    return changed;
}

std::int32_t stream_volume(std::int32_t old, std::int32_t vol) {
    std::int32_t v;
    if (vol < 0) v = static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(vol));
    else if (vol == 0x7fffffff) v = old;
    else v = mul(vol, 127) >> 10;
    return v < 0x80 ? v : 0x7f;
}

std::int32_t stream_pan(std::int32_t old, std::int32_t pan) {
    if (pan == -1) pan = 0;
    else if (pan == -2) pan = old;
    while (pan >= 360) pan -= 360;
    while (pan < 0) pan += 360;
    return pan;
}

FadeCall fade_start(FadeStream& s, bool pool_free, std::int16_t vol, std::int16_t time16, std::int16_t mode) {
    FadeCall call;
    const bool type5 = ((s.handle >> 24) & 0x1f) == 5;
    std::int32_t target = vol, time = time16;
    bool stop = false;
    if (target == -4) {  // 3b8c
        target = 0;
        stop = true;
    } else if (!type5) {  // 3bb0: vol * stream +c >> 10, at most 127
        target = mul(target, s.scale) >> 10;
        if (!(target < 0x80)) target = 0x7f;
    }
    const std::int32_t cur = type5 ? s.volume_3a : s.volume_10;
    const auto delta = static_cast<std::int32_t>(static_cast<std::uint32_t>(target) - static_cast<std::uint32_t>(cur));
    const auto negated = static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(target));
    if (time == 0) {  // 3bf8: e990, then c060 or c1d4(handle, -target, -2)
        s.fade.reset();
        if (stop) call.stop = true;
        else call.set_volume = negated;
        return call;
    }
    if (delta == 0) {  // 3c3c: e990 only
        s.fade.reset();
        return call;
    }
    bool fresh = false;
    if (s.fade) {  // 3c60: e9c4 found it; locked while it is reprogrammed
        s.fade->flags |= 0x80000000U;
        if (mode == 3) {  // 3c78: the remaining time scaled to the new distance
            const auto d = static_cast<std::int32_t>(static_cast<std::uint32_t>(target) - static_cast<std::uint32_t>(s.fade->target));
            if (d == 0) {  // 3c8c: e990, c1d4(handle, -target, -2)
                s.fade.reset();
                call.set_volume = negated;
                return call;
            }
            time = mag(sdiv(mul(delta, time), d));
            if (!(time > 0)) time = 1;
        }
    } else {  // 3cdc: e9fc
        if (!pool_free) {
            call.set_volume = negated;
            return call;
        }
        s.fade = FadeRecord{};
        s.fade->flags = 1U | 2U;  // e9fc +0 = 1; 3d08 +10 = 3e1c, +0 |= 2
        fresh = true;
    }
    auto& r = *s.fade;
    if (mag(delta) < time) {  // 3d38: one step of 1 every |time / delta| ticks
        const auto every = mag(sdiv(time, delta));
        r.reload = every;
        r.countdown = 1;
        r.count = every;
        r.step = target < cur ? -1 : 1;
    } else {  // 3d84: a step of (delta * 32) / time every tick
        r.step = sdiv(static_cast<std::int32_t>(static_cast<std::uint32_t>(delta) << 5), time);
        r.countdown = 0;
        r.count = 0;
    }
    r.target = stop ? -4 : target;
    if (!fresh) r.flags &= 0x7fffffffU;  // 3dd4 (a new record is appended by ea44)
    return call;
}

FadeStep fade_callback(FadeStream& s) {
    auto& r = *s.fade;
    FadeStep out;
    if (r.target == -4) {  // 3e4c
        r.target = 0;
        out.stop = true;
    }
    const std::int32_t cur = ((s.handle >> 24) & 0x1f) == 5 ? s.volume_3a : s.volume_10;
    std::int32_t v;
    if (!(mag(r.step) < 2))  // 3e98: ((cur << 5) + step) << 11 >> 16
        v = static_cast<std::int32_t>(((static_cast<std::uint32_t>(cur) << 5) + static_cast<std::uint32_t>(r.step)) << 11) >> 16;
    else  // 3ea4: (s16)(u16 step + cur)
        v = static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(r.step) + static_cast<std::uint32_t>(cur)));
    if (r.step > 0) {
        if (r.target < v) v = static_cast<std::int16_t>(r.target);
    } else if (r.step < 0) {
        if (v < r.target) v = static_cast<std::int16_t>(r.target);
    }
    out.set_volume = static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(v));  // c1d4(handle, -v, -2)
    if (v == r.target) {  // 3f14: ea8c
        s.fade.reset();
        return out;
    }
    if (r.countdown == 1) r.count = r.reload;
    if (out.stop) r.target = -4;
    out.stop = false;
    return out;
}

std::optional<FadeStep> fade_tick(FadeStream& s) {
    if (!s.fade) return std::nullopt;
    auto& r = *s.fade;
    if (r.flags & 0x80000000U) return std::nullopt;  // e8f0: locked
    if (r.countdown == 1) r.count = static_cast<std::int32_t>(static_cast<std::uint32_t>(r.count) - 1U);
    if (r.count != 0) return std::nullopt;
    return fade_callback(s);
}

namespace {
std::int32_t add(std::int32_t a, std::int32_t b) {  // addu / subu
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t sx16(std::int32_t v) { return static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(v))); }
}

GlideCall glide_start(GlideStream& s, bool pool_free, std::int32_t target, std::int32_t dir, std::int32_t time, std::int32_t mode) {
    GlideCall call;
    std::int32_t delta = add(target, -static_cast<std::int32_t>(s.pan_12));  // 2c30: target - (s16)+12
    bool shortest = false;
    if (dir == 0) {  // 2c3c
        shortest = true;
        dir = 1;
    }
    if (dir > 0 && delta < 0) delta = add(delta, 360);
    if (dir < 0 && delta > 0) delta = add(delta, -360);
    if (shortest && !(mag(delta) < 0xb5)) {  // 2c74: the other way round
        dir = static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(dir));
        delta = add(delta, -360);
    }
    if (mode == 4) time = magic_div(mul(time, mag(delta)), 0xb60b60b7U, 7);  // 2c9c: time * |delta| / 180
    if (time == 0) {  // 2cd8: e990, then c1d4(handle, 0x7fffffff, target)
        s.glide.reset();
        call.set_pan = target;
        return call;
    }
    if (delta == 0) {  // 2cf0: e990 only
        s.glide.reset();
        return call;
    }
    bool fresh = false;
    if (s.glide) {  // 2d20: e9c4 found it; locked while it is reprogrammed
        s.glide->flags |= 0x80000000U;
        if (mode == 3) {  // 2d38: the time rescaled to the distance from the old target
            std::int32_t a = add(target, -s.glide->target);
            if (dir > 0 && a < 0) a = add(a, 360);
            if (dir < 0 && a > 0) a = add(a, -360);
            if (a == 0) {  // 2d6c: e990, c1d4(handle, 0x7fffffff, target)
                s.glide.reset();
                call.set_pan = target;
                return call;
            }
            time = mag(sdiv(mul(delta, time), a));
            if (!(time > 0)) time = 1;
        }
    } else {  // 2dbc: e9fc
        if (!pool_free) {  // 2dd0
            call.set_pan = target;
            return call;
        }
        s.glide = FadeRecord{};
        s.glide->flags = 1U | 4U;  // e9fc +0 = 1; 2df4 +10 = 2ef0, +0 |= 4
        fresh = true;
    }
    auto& r = *s.glide;
    if (mag(delta) < time) {  // 2e24: one degree every |time / delta| ticks
        const auto every = mag(sdiv(time, delta));
        r.reload = every;
        r.countdown = 1;
        r.count = every;
        r.step = delta < 0 ? -1 : 1;
    } else {  // 2e6c: (delta * 32) / time every tick
        r.step = sdiv(static_cast<std::int32_t>(static_cast<std::uint32_t>(delta) << 5), time);
        r.countdown = 0;
        r.count = 0;
    }
    r.target = target;
    if (!fresh) r.flags &= 0x7fffffffU;  // 2ea4 (a new record is appended by ea44)
    return call;
}

GlideCall glide_callback(GlideStream& s) {
    auto& r = *s.glide;
    const std::int32_t old = s.pan_12;
    std::int32_t pan;
    if (!(mag(r.step) < 2))  // 2f28: ((old << 5) + step) << 11 >> 16
        pan = static_cast<std::int32_t>(((static_cast<std::uint32_t>(old) << 5) + static_cast<std::uint32_t>(r.step)) << 11) >> 16;
    else  // 2f44: (s16)(u16 +12 + u16 step)
        pan = sx16(add(old & 0xffff, r.step & 0xffff));
    const bool below_360 = pan < 360;
    std::int32_t goal;
    if (!below_360 && r.target < old) goal = sx16(add(r.target & 0xffff, 360));     // 2f80
    else if (pan < 0 && old < r.target) goal = sx16(add(r.target & 0xffff, -360));  // 2fa8
    else goal = sx16(r.target);                                                    // 2fc0
    if ((r.step > 0 && goal < pan && old < goal) || (r.step < 0 && pan < goal && goal < old))
        pan = sx16(r.target);  // 3028: passed the target
    else if (pan < 0)
        pan = sx16(add(pan, 360));
    else if (!below_360)
        pan = sx16(add(pan, -360));
    GlideCall call;
    call.set_pan = pan;  // 3038: c1d4(+0, 0x7fffffff, pan)
    if (pan == r.target) s.glide.reset();  // 3054: ea8c
    else if (r.countdown == 1) r.count = r.reload;
    return call;
}

std::optional<GlideCall> glide_tick(GlideStream& s) {
    if (!s.glide) return std::nullopt;
    auto& r = *s.glide;
    if (r.flags & 0x80000000U) return std::nullopt;  // e8f0: locked
    if (r.countdown == 1) r.count = static_cast<std::int32_t>(static_cast<std::uint32_t>(r.count) - 1U);
    if (r.count != 0) return std::nullopt;
    return glide_callback(s);
}

void make_volume(const VolumeState& s, std::int32_t vol1, std::int32_t pan1, std::int32_t vol2, std::int32_t pan2,
                 std::int32_t vol3, std::int32_t pan3, std::array<std::int16_t, 2>& out) {
    auto t0 = mul(vol1, 258);
    t0 = div127(mul(t0, vol3));
    t0 = div127(mul(t0, vol2));
    volume_tail(s, t0, static_cast<std::int32_t>(static_cast<std::uint32_t>(pan1) + static_cast<std::uint32_t>(pan3) + static_cast<std::uint32_t>(pan2)), out);
}

void make_volume4(const VolumeState& s, std::int32_t vol4, std::int32_t vol1, std::int32_t pan1, std::int32_t vol3,
                  std::int32_t pan2, std::int32_t vol2, std::int32_t pan3, std::array<std::int16_t, 2>& out) {
    auto t0 = mul(vol1, 258);
    t0 = div127(mul(t0, vol2));
    t0 = div127(mul(t0, vol3));
    t0 = div127(mul(t0, vol4));
    volume_tail(s, t0, static_cast<std::int32_t>(static_cast<std::uint32_t>(pan1) + static_cast<std::uint32_t>(pan3) + static_cast<std::uint32_t>(pan2)), out);
}

}
