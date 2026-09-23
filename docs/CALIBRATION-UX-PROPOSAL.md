# Worn calibration proposal (design only)

The wearer wants to calibrate with the glasses on and workspace mode active. The current
standalone tool records stillness, left turn, downward nod, and right tilt for six seconds per
motion. It requires at least 12 degrees of excursion for each motion. Its nod prompt already
says a *small* nod is enough; looking straight down at the floor is unnecessary. The tilt is
used both to check handedness and to average a redundant forward axis.

## Recommended interaction

1. Keep stillness and the left turn as they are. Confirm the glasses are seated as they will be
   used, and ask the wearer to face a fixed vertical edge with their head in a comfortable,
   neutral posture.
2. For the nod, display a small target just below the current sight line. Ask for a gentle
   15-20 degree **head** nod toward it and a return to neutral. Eyes alone do not move the IMU;
   neither a 90-degree floor look nor bending the whole torso is needed. Show a live progress
   arc and only accept a clean, one-axis motion. Allow an immediate retry without restarting
   the earlier phases.
3. Compute up from gravity/left turn and right from the nod. Their cross product supplies the
   forward axis, so a clean nod can make the right tilt an *optional verification step*. If the
   nod axis is ambiguous or disagrees with gravity, offer a small right-shoulder tilt and use it
   as redundant evidence. The required tilt should be a comfortable 15-20 degrees, not an
   extreme lean. Do not silently accept a weak axis merely to finish faster.
4. Show quality in plain language: detected excursion, off-axis motion, and how closely the
   independent axes agree. When a step fails, say what to change and retry only that step.
   Preview a nod against a vertical screen edge before saving so the wearer can reject a result
   that still looks like roll.

## Implementation boundary for later work

The present calibration executable opens and controls the same HID stream used by the engine.
An in-workspace wizard therefore needs the engine's IMU samples exposed to the controller, or
an engine-owned calibration session; launching the standalone executable while the engine owns
the device is not a reliable integration. That change requires its own design, regression
tests for axis quality and optional-tilt fallback, and a worn-glasses acceptance test.

Do **not** remove the glasses and rotate them by hand for sensor-to-head calibration. That
measures the package's motion in the hand, not its axes and fit relative to the wearer's head.
