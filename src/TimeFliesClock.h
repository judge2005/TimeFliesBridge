#ifndef _TIME_FLIES_CLOCK_H
#define _TIME_FLIES_CLOCK_H

#include <ConfigItem.h>
#include <TimeSync.h>

#include "ClockTimer.h"

class TimeFliesClock {
public:
    TimeFliesClock();

    static IntConfigItem& getTimeOrDate() { static IntConfigItem time_or_date("time_or_date", 0); return time_or_date; }	// time
    static ByteConfigItem& getDateFormat() { static ByteConfigItem date_format("date_format", 1); return date_format; }			// mm-dd, dd-mm
    static BooleanConfigItem& getHourFormat() { static BooleanConfigItem hour_format("hour_format", true); return hour_format; }	// 12/24 hour
    static ByteConfigItem& getOffStateOff() { static ByteConfigItem off_state_off("off_state_off", 1); return off_state_off; }	// When blanking period is active, turn clock off or dim it
    static ByteConfigItem& getDisplayOn() { static ByteConfigItem display_on("display_on", 0); return display_on; }
    static ByteConfigItem& getDisplayOff() { static ByteConfigItem display_off("display_off", 24); return display_off; }
    static StringConfigItem& getTimeZone() { static StringConfigItem time_zone("time_zone", 63, "EST5EDT,M3.2.0,M11.1.0"); return time_zone; }	// POSIX timezone format
    static StringConfigItem& getCommand() { static StringConfigItem command("command", 63, ""); return command; }	// POSIX timezone format
    static IntConfigItem& getDimming() { static IntConfigItem dimming("dimming", 2); return dimming; }

    bool clockOn();
    void setTimeSync(TimeSync *pTimeSync) { this->pTimeSync = pTimeSync; }
private:

	TimeSync *pTimeSync = 0;
};

#endif