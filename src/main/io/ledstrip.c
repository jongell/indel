/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

#include <common/maths.h>

#include "drivers/light_ws2811strip.h"
#include "drivers/system.h"

#include "sensors/battery.h"

#include "config/runtime_config.h"
#include "config/config.h"
#include "rx/rx.h"
#include "io/rc_controls.h"

#include "io/ledstrip.h"

#define LED_WHITE  {255, 255, 255}
#define LED_BLACK  {0,   0,   0  }
#define LED_RED    {255, 0,   0  }
#define LED_GREEN  {0,   255, 0  }
#define LED_BLUE   {0,   0,   255}
#define LED_CYAN   {0,   255, 255}
#define LED_YELLOW {255, 255, 0  }
#define LED_ORANGE {255, 128, 0  }
#define LED_PINK   {255, 0,   128}
#define LED_PURPLE {192, 64,  255}

/*
 * 0..5   - rear right cluster,  0..2 rear 3..5 right
 * 6..11  - front right cluster, 6..8 rear, 9..11 front
 * 12..15 - front center cluster
 * 16..21 - front left cluster,  16..18 front, 19..21 rear
 * 22..27 - rear left cluster,   22..24 left, 25..27 rear
 */

typedef enum {
    LED_DISABLED = 0,
    LED_DIRECTION_NORTH    = (1 << 0),
    LED_DIRECTION_EAST     = (1 << 1),
    LED_DIRECTION_SOUTH    = (1 << 2),
    LED_DIRECTION_WEST     = (1 << 3),
    LED_DIRECTION_UP       = (1 << 4),
    LED_DIRECTION_DOWN     = (1 << 5),
    LED_FUNCTION_INDICATOR = (1 << 6),
    LED_FUNCTION_BATTERY   = (1 << 7),
    LED_FUNCTION_MODE      = (1 << 8)
} ledFlag_e;

#define LED_X_BIT_OFFSET 4
#define LED_Y_BIT_OFFSET 0

#define LED_X_MASK (0xF0)
#define LED_Y_MASK (0x0F)

#define LED_X(ledConfig) ((ledConfig->xy & LED_X_MASK) >> LED_X_BIT_OFFSET)
#define LED_Y(ledConfig) ((ledConfig->xy & LED_Y_MASK) >> LED_Y_BIT_OFFSET)

#define LED_XY(x,y) (((x & LED_X_MASK) << LED_X_BIT_OFFSET) | ((y & LED_Y_MASK) << LED_Y_BIT_OFFSET))

typedef struct ledConfig_s {
    uint8_t xy; // see LED_X/Y_MASK defines
    uint16_t flags; // see ledFlag_e
} ledConfig_t;

static uint8_t ledGridWidth;
static uint8_t ledGridHeight;

static const ledConfig_t ledConfigs[WS2811_LED_STRIP_LENGTH] = {
        { LED_XY( 9,  9), LED_DIRECTION_SOUTH | LED_FUNCTION_MODE | LED_FUNCTION_BATTERY },
        { LED_XY(10, 10), LED_DIRECTION_SOUTH | LED_FUNCTION_MODE | LED_FUNCTION_BATTERY },
        { LED_XY(11, 11), LED_DIRECTION_SOUTH | LED_FUNCTION_INDICATOR },
        { LED_XY(11, 11), LED_DIRECTION_EAST  | LED_FUNCTION_INDICATOR },
        { LED_XY(10, 10), LED_DIRECTION_EAST  | LED_FUNCTION_MODE },
        { LED_XY( 9,  9), LED_DIRECTION_EAST  | LED_FUNCTION_MODE },

        { LED_XY(10,  5), LED_DIRECTION_SOUTH | LED_FUNCTION_MODE },
        { LED_XY(11,  4), LED_DIRECTION_SOUTH | LED_FUNCTION_MODE },
        { LED_XY(12,  3), LED_DIRECTION_SOUTH | LED_FUNCTION_INDICATOR },
        { LED_XY(12,  2), LED_DIRECTION_NORTH | LED_FUNCTION_INDICATOR },
        { LED_XY(11,  1), LED_DIRECTION_NORTH | LED_FUNCTION_MODE },
        { LED_XY(10,  0), LED_DIRECTION_NORTH | LED_FUNCTION_MODE },

        { LED_XY( 7,  0), LED_DIRECTION_NORTH | LED_FUNCTION_MODE | LED_FUNCTION_BATTERY },
        { LED_XY( 6,  0), LED_DIRECTION_NORTH | LED_FUNCTION_MODE | LED_FUNCTION_BATTERY },
        { LED_XY( 5,  0), LED_DIRECTION_NORTH | LED_FUNCTION_MODE | LED_FUNCTION_BATTERY },
        { LED_XY( 4,  0), LED_DIRECTION_NORTH | LED_FUNCTION_MODE | LED_FUNCTION_BATTERY },

        { LED_XY( 2,  0), LED_DIRECTION_NORTH | LED_FUNCTION_MODE },
        { LED_XY( 1,  1), LED_DIRECTION_NORTH | LED_FUNCTION_MODE },
        { LED_XY( 0,  2), LED_DIRECTION_NORTH | LED_FUNCTION_INDICATOR },
        { LED_XY( 0,  3), LED_DIRECTION_WEST  | LED_FUNCTION_INDICATOR },
        { LED_XY( 1,  4), LED_DIRECTION_WEST  | LED_FUNCTION_MODE },
        { LED_XY( 2,  5), LED_DIRECTION_WEST  | LED_FUNCTION_MODE },

        { LED_XY( 2,  9), LED_DIRECTION_WEST  | LED_FUNCTION_MODE },
        { LED_XY( 1, 10), LED_DIRECTION_WEST  | LED_FUNCTION_MODE },
        { LED_XY( 0, 11), LED_DIRECTION_WEST  | LED_FUNCTION_INDICATOR },
        { LED_XY( 0, 11), LED_DIRECTION_SOUTH | LED_FUNCTION_INDICATOR },
        { LED_XY( 1, 10), LED_DIRECTION_SOUTH | LED_FUNCTION_MODE | LED_FUNCTION_BATTERY },
        { LED_XY( 2,  9), LED_DIRECTION_SOUTH | LED_FUNCTION_MODE | LED_FUNCTION_BATTERY }
};

