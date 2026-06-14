#include "imt.h"

#include <string.h>

/* Error clamp: e = (T - L)/T is at most 1.0 and is bounded below to keep
 * the 64-bit intermediates far away from overflow on absurd inputs. */
#define IMT_PACE_ERROR_MIN_Q16 ((int64_t)-8 * IMT_PACE_Q16_ONE)
#define IMT_PACE_ERROR_MAX_Q16 ((int64_t)IMT_PACE_Q16_ONE)

/* R8.3: decay speed toward 1.0 is 10% (0.1 in rate units) per second. */
#define IMT_PACE_DECAY_PER_SEC_Q16 6554
#define IMT_PACE_NS_PER_SEC 1000000000

static int32_t imt_pace_clamp_rate(int64_t rate_q16)
{
    if (rate_q16 < IMT_PACE_RATE_MIN_Q16) {
        return IMT_PACE_RATE_MIN_Q16;
    }
    if (rate_q16 > IMT_PACE_RATE_MAX_Q16) {
        return IMT_PACE_RATE_MAX_Q16;
    }
    return (int32_t)rate_q16;
}

int imt_pace_init(ImtPace* pace, int64_t target_latency_ns)
{
    if (!pace || target_latency_ns <= 0) {
        return -1;
    }

    memset(pace, 0, sizeof(*pace));
    pace->target_latency_ns = target_latency_ns;
    pace->kp_q16 = IMT_PACE_DEFAULT_KP_Q16;
    pace->ki_q16 = IMT_PACE_DEFAULT_KI_Q16;
    pace->rate_q16 = IMT_PACE_Q16_ONE;
    /* Anti-windup: ki*integ alone must not push r outside [0.1, 2.0]. */
    pace->integ_max_q16 = ((int64_t)(IMT_PACE_RATE_MAX_Q16 - IMT_PACE_Q16_ONE) << 16) /
                          pace->ki_q16;
    pace->integ_min_q16 = -(((int64_t)(IMT_PACE_Q16_ONE - IMT_PACE_RATE_MIN_Q16) << 16) /
                            pace->ki_q16);
    return 0;
}

int imt_pace_update(ImtPace* pace, int64_t measured_latency_ns)
{
    int64_t error_q16;
    int64_t rate_q16;

    if (!pace || pace->target_latency_ns <= 0 || pace->ki_q16 <= 0 ||
        measured_latency_ns < 0) {
        return -1;
    }

    /* R8.2: e = (T - L)/T in Q16, 64-bit intermediates throughout. The
     * difference is pre-clamped so the Q16 shift cannot overflow. */
    {
        int64_t diff_ns = pace->target_latency_ns - measured_latency_ns;
        const int64_t diff_limit = (int64_t)1 << 46;
        if (diff_ns < -diff_limit) {
            diff_ns = -diff_limit;
        }
        if (diff_ns > diff_limit) {
            diff_ns = diff_limit;
        }
        error_q16 = (diff_ns * (int64_t)IMT_PACE_Q16_ONE) / pace->target_latency_ns;
    }
    if (error_q16 < IMT_PACE_ERROR_MIN_Q16) {
        error_q16 = IMT_PACE_ERROR_MIN_Q16;
    }
    if (error_q16 > IMT_PACE_ERROR_MAX_Q16) {
        error_q16 = IMT_PACE_ERROR_MAX_Q16;
    }

    pace->integ_q16 += error_q16;
    if (pace->integ_q16 > pace->integ_max_q16) {
        pace->integ_q16 = pace->integ_max_q16;
    }
    if (pace->integ_q16 < pace->integ_min_q16) {
        pace->integ_q16 = pace->integ_min_q16;
    }

    rate_q16 = (int64_t)IMT_PACE_Q16_ONE +
               (((int64_t)pace->kp_q16 * error_q16) >> 16) +
               (((int64_t)pace->ki_q16 * pace->integ_q16) >> 16);
    pace->rate_q16 = imt_pace_clamp_rate(rate_q16);
    return pace->rate_q16;
}

int imt_pace_idle(ImtPace* pace, int64_t elapsed_ns)
{
    int64_t step_q16;
    int64_t gap_q16;

    if (!pace || pace->ki_q16 <= 0 || elapsed_ns < 0) {
        return -1;
    }

    /* 100 s fully decays any gap (max |gap| is 1.0 -> 10 s at 0.1/s), and
     * the cap keeps the multiplication below within int64 range. */
    if (elapsed_ns > (int64_t)100 * IMT_PACE_NS_PER_SEC) {
        elapsed_ns = (int64_t)100 * IMT_PACE_NS_PER_SEC;
    }
    step_q16 = (elapsed_ns * IMT_PACE_DECAY_PER_SEC_Q16) / IMT_PACE_NS_PER_SEC;
    gap_q16 = (int64_t)pace->rate_q16 - IMT_PACE_Q16_ONE;
    if (gap_q16 > 0) {
        gap_q16 = gap_q16 > step_q16 ? gap_q16 - step_q16 : 0;
    } else if (gap_q16 < 0) {
        gap_q16 = gap_q16 < -step_q16 ? gap_q16 + step_q16 : 0;
    }
    pace->rate_q16 = imt_pace_clamp_rate(IMT_PACE_Q16_ONE + gap_q16);

    /* Keep the integrator consistent with the decayed rate so that the next
     * feedback sample resumes smoothly (r = 1 + ki*integ at e = 0). */
    pace->integ_q16 = (gap_q16 << 16) / pace->ki_q16;
    return pace->rate_q16;
}

int32_t imt_pace_rate_q16(const ImtPace* pace)
{
    return pace ? pace->rate_q16 : IMT_PACE_Q16_ONE;
}
