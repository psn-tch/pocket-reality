/* Replays stylus event patterns recorded on the real Jornada. */
#include <assert.h>

#include "../src/ui/ui.c"

static void stroke(int x, int y, long long at)
{
	touch_press();
	touch_sample_x(x);
	touch_sample_y(y);
	touch_release(at);
}

int main(void)
{
	int x;
	int y;

	/* One press recorded as a burst, with one reading far away. */
	stroke(691, 609, 0);
	assert(!touch_finished(40));
	stroke(690, 604, 60);
	assert(!touch_finished(100));
	stroke(692, 606, 120);
	stroke(349, 671, 170);
	stroke(691, 608, 220);
	assert(!touch_finished(300));
	assert(touch_finished(371));
	take_tap(&x, &y);
	assert(x >= 690 && x <= 692);
	assert(y >= 604 && y <= 609);

	/* The tap is reported once, and two separate presses stay separate. */
	assert(!touch_finished(600));
	stroke(100, 200, 1000);
	assert(touch_finished(1200));
	take_tap(&x, &y);
	assert(x == 100 && y == 200);
	stroke(300, 400, 1500);
	assert(touch_finished(1700));
	take_tap(&x, &y);
	assert(x == 300 && y == 400);

	/* A held stylus and an empty burst never make a tap. */
	touch_press();
	assert(!touch_finished(9000));
	touch_release(9000);
	assert(!touch_finished(9200));
	puts("tap tests passed");
	return 0;
}
