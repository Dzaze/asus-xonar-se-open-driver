// SPSC : un producteur de contrôle, un consommateur audio. La valeur la plus
// récente gagne. Trois cases possédées exclusivement, aucun verrou ni allocation.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace xonar::dsp {
template<class T> class LatestValue {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static constexpr std::uint32_t dirty = 4;
public:
    void reset() noexcept { producer_ = 0; consumer_ = 2; middle_.store(1); }
    void publish(const T& value) noexcept {
        slots_[producer_] = value;
        producer_ = middle_.exchange(producer_ | dirty, std::memory_order_acq_rel) & 3;
    }
    // Valide jusqu'au prochain consume() ; le producteur ne peut pas écrire ici.
    const T* consume() noexcept {
        if (!(middle_.load(std::memory_order_acquire) & dirty)) return nullptr;
        consumer_ = middle_.exchange(consumer_, std::memory_order_acq_rel) & 3;
        return &slots_[consumer_];
    }
private:
    std::array<T, 3> slots_{};
    std::atomic<std::uint32_t> middle_{1};
    std::uint32_t producer_ = 0;
    std::uint32_t consumer_ = 2;
};
}
