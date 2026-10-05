#pragma once

// Fixed-capacity circular buffer over contiguous storage.
//
// The contiguous layout is deliberate: ImPlot's PlotLine/PlotShaded overloads
// accept an `offset` that it applies modulo `count`, so a full ring can be
// drawn straight from `data()` with `offset()` and zero copying.

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "core/StringUtil.h"  // em::Numeric

namespace em {

template <Numeric T>
class RingBuffer {
public:
    RingBuffer() = default;

    explicit RingBuffer(std::size_t capacity) : storage_(capacity) {}

    void reserveCapacity(std::size_t capacity) {
        if (capacity == storage_.size()) return;
        // Preserve the most recent min(count, capacity) samples, oldest first.
        std::vector<T> kept;
        const std::size_t keep = std::min(count_, capacity);
        kept.reserve(keep);
        for (std::size_t i = count_ - keep; i < count_; ++i) kept.push_back((*this)[i]);

        storage_.assign(capacity, T{});
        count_ = 0;
        head_ = 0;
        for (const T& v : kept) push(v);
    }

    void push(T value) {
        if (storage_.empty()) return;
        storage_[head_] = value;
        head_ = (head_ + 1) % storage_.size();
        if (count_ < storage_.size()) ++count_;
    }

    void clear() noexcept {
        count_ = 0;
        head_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] bool full() const noexcept { return count_ == storage_.size(); }

    [[nodiscard]] const T* data() const noexcept { return storage_.data(); }

    // Index to hand to ImPlot alongside data() and size().
    [[nodiscard]] int plotOffset() const noexcept {
        return full() ? static_cast<int>(head_) : 0;
    }

    // Logical index: 0 is the oldest retained sample.
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept {
        const std::size_t base = full() ? head_ : 0;
        return storage_[(base + i) % storage_.size()];
    }

    [[nodiscard]] T back() const noexcept {
        return count_ == 0 ? T{} : (*this)[count_ - 1];
    }

    [[nodiscard]] T latestOr(T fallback) const noexcept {
        return count_ == 0 ? fallback : back();
    }

    // Copies oldest-first into `out`, which must hold at least size() items.
    void copyOrdered(std::span<T> out) const {
        const std::size_t n = std::min(out.size(), count_);
        for (std::size_t i = 0; i < n; ++i) out[i] = (*this)[i];
    }

    [[nodiscard]] T max() const noexcept {
        T best{};
        for (std::size_t i = 0; i < count_; ++i) best = std::max(best, (*this)[i]);
        return best;
    }

    [[nodiscard]] double mean() const noexcept {
        if (count_ == 0) return 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < count_; ++i) sum += static_cast<double>((*this)[i]);
        return sum / static_cast<double>(count_);
    }

private:
    std::vector<T> storage_;
    std::size_t count_ = 0;
    std::size_t head_ = 0;
};

// Percentile over an unsorted range, using the nearest-rank method that
// `dumpsys gfxinfo` itself reports. `values` is sorted in place.
[[nodiscard]] inline double percentileInPlace(std::span<double> values, double pct) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double rank = (pct / 100.0) * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(rank);
    const std::size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = rank - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

}  // namespace em
