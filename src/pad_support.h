/*
 * pad_support - the pure decision logic behind dinput8_xinput, kept separate to be tested.
 *
 * Stick shaping, the XInput scan throttle, and buffered-event sequence arithmetic.
 * Nothing here touches Windows, DirectInput or XInput, and none of it needs a controller
 * plugged in. That matters: deadzone geometry, frame-rate independence and event ordering
 * are exactly the kind of thing that is easy to get subtly wrong and impossible to notice
 * by feel, so the proxy and the test suite compile the same code and the test asserts on
 * numbers.
 *
 * License: MIT.
 */

#ifndef BGEFIX_PAD_SUPPORT_H
#define BGEFIX_PAD_SUPPORT_H

#include <math.h>

/* XInput axes report +32767 at full deflection (and -32768, which we clamp). */
#define PAD_AXIS_FULL 32767.0

/* sin(22.5 degrees): the half-width of an octant, used to quantise a stick direction
 * into the 8 compass directions a digital movement key can express. */
#define PAD_OCTANT 0.38268343236508984

/*
 * Radial deadzone with rescale. Writes a direction vector whose magnitude is 0 at the
 * deadzone edge and 1 at full deflection.
 *
 * Two properties a per-axis threshold does not have:
 *
 *   Circular, not cross-shaped. Testing `x > dz || y > dz` per axis leaves a dead cross:
 *   with dz = 8000 a diagonal push of (8000, 8000) fails both tests and reads as centred,
 *   even though the stick is 11313 units out - over a third of its travel. Diagonals then
 *   need a visibly harder push than cardinals. Here the test is on vector length, so the
 *   dead region is a disc and every direction behaves the same.
 *
 *   Continuous, not a step. Passing the raw axis through once it clears the threshold
 *   means output snaps from 0 to whatever the axis already read - a jump of dz/full of
 *   the range, about a quarter, the instant you cross it. Remapping [dz, full] onto
 *   [0, 1] makes the response ramp from zero instead.
 */
static void PadApplyDeadzone(int rawX, int rawY, int dz, double* outX, double* outY)
{
    *outX = 0.0;
    *outY = 0.0;

    double x = (double)rawX;
    double y = (double)rawY;
    double len = sqrt(x * x + y * y);

    if (len <= (double)dz || len <= 0.0) return;
    if ((double)dz >= PAD_AXIS_FULL) return;      /* degenerate config; nothing is live */

    /* A full diagonal can exceed PAD_AXIS_FULL because the negative axis reaches -32768.
     * Clamp the magnitude so the rescaled length cannot pass 1. */
    double mag = (len > PAD_AXIS_FULL) ? PAD_AXIS_FULL : len;
    double scaled = (mag - (double)dz) / (PAD_AXIS_FULL - (double)dz);
    if (scaled > 1.0) scaled = 1.0;

    /* direction from the raw vector, length from the rescaled magnitude */
    *outX = (x / len) * scaled;
    *outY = (y / len) * scaled;
}

/*
 * Converts a stick velocity into whole device counts for one time step, carrying the
 * fraction across calls.
 *
 * `velocity` is -1..1 from PadApplyDeadzone, `speed` is counts per second at full
 * deflection, `dt` is seconds since this consumer last took a step. Multiplying by dt is
 * what makes the result frame-rate independent: adding a fixed delta per poll instead
 * makes motion-per-second scale with the poll rate, so the same setting moves the camera
 * 2.4x faster at 144 Hz than at 60 Hz.
 *
 * The carry matters at low speeds: without it, any step whose exact value is below one
 * count truncates to zero every time and the stick appears dead rather than slow.
 */
/*
 * Whether to spend a scan of all four XInput user indices this poll.
 *
 * XInputGetState on an EMPTY slot is a documented slow path - it goes off and looks for
 * a device - and Microsoft says explicitly not to do it every frame. Walking 0..3 until
 * one answers therefore costs four slow calls per poll on a machine with no controller,
 * which is most machines running an Alt+Tab fix, and this game is pinned to one core for
 * timing stability.
 *
 * So the scan is throttled to at most once per `rescanSec`, and skipped entirely while a
 * connected pad is actually being used - during play a poll costs exactly one call on the
 * known-good index. A scan is still allowed when the current pad is idle, which is what
 * lets someone put down pad 0 and pick up pad 1 without a restart.
 *
 * `sinceScan` must be large on the very first call so that the first poll scans.
 */
static int PadShouldScan(int haveCurrent, int currentIdle, double sinceScan, double rescanSec)
{
    if (haveCurrent && !currentIdle) return 0;   /* mid-game on a live pad: never scan */
    return (sinceScan >= rescanSec) ? 1 : 0;
}

static long PadAccumulate(double velocity, int speed, double dt, double* carry)
{
    double exact = velocity * (double)speed * dt + *carry;
    long   whole = (long)exact;   /* truncates toward zero, correct for both signs */

    /* Drop the carry once the stick is centred, so releasing it cannot leave a
     * sub-count nudge waiting to be applied on some later step. */
    *carry = (velocity != 0.0) ? exact - (double)whole : 0.0;
    return whole;
}

/* ------------------------------------------------------------------ event sequencing
 *
 * DirectInput stamps every buffered event with a sequence number drawn from ONE counter
 * shared by every device in the process. That is the whole point of them: an application
 * compares numbers from the keyboard and the mouse with DISEQUENCE_COMPARE to work out
 * which happened first. A synthetic event injected into that buffer therefore has to be
 * numbered in DirectInput's namespace, not in one of our own.
 *
 * There is no API to read DirectInput's counter, so the next best thing is to anchor to
 * it: remember the highest real sequence number seen on either wrapped device and issue
 * synthetic events just above it.
 */

/* Wraparound-correct "did a happen after b", the comparison DISEQUENCE_COMPARE performs.
 * Sequence numbers wrap, so this must be a signed difference and never a < b. */
static int PadSeqAfter(unsigned long a, unsigned long b)
{
    return (long)(a - b) > 0;
}

/* The number for the next synthetic event: one past whichever is later, the highest real
 * sequence observed (`anchor`) or the last synthetic value we issued (`last`).
 *
 * Guarantees the two properties that matter - a synthetic event never sorts before a real
 * event the application has already been handed, and our own events stay ordered among
 * themselves. It cannot guarantee the far side: during a long burst of synthetic events
 * with no real input to re-anchor against, our numbers run ahead, and a real event
 * arriving later can land below one of ours. That window is bounded by the gap between
 * real events, and inside that gap there is by definition no real event to mis-order
 * against. */
static unsigned long PadSeqNext(unsigned long anchor, unsigned long last)
{
    return (PadSeqAfter(anchor, last) ? anchor : last) + 1;
}

#endif /* BGEFIX_PAD_SUPPORT_H */
