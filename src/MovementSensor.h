/*
 * MovementSensor.h
 *
 *  Created on: Mar 29, 2019
 *      Author: mpand
 */

#ifndef LIBRARIES_NIXIEMISC_MOVEMENTSENSOR_H_
#define LIBRARIES_NIXIEMISC_MOVEMENTSENSOR_H_

#include <Arduino.h>

class MovementSensor {
public:
	MovementSensor() : onTime(0), delayMs(0), enabled(false) {
	}

	void setDelay(byte delayMinutes) {
		delayMs = delayMinutes * 60000;
	}

	unsigned long getDelayMs() {
		return delayMs;
	}

	void setEnabled(bool enabled) {
		this->enabled = enabled;
	}

	void setOnTime(unsigned long onTime) {
		this->onTime = onTime;
	}

	bool isOn() {
		return !isOff();
	}

	bool isOff() {
		unsigned long nowMs = millis();

		return enabled && (delayMs != 0) && (nowMs - onTime >= delayMs);	// So zero = always off
	}

	void trigger() {
		onTime = millis();	// Sensor will stay high while movement is detected
	}

private:
	unsigned long onTime;
	unsigned long delayMs;
	bool enabled;
};

#endif /* LIBRARIES_NIXIEMISC_MOVEMENTSENSOR_H_ */
