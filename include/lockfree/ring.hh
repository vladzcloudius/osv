/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

//
// single-producer / single-consumer lockless ring buffer of fixed size.
//
#ifndef __LF_RING_HH__
#define __LF_RING_HH__

#include <atomic>
#include <sched.hh>
#include <arch.hh>
#include <ilog2.hh>
#include "debug.hh"

//
// spsc ring of fixed size
//
template<class T>
class ring_spsc {
public:
    ring_spsc(unsigned max_size): _begin(0), _end(0), _max_size(max_size),
                                  _max_size_mask(max_size - 1)
    {
        assert(is_power_of_two(_max_size));
        _ring = new T[_max_size];
    }

    ~ring_spsc()
    {
        delete[] _ring;
    }

    bool push(const T& element)
    {
        unsigned end = _end.load(std::memory_order_relaxed);

        // Consider making it a DEBUG_ASSERT() since it's going to cause a
        // false sharing on a "_begin" cache line
        if (size() >= _max_size) {
            return false;
        }

        _ring[end & _max_size_mask] = element;
        _end.store(end + 1, std::memory_order_relaxed);

        return true;
    }

    bool pop(T& element)
    {
        unsigned beg = _begin.load(std::memory_order_relaxed);

        // Consider making it a DEBUG_ASSERT() since it's going to cause a
        // false sharing on a "_begin" cache line
        if (empty()) {
            return false;
        }

        element = _ring[beg & _max_size_mask];
        _begin.store(beg + 1, std::memory_order_relaxed);

        return true;
    }

    bool empty() const {
        return size() == 0;
    }

    T& front() const {
        DEBUG_ASSERT(!empty(), "calling front() on an empty queue!");

        unsigned beg = _begin.load(std::memory_order_relaxed);

        return _ring[beg & _max_size_mask];
    }

    unsigned size() const {
        unsigned end = _end.load(std::memory_order_relaxed);
        unsigned beg = _begin.load(std::memory_order_relaxed);

        return (end - beg);
    }

private:
    std::atomic<unsigned> _begin CACHELINE_ALIGNED;
    std::atomic<unsigned> _end CACHELINE_ALIGNED;
    const unsigned _max_size;
    const unsigned _max_size_mask;
    T* _ring;
};

//
// mpsc ring of fixed size
//
template<class T, unsigned MaxSize>
class ring_mpsc {
public:
    ring_mpsc(): _insert_idx(0), _begin(0), _end(0), _empty() {
        for(unsigned i=0; i < MaxSize; i++) {
            _ring[i] = _empty;
        }
    }

    unsigned push(const T& element)
    {
        assert(element != _empty);

        unsigned beg = _begin.load(std::memory_order_relaxed);
        unsigned in_idx = _insert_idx.fetch_add(1);

        if (in_idx - beg >= MaxSize) {
            return in_idx;
        }

        _ring[in_idx % MaxSize].store(element, std::memory_order_relaxed);
        _end.fetch_add(1);

        return 0;
    }

    bool push_to(const T& element, unsigned in_idx)
    {
        unsigned beg = _begin.load(std::memory_order_relaxed);

        if (in_idx - beg >= MaxSize) {
            return false;
        }

        _ring[in_idx % MaxSize].store(element, std::memory_order_relaxed);
        _end.fetch_add(1);

        return true;
    }

    bool pop(T& element)
    {
        unsigned beg = _begin.load(std::memory_order_relaxed);
        unsigned end = _end.load(std::memory_order_acquire);

        if (beg >= end) {
            return false;
        }

        element = _ring[beg % MaxSize].load(std::memory_order_relaxed);
        if (element == _empty) {
            return false;
        }

        _ring[beg % MaxSize].store(_empty, std::memory_order_relaxed);
        _begin.store(beg + 1, std::memory_order_release);

        return true;
    }

    unsigned size() {
        unsigned end = _end.load(std::memory_order_relaxed);
        unsigned beg = _begin.load(std::memory_order_relaxed);

        return (end - beg);
    }

private:
    std::atomic<unsigned> _insert_idx CACHELINE_ALIGNED;
    std::atomic<unsigned> _begin CACHELINE_ALIGNED;
    std::atomic<unsigned> _end CACHELINE_ALIGNED;

    // FIXME: use iterator instead of _empty
    T _empty;
    std::atomic<T> _ring[MaxSize];

};

#endif // !__LF_RING_HH__
