#ifndef _LOW_LATENCY_HH
#define _LOW_LATENCY_HH

namespace osv {
namespace algorithm {


class dynamic_thread_priority {
public:
    /*
     * This "strange" values are due to the "strange" priority value semantics
     * in OSv: the higher priority value corresponds to a lower thread priority.
     */

    /*
     * 1.1 is needed to ensure (0.1 * 10 <= min_priority) and thus thread's
     * priority is able to reach 1.
     */
    static constexpr float min_priority      = 1.1;

    static constexpr float max_priority      = 0.001;
    static constexpr float prio_step_down    = 10;
    static constexpr float prio_step_up      = 10;
    static constexpr int time_thresh_ms      = 100;

    dynamic_thread_priority(double work_thresh):
        _work(0), _check_points(0),
        _work_thresh(work_thresh),_start(osv::clock::uptime::now()) {}

    void update(u64 new_work) {
        using namespace std::chrono;

        _check_points++;
        _work += new_work;

        //
        // If threshold time has passed - check the average work rate and update
        // thread's priority.
        //
        auto now = osv::clock::uptime::now();
        auto diff = now - _start;
        if (duration_cast<milliseconds>(diff) >= milliseconds(time_thresh_ms)) {
            sched::thread *current = sched::thread::current();
            float cur_prio = current->priority();
            double average_work_rate =
                static_cast<double>(_work) / _check_points;

            if (average_work_rate >= _work_thresh) {
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

            _work = _check_points = 0;
            _start = osv::clock::uptime::now();
        }
    }
private:
    u64 _work;
    u64 _check_points;
    double _work_thresh;
    osv::clock::uptime::time_point _start;
};



} // namespace low_latency
} // osv
#endif // _LOW_LATENCY_HH
