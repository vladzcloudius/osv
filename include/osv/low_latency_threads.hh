#ifndef _LOW_LATENCY_HH
#define _LOW_LATENCY_HH

namespace osv {
namespace low_latency {
/*
 * This "strange" values are due to the "strange" priority value semantics in
 * OSv: the higher priority value corresponds to a lower thread priority.
 */
static const float min_priority   = 1;
static const float max_priority   = 0.001;
static const float prio_step_down = 10;
static const float prio_step_up   = 1.00023; // 10^0.0001
static const u64   packets_thresh = 128;

static inline void update_thread_prio(u64 packets)
{
    sched::thread *current = sched::thread::current();
    float cur_prio = current->priority();

    if (packets >= packets_thresh) {
        if (cur_prio * prio_step_down <= min_priority) {
            cur_prio *= prio_step_down;
            current->set_priority(cur_prio);
        }
    } else {
        if (cur_prio / prio_step_up >= max_priority) {
            cur_prio /= prio_step_up;
            current->set_priority(cur_prio);
        }
    }
}

} // namespace low_latency
} // osv
#endif // _LOW_LATENCY_HH
