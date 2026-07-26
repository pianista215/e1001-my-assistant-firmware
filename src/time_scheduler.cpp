#include "time_scheduler.h"

namespace {

// mktime() normalizes overflowed fields (e.g. tm_hour==24 rolls into the
// next day) and returns the UTC epoch for the given local time; localtime_r
// converts back so callers see fully normalized, DST-correct tm fields.
time_t normalize(struct tm& t) {
    t.tm_isdst = -1;  // let mktime figure out DST for this date/time
    const time_t epoch = mktime(&t);
    localtime_r(&epoch, &t);
    return epoch;
}

}  // namespace

WakeDecision computeNextWake(struct tm nowLocal,
                              int nightSkipFromHour,
                              int nightSkipToHour,
                              int nightSkipTargetHour) {
    struct tm now = nowLocal;
    const time_t nowEpoch = normalize(now);

    struct tm target = now;
    target.tm_min = 0;
    target.tm_sec = 0;
    target.tm_hour += 1;
    time_t targetEpoch = normalize(target);

    if (target.tm_hour >= nightSkipFromHour && target.tm_hour <= nightSkipToHour) {
        target.tm_hour = nightSkipTargetHour;
        target.tm_min = 0;
        target.tm_sec = 0;
        targetEpoch = normalize(target);
    }

    int64_t sleepSeconds = static_cast<int64_t>(targetEpoch) - static_cast<int64_t>(nowEpoch);
    if (sleepSeconds <= 0) sleepSeconds = 60;  // defensive; structurally shouldn't happen

    WakeDecision result;
    result.target = target;
    result.sleepSeconds = sleepSeconds;
    return result;
}
