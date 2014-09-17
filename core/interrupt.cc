/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <algorithm>
#include <list>
#include <map>

#include <osv/sched.hh>
#include "drivers/pci-function.hh"
#include "exceptions.hh"
#include <osv/interrupt.hh>
#include "apic.hh"
#include <osv/trace.hh>

TRACEPOINT(trace_msix_interrupt, "vector=0x%02x", unsigned);
TRACEPOINT(trace_msix_migrate, "vector=0x%02x apic_id=0x%x",
                               unsigned, unsigned);

using namespace pci;

msix_vector::msix_vector(pci::function* dev)
    : _dev(dev)
{
    _vector = idt.register_handler([this] { interrupt(); });
}

msix_vector::~msix_vector()
{
    idt.unregister_handler(_vector);
}

pci::function* msix_vector::get_pci_function(void)
{
    return (_dev);
}

unsigned msix_vector::get_vector(void)
{
    return (_vector);
}

void msix_vector::msix_unmask_entries(void)
{
    for (auto entry_id : _entryids) {
        if (_dev->is_msix()) {
            _dev->msix_unmask_entry(entry_id);
        } else {
            _dev->msi_unmask_entry(entry_id);
        }
    }
}

void msix_vector::msix_mask_entries(void)
{
    for (auto entry_id : _entryids) {
        if (_dev->is_msix()) {
            _dev->msix_mask_entry(entry_id);
        } else {
            _dev->msi_mask_entry(entry_id);
        }
    }
}

void msix_vector::set_handler(std::function<void ()> handler)
{
    _handler = handler;
}

void msix_vector::add_entryid(unsigned entry_id)
{
    _entryids.push_back(entry_id);
}

void msix_vector::interrupt(void)
{
    trace_msix_interrupt(_vector);
    _handler();
}

void msix_vector::set_affinity(unsigned apic_id)
{
    msi_message msix_msg = apic->compose_msix(_vector, apic_id);
    for (auto entry_id : _entryids) {
        _dev->msix_write_entry(entry_id, msix_msg._addr, msix_msg._data);
    }
}

interrupt_manager::interrupt_manager(pci::function* dev)
    : _dev(dev)
{
}

interrupt_manager::~interrupt_manager()
{

}

void interrupt_manager::easy_unregister()
{
    free_vectors(_easy_vectors);
    _easy_vectors.clear();
}

std::vector<msix_vector*> interrupt_manager::request_vectors(unsigned num_vectors)
{
    std::vector<msix_vector*> results;
    unsigned num_entries;

    if (_dev->is_msix()) {
        num_entries = _dev->msix_get_num_entries();
    } else {
        num_entries = _dev->msi_get_num_entries();
    }

    auto num = std::min(num_vectors, num_entries);

    for (unsigned i = 0; i < num; ++i) {
        results.push_back(new msix_vector(_dev));
    }

    return (results);
}

bool interrupt_manager::assign_isr(msix_vector* vector, std::function<void ()> handler)
{
    vector->set_handler(handler);

    return (true);
}

bool interrupt_manager::setup_entry(unsigned entry_id, msix_vector* msix)
{
    auto vector = msix->get_vector();
    msi_message msix_msg = apic->compose_msix(vector, 0);

    if (msix_msg._addr == 0) {
        return (false);
    }

    if (_dev->is_msix()) {
        if (!_dev->msix_write_entry(entry_id, msix_msg._addr, msix_msg._data)) {
            return false;
        }
    } else {
        if (!_dev->msi_write_entry(entry_id, msix_msg._addr, msix_msg._data)) {
            return false;
        }
    }

    msix->add_entryid(entry_id);
    return (true);
}

void interrupt_manager::free_vectors(const std::vector<msix_vector*>& vectors)
{
    for (auto msix : vectors) {
        delete msix;
    }
}

bool interrupt_manager::unmask_interrupts(const std::vector<msix_vector*>& vectors)
{
    for (auto msix : vectors) {
        msix->msix_unmask_entries();
    }

    return (true);
}

inter_processor_interrupt::inter_processor_interrupt(std::function<void ()> handler)
    : _vector(idt.register_handler(handler))
{
}

inter_processor_interrupt::~inter_processor_interrupt()
{
    idt.unregister_handler(_vector);
}

void inter_processor_interrupt::send(sched::cpu* cpu)
{
    apic->ipi(cpu->arch.apic_id, _vector);
}

void inter_processor_interrupt::send_allbutself(){
    apic->ipi_allbutself(_vector);
}