uint32_t nextIndicatorFlashAt = 0;
uint32_t nextBatteryFlashAt = 0;

#define LED_STRIP_10HZ ((1000 * 1000) / 10)
#define LED_STRIP_5HZ ((1000 * 1000) / 5)

#define LED_DIRECTION_COUNT 6

struct modeColors_s {
    rgbColor24bpp_t north;
    rgbColor24bpp_t east;
    rgbColor24bpp_t south;
    rgbColor24bpp_t west;
    rgbColor24bpp_t up;
    rgbColor24bpp_t down;
};

typedef union {
    rgbColor24bpp_t raw[LED_DIRECTION_COUNT];
    struct modeColors_s colors;
} modeColors_t;

static const modeColors_t orientationColors = {
    .raw = {
        {LED_WHITE},
        {LED_BLUE},
        {LED_RED},
        {LED_GREEN},
        {LED_PURPLE},
        {LED_CYAN}
    }
};

void applyLEDModeLayer(void)
{
    const ledConfig_t *ledConfig;

    uint8_t highestYValueForNorth = (ledGridHeight / 2) - 1;
    highestYValueForNorth &= ~(1 << 0); // make even

    uint8_t lowestYValueForSouth = (ledGridHeight / 2) - 1;
    if (lowestYValueForSouth & 1) {
        lowestYValueForSouth = min(lowestYValueForSouth + 1, ledGridHeight - 1);
    }

    uint8_t ledIndex;
    for (ledIndex = 0; ledIndex < WS2811_LED_STRIP_LENGTH; ledIndex++) {

        ledConfig = &ledConfigs[ledIndex];

        if (!(ledConfig->flags & LED_FUNCTION_MODE)) {
            setLedColor(ledIndex, &black);
            continue;
        }

        if (ledConfig->flags & LED_DIRECTION_NORTH && LED_Y(ledConfig) < highestYValueForNorth) {
            setLedColor(ledIndex, &orientationColors.colors.north);
            continue;
        }

        if (ledConfig->flags & LED_DIRECTION_SOUTH && LED_Y(ledConfig) >= lowestYValueForSouth) {
            setLedColor(ledIndex, &orientationColors.colors.south);
            continue;
        }

        setLedColor(ledIndex, &black);
    }
/*
    if (f.ARMED) {
        setStripColors(stripOrientation);
    } else {
        setStripColors(stripReds);
    }

    if (f.HEADFREE_MODE) {
        setStripColors(stripHeadfree);
#ifdef MAG
    } else if (f.MAG_MODE) {
        setStripColors(stripMag);
#endif
    } else if (f.HORIZON_MODE) {
        setStripColors(stripHorizon);
    } else if (f.ANGLE_MODE) {
        setStripColors(stripAngle);
    }
*/
}
void updateLedStrip(void)
{
    if (!isWS2811LedStripReady()) {
        return;
    }

    uint32_t now = micros();

    bool indicatorFlashNow = (int32_t)(now - nextIndicatorFlashAt) >= 0L;
    bool batteryFlashNow = (int32_t)(now - nextBatteryFlashAt) >= 0L;

    if (!(batteryFlashNow || indicatorFlashNow)) {
        return;
    }

    static uint8_t indicatorFlashState = 0;
    static uint8_t batteryFlashState = 0;

    static const rgbColor24bpp_t *flashColor;

    // LAYER 1

    applyLEDModeLayer();

    // LAYER 2

    if (batteryFlashNow) {
        nextBatteryFlashAt = now + LED_STRIP_10HZ;

        if (batteryFlashState == 0) {
            batteryFlashState = 1;
        } else {
            batteryFlashState = 0;
        }
    }

    if (batteryFlashState == 1 && feature(FEATURE_VBAT) && shouldSoundBatteryAlarm()) {
        setStripColor(&black);
    }

    // LAYER 3

    if (indicatorFlashNow) {

        uint8_t rollScale = abs(rcCommand[ROLL]) / 50;
        uint8_t pitchScale = abs(rcCommand[PITCH]) / 50;
        uint8_t scale = max(rollScale, pitchScale);
        nextIndicatorFlashAt = now + (LED_STRIP_5HZ / max(1, scale));

        if (indicatorFlashState == 0) {
            indicatorFlashState = 1;
        } else {
            indicatorFlashState = 0;
        }
    }

    if (indicatorFlashState == 0) {
        flashColor = &orange;
    } else {
        flashColor = &black;
    }
    if (rcCommand[ROLL] < -50) {
        setLedColor(0, flashColor);
        setLedColor(9, flashColor);
    }
    if (rcCommand[ROLL] > 50) {
        setLedColor(4, flashColor);
        setLedColor(5, flashColor);
    }
    if (rcCommand[PITCH] > 50) {
        setLedColor(0, flashColor);
        setLedColor(4, flashColor);
    }
    if (rcCommand[PITCH] < -50) {
        setLedColor(5, flashColor);
        setLedColor(9, flashColor);
    }

    ws2811UpdateStrip();
}

void determineLedStripDimensions() {
    // TODO iterate over ledConfigs and determine programatically
    ledGridWidth = 12;
    ledGridHeight = 12;
}

void ledStripInit(void) {
    determineLedStripDimensions();
}
