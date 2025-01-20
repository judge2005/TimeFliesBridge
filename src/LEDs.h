#ifndef _LEDS_H
#define _LEDS_H

#include <ConfigItem.h>
class LEDs {
public:
    static BooleanConfigItem& getBacklights() { static BooleanConfigItem backlights("backlights", false); return backlights; }
    static ByteConfigItem& getBacklightRed() { static ByteConfigItem backlight_red("backlight_red", 7); return backlight_red; }
    static ByteConfigItem& getBacklightGreen() { static ByteConfigItem backlight_green("backlight_green", 7); return backlight_green; }
    static ByteConfigItem& getBacklightBlue() { static ByteConfigItem backlight_blue("backlight_blue", 7); return backlight_blue; }

    static BooleanConfigItem& getUnderlights() { static BooleanConfigItem underlights("underlights", false); return underlights; }
    static ByteConfigItem& getUnderlightRed() { static ByteConfigItem underlight_red("underlight_red", 7); return underlight_red; }
    static ByteConfigItem& getUnderlightGreen() { static ByteConfigItem underlight_green("underlight_green", 7); return underlight_green; }
    static ByteConfigItem& getUnderlightBlue() { static ByteConfigItem underlight_blue("underlight_blue", 7); return underlight_blue; }

    static BooleanConfigItem& getBaselights() { static BooleanConfigItem baselights("baselights", false); return baselights; }
    static ByteConfigItem& getBaselightRed() { static ByteConfigItem baselight_red("baselight_red", 7); return baselight_red; }
    static ByteConfigItem& getBaselightGreen() { static ByteConfigItem baselight_green("baselight_green", 7); return baselight_green; }
    static ByteConfigItem& getBaselightBlue() { static ByteConfigItem baselight_blue("baselight_blue", 7); return baselight_blue; }
};

#endif