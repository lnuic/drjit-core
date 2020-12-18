#pragma once

#include <mutex>

#if !defined(likely)
#  if !defined(_MSC_VER)
#    define likely(x)   __builtin_expect(!!(x), 1)
#    define unlikely(x) __builtin_expect(!!(x), 0)
#  else
#    define unlikely(x) x
#    define likely(x) x
#  endif
#endif

#if defined(_WIN32)
#  define dlsym(ptr, name) GetProcAddress((HMODULE) ptr, name)
#endif

using lock_guard = std::unique_lock<std::mutex>;

/// RAII helper for *unlocking* a mutex
class unlock_guard {
public:
    unlock_guard(std::mutex &mutex) : m_mutex(mutex) {
        m_mutex.unlock();
    }

    ~unlock_guard() { m_mutex.lock(); }
    unlock_guard(const unlock_guard &) = delete;
    unlock_guard &operator=(const unlock_guard &) = delete;
private:
    std::mutex &m_mutex;
};

namespace detail {
    template <typename Func> struct scope_guard {
        scope_guard(const Func &func) : func(func) { }
        scope_guard(const scope_guard &) = delete;
        scope_guard(scope_guard &&g) : func(std::move(g.func)) {}
        scope_guard &operator=(const scope_guard &) = delete;
        scope_guard &operator=(scope_guard &&g) { func = std::move(g.func); }
    private:
        Func func;
    };
};

template <class Func>
detail::scope_guard<Func> scope_guard(const Func &func) {
    return detail::scope_guard<Func>(func);
}
