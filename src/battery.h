#pragma once

// Reads the battery via the onboard resistor divider (no fuel-gauge IC on
// this board) and returns a percentage clamped to [1, 100] -- never 0,
// since the my-assistant API rejects battery=0 with 400 Bad Request.
int readBatteryPercent();
