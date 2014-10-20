#ifndef _LOW_LATENCY_HH
#define _LOW_LATENCY_HH

namespace osv {
namespace algorithm {

/*
 * 1.1 is needed to ensure (0.1 * 10 <= min_priority) and thus thread's
 * priority is able to reach 1.
 */
static constexpr float min_priority      = 1.1;

static constexpr float prio_step_down    = 10;
static constexpr float prio_step_up      = 10;
static constexpr int time_thresh_ns      = 100000000; // 100ms
static constexpr u64 work_thresh         = 10000;

enum update_state {
    prio_up,
    prio_down,
    prio_unchanged
};

class dynamic_thread_priority {
public:


    dynamic_thread_priority(u64 idle_low_thresh,
                                   u64 idle_high_thresh):
        _last_idle_clock(get_system_idle_time()),
        _work(0),
        _start(osv::clock::uptime::now()), _idle_low_thresh(idle_low_thresh),
        _idle_high_thresh(idle_high_thresh) {}

    sched::thread_runtime::duration get_system_idle_time() {
        sched::thread_runtime::duration idle_time(0);
        for (auto c : sched::cpus) {
            idle_time += c->idle_thread->thread_clock();
        }

        return idle_time;
    }

    enum update_state update(int new_work) {
        using namespace std::chrono;
        enum update_state rc = prio_unchanged;

        _work += new_work;

        // Don't check time to often - that's quite expensive.
        if (_work < work_thresh) {
            return rc;
        } else {
            _work = 0;
        }

        //
        // If "time_thresh_ms" time has passed we'll check the average work rate
        // and update thread's priority appropriately.
        //
        auto now = osv::clock::uptime::now();
        auto diff = now - _start;
        if (duration_cast<nanoseconds>(diff) >= nanoseconds(time_thresh_ns)) {
            sched::thread *current = sched::thread::current();
            float cur_prio = current->priority();

            auto cur_idle_clock = get_system_idle_time();
            auto idle_time_since_start = cur_idle_clock - _last_idle_clock;

            u64 average_idle_time =
                100 * duration_cast<nanoseconds>(idle_time_since_start).count() /
                duration_cast<nanoseconds>(diff).count();

            // Decrease a priority if CPU idle time is too low
            if (average_idle_time <= _idle_low_thresh) {
                if (cur_prio * prio_step_down <= min_priority) {
                    cur_prio *= prio_step_down;
                    current->set_priority(cur_prio);
                }

                rc = prio_down;

            // Increase the priority if there is enough idle CPU
            } else if (average_idle_time >= _idle_high_thresh) {
                if (cur_prio / prio_step_up >=
                                    sched::thread::priority_infinity) {
                    cur_prio /= prio_step_up;
                    current->set_priority(cur_prio);
                }

                rc = prio_up;
            }

            printf("CPU[%d]: %s: idle %d, cur_prio * 100000 %d\n",
                   sched::current_cpu->id, current->name().c_str(),
                   average_idle_time,
                   (u64)(cur_prio * 100000));

            _last_idle_clock = cur_idle_clock;
            _start = osv::clock::uptime::now();
        }

        return rc;
    }
private:
    sched::thread_runtime::duration _last_idle_clock;
    u64 _work;
    osv::clock::uptime::time_point _start;
    const u64 _idle_low_thresh;
    const u64 _idle_high_thresh;
};



} // namespace low_latency
} // osv
#endif // _LOW_LATENCY_HH
