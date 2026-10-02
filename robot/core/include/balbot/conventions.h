/* SPDX-License-Identifier: GPL-2.0-only */
/* Sign and unit conventions shared by C, Python and the docs (doc 05, 5.3).
 *
 *  body frame     x forward, y left, z up
 *  pitch          rotation about +y, positive = leaning FORWARD (nose down)
 *  pitch rate     gyro y, rad/s, positive = pitching forward
 *  yaw rate       gyro z, rad/s, positive = turning LEFT (counter-clockwise from above)
 *  velocity       positive = driving FORWARD
 *  motor command  positive volts = wheels drive the robot FORWARD
 *
 * Consequences checked by tests/test_ctrl.c:
 *  - pitch > 0 (leaning forward) gives a POSITIVE command (wheels forward, back under the CG)
 *  - a positive pitch SETPOINT first gives a negative command (non-minimum-phase)
 *  - positive yaw error turns left: right wheel faster, left wheel slower
 */
#ifndef BALBOT_CONVENTIONS_H
#define BALBOT_CONVENTIONS_H
#endif
