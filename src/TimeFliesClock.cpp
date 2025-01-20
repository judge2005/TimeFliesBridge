#include "TimeFliesClock.h"

TimeFliesClock::TimeFliesClock() {
}

bool TimeFliesClock::clockOn() {
	struct tm now;
	suseconds_t uSec;
    bool scheduledOn = false;

	if (getDisplayOn().value == getDisplayOff().value) {
		scheduledOn = true;
	} else if (pTimeSync) {
        pTimeSync->getLocalTime(&now, &uSec);

        if (getDisplayOn().value < getDisplayOff().value) {
            scheduledOn = now.tm_hour >= getDisplayOn().value && now.tm_hour < getDisplayOff().value;
        } else if (getDisplayOn().value > getDisplayOff().value) {
            scheduledOn = !(now.tm_hour >= getDisplayOff().value && now.tm_hour < getDisplayOn().value);
        }
    }

	return scheduledOn;
}
