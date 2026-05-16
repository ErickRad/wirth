#pragma once

#include <stdint.h>

namespace kernel::sync {

class SpinLock {
public:
    SpinLock() : m_state(0) {}

    void lock() {
        while (!try_lock()) {
            asm volatile("pause");
        }
    }

    bool try_lock() {
        uint32_t previous = 1;
        asm volatile("xchg %0, %1" : "+r"(previous), "+m"(m_state) : : "memory");
        return previous == 0;
    }

    void unlock() {
        asm volatile("" ::: "memory");
        m_state = 0;
    }

private:
    volatile uint32_t m_state;
};

class LockGuard {
public:
    explicit LockGuard(SpinLock& lock) : m_lock(lock) {
        m_lock.lock();
    }

    ~LockGuard() {
        m_lock.unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    SpinLock& m_lock;
};

}  // namespace kernel::sync
