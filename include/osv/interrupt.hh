/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef INTERRUPT_HH_
#define INTERRUPT_HH_

#include <functional>
#include <map>
#include <list>

#include <osv/sched.hh>
#include "drivers/pci.hh"
#include "drivers/pci-function.hh"
#include <osv/types.h>
#include <initializer_list>
#include <boost/optional.hpp>

// max vectors per request
const int max_vectors = 256;

class msix_vector {
public:
    msix_vector(pci::function* dev);
    virtual ~msix_vector();

    pci::function* get_pci_function(void);
    unsigned get_vector(void);
    void msix_unmask_entries(void);
    void msix_mask_entries(void);

    void add_entryid(unsigned entry_id);
    void interrupt(void);
    void set_handler(std::function<void ()> handler);
    void set_affinity(unsigned apic_id);

private:
    // Handler to invoke...
    std::function<void ()> _handler;
    // The device that owns this vector
    pci::function * _dev;
    // Entry ids used by this vector
    std::list<unsigned> _entryids;
    unsigned _vector;
};

// entry -> thread to wake
template <class T>
struct msix_binding {
    unsigned entry;
    // high priority ISR
    std::function<void ()> isr;
    // bottom half
    T *t;
};

class interrupt_manager {
public:

    explicit interrupt_manager(pci::function* dev);
    ~interrupt_manager();

    ////////////////////
    // Easy Interface //
    ////////////////////

    // 1. Enabled MSI-x For device
    // 2. Allocate vectors and assign ISRs
    // 3. Setup entries
    // 4. Unmask interrupts
    template <class T>
    bool easy_register(std::initializer_list<msix_binding<T>> bindings);
    void easy_unregister();

    /////////////////////
    // Multi Interface //
    /////////////////////

    std::vector<msix_vector*> request_vectors(unsigned num_vectors);
    void free_vectors(const std::vector<msix_vector*>& vectors);
    bool assign_isr(msix_vector*, std::function<void ()> handler);
    // Multiple entry can be assigned the same vector
    bool setup_entry(unsigned entry_id, msix_vector* vector);
    // unmasks all interrupts
    bool unmask_interrupts(const std::vector<msix_vector*>& vectors);

private:
    pci::function* _dev;
    // Used by the easy interface
    std::vector<msix_vector*> _easy_vectors;
};

/**
 * Changes the affinity of the MSI-X vector to the same CPU where its service
 * routine thread is bound and then wakes that thread.
 *
 * @param current The CPU to which the MSI-X vector is currently bound
 * @param v MSI-X vector handle
 * @param t interrupt service routine thread
 */
template <class T>
static inline void set_affinity_and_wake(
    sched::cpu*& current, msix_vector* v, T* t)
{
    auto cpu = t->get_cpu();

    if (cpu != current) {

        //
        // According to PCI spec chapter 6.8.3.5 the MSI-X table entry may be
        // updated only if the entry is masked and the new values are promissed
        // to be read only when the entry is unmasked.
        //
        v->msix_mask_entries();

        std::atomic_thread_fence(std::memory_order_seq_cst);

        current = cpu;
        v->set_affinity(cpu->arch.apic_id);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        v->msix_unmask_entries();
    }

    t->wake();
}

template <class T>
bool interrupt_manager::easy_register(std::initializer_list<msix_binding<T>>
                                      bindings)
{
    unsigned n = bindings.size();

    std::vector<msix_vector*> assigned = request_vectors(n);

    if (assigned.size() != n) {
        free_vectors(assigned);
        return (false);
    }

    // Enable the device msix capability,
    // masks all interrupts...

    if (_dev->is_msix()) {
        _dev->msix_enable();
    } else {
        _dev->msi_enable();
    }

    int idx=0;

    for (auto binding : bindings) {
        msix_vector* vec = assigned[idx++];
        auto isr = binding.isr;
        auto t = binding.t;

        bool assign_ok;

        if (t) {
            sched::cpu* current = nullptr;
            assign_ok =
                assign_isr(vec,
                    [=]() mutable {
                                    if (isr)
                                        isr();
                                    set_affinity_and_wake(current, vec, t);
                                  });
        } else {
            assign_ok = assign_isr(vec, [=]() { if (isr) isr(); });
        }

        if (!assign_ok) {
            free_vectors(assigned);
            return false;
        }
        bool setup_ok = setup_entry(binding.entry, vec);
        if (!setup_ok) {
            free_vectors(assigned);
            return false;
        }
    }

    // Save reference for assigned vectors
    _easy_vectors = assigned;
    unmask_interrupts(assigned);

    return (true);
}

class inter_processor_interrupt {
public:
    explicit inter_processor_interrupt(std::function<void ()>);
    ~inter_processor_interrupt();
    void send(sched::cpu* cpu);
    void send_allbutself();
private:
    unsigned _vector;
};

class gsi_interrupt {
public:
    void set(unsigned gsi, unsigned vector);
    void clear();
private:
    unsigned _gsi;
};

class gsi_edge_interrupt {
public:
    gsi_edge_interrupt(unsigned gsi, std::function<void ()> handler);
    ~gsi_edge_interrupt();
private:
    unsigned _vector;
    gsi_interrupt _gsi;
};

class gsi_level_interrupt {
public:
    gsi_level_interrupt() {};
    gsi_level_interrupt(unsigned gsi,
                        std::function<bool ()> ack,
                        std::function<void ()> handler);
    ~gsi_level_interrupt();

    void set_ack_and_handler(unsigned gsi,
            std::function<bool ()> ack,
            std::function<void ()> handler);
private:
    boost::optional<shared_vector> _vector;
    gsi_interrupt _gsi;
};

#endif /* INTERRUPT_HH_ */
