#include <string.h>
#include "sensors/compass.h"
#include "sensors/fmath.h"

/* The vendor library's own correction, as it answers steady fields
 * near the Earth's: within 0.6 µT of them. */
static const float correction[3][3] = {
	{ 0.91703f, -0.15628f, 0.03115f },
	{ -0.10735f, 0.99400f, -0.00289f },
	{ 0.10105f, 0.07653f, 1.03206f },
};
/* The library's change of the field on any axis that means a magnet
 * came or went. */
#define JUMP_UT		150.0f
/* Samples left out after one, the average's included. */
#define QUIET		25
#define MIN_BINS	8
#define REFIT_UPDATES	8
/* Around the Earth's 22-67 µT. */
#define RADIUS_MIN	15.0f
#define RADIUS_MAX	120.0f
/* The spread's determinant over the cube of its mean variance: 1 for
 * samples all round, 0 for samples in one plane, which any sphere
 * through their circle fits. */
#define FLAT_MIN	0.2f
/* A sample further than this from the sphere than the median one is
 * not on it. */
#define OUTLIER		4.0f
#define OUTLIER_MIN	3.0f
/* The turns' sums tell the offset on their own once the watch turned
 * about more than one axis: their spread's determinant over the cube
 * of its mean, as for the samples. */
#define TURNS_FLAT	0.05f
#define OFF_FIELD	0.3f
#define OFF_FIELD_MAX	16
/* Of the turns' sums at each sample, 2 s at 50 Hz. */
#define TURNS_KEEP	0.99f
/* A sample this long after the last starts the average over: one rate
 * does not tell how far the watch turned in between. */
#define GAP_S		0.1f
#define TURN_SLACK_UT	1.0f
/* The AK09918's noise on beluga, measured still and turning; a change
 * of the average carries two samples' of it over COMPASS_AVG on each
 * axis. */
#define NOISE_UT	0.85f
#define CHANGE_NOISE	(6 * NOISE_UT * NOISE_UT / (COMPASS_AVG * COMPASS_AVG))

enum { UNRELIABLE, LOW, MEDIUM, HIGH };
/* For each accuracy: the directions covered, with an offset found
 * before, with a new one and with a new one the turns cannot vouch for;
 * the root mean square distance from the sphere as a part of its
 * radius; how much of the field's change the turns explain (µT²), and
 * the part of it left unexplained besides the chip's noise. */
static const unsigned known_bins[4] = { 0, 1, 1, 2 }, new_bins[4] = { 0, 2, 2, 2 }, alone_bins[4] = { 0, 6, 10, 16 };
static const float fit_part[4] = { 0, 0.12f, 0.07f, 0.04f };
static const float turned[4] = { 0, 5, 15, 30 }, unexplained[4] = { 0, 0.35f, 0.25f, 0.2f };
/* And the angle turned since the samples started over, in rad, for an
 * offset found before and a new one. */
static const float known_angle[4] = { 0, 1.0f, 1.5f, 2.5f }, new_angle[4] = { 0, 2.5f, 3.5f, 5.0f };

static float len3(const float v[3]) { return fm_sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void cross(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void compass_correct(const float raw[3], float out[3])
{
	for (int i = 0; i < 3; i++)
		out[i] = correction[i][0] * raw[0] + correction[i][1] * raw[1] + correction[i][2] * raw[2];
}

void compass_init(struct compass *c) { memset(c, 0, sizeof *c); }

void compass_restart(struct compass *c)
{
	c->avg_n = c->avg_at = 0;
	c->have_last = false;
}

unsigned compass_bins(const struct compass *c) { return (unsigned)__builtin_popcount(c->filled); }

static int bin_of(const float d[3])
{
	float n = len3(d);
	if (n < 1e-3f)
		return -1;
	int k = 0;
	for (int i = 0; i < 3; i++)
		k = k * 3 + (d[i] > 0.5f * n ? 2 : d[i] < -0.5f * n ? 0 : 1);
	/* 13 is the centre, which a unit vector never quantises to. */
	return k > 13 ? k - 1 : k;
}

static unsigned coverage(const struct compass *c, const float centre[3])
{
	uint32_t seen = 0;
	for (int b = 0; b < COMPASS_BINS; b++)
		if (c->filled & 1u << b) {
			float d[3] = { c->bin[b][0] - centre[0], c->bin[b][1] - centre[1], c->bin[b][2] - centre[2] };
			int k = bin_of(d);
			if (k >= 0)
				seen |= 1u << k;
		}
	return (unsigned)__builtin_popcount(seen);
}

/* The samples' root mean square distance from a sphere, as a part of
 * its radius. */
static float off_sphere(const struct compass *c, const float centre[3], float radius)
{
	float err = 0, n = 0;
	for (int b = 0; b < COMPASS_BINS; b++)
		if (c->filled & 1u << b) {
			float d[3] = { c->bin[b][0] - centre[0], c->bin[b][1] - centre[1], c->bin[b][2] - centre[2] };
			float e = len3(d) - radius;
			err += e * e;
			n++;
		}
	return n ? fm_sqrt(err / n) / radius : 1;
}

/* Solves the 4x4 system m·x = m[.][4] in place; false if singular. */
static bool solve(float m[4][5], float x[4])
{
	float scale = 0;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			if (fm_abs(m[i][j]) > scale)
				scale = fm_abs(m[i][j]);
	for (int col = 0; col < 4; col++) {
		int p = col;
		for (int r = col + 1; r < 4; r++)
			if (fm_abs(m[r][col]) > fm_abs(m[p][col]))
				p = r;
		if (fm_abs(m[p][col]) <= scale * 1e-6f)
			return false;
		if (p != col)
			for (int j = 0; j < 5; j++) {
				float t = m[col][j];
				m[col][j] = m[p][j];
				m[p][j] = t;
			}
		for (int r = col + 1; r < 4; r++) {
			float f = m[r][col] / m[col][col];
			for (int j = col; j < 5; j++)
				m[r][j] -= f * m[col][j];
		}
	}
	for (int i = 3; i >= 0; i--) {
		float s = m[i][4];
		for (int j = i + 1; j < 4; j++)
			s -= m[i][j] * x[j];
		x[i] = s / m[i][i];
	}
	return true;
}

/* |p|² = 2a·p + k for p taken from their mean, which keeps the sums
 * small enough for floats. */
static bool sphere(const struct compass *c, struct compass_cal *out)
{
	float mean[3] = { 0 }, n = 0;
	for (int b = 0; b < COMPASS_BINS; b++)
		if (c->filled & 1u << b) {
			for (int i = 0; i < 3; i++)
				mean[i] += c->bin[b][i];
			n++;
		}
	if (n < MIN_BINS)
		return false;
	for (int i = 0; i < 3; i++)
		mean[i] /= n;
	float m[4][5] = { { 0 } };
	for (int b = 0; b < COMPASS_BINS; b++) {
		if (!(c->filled & 1u << b))
			continue;
		float r[5] = { c->bin[b][0] - mean[0], c->bin[b][1] - mean[1], c->bin[b][2] - mean[2], 1, 0 };
		r[4] = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 5; j++)
				m[i][j] += r[i] * r[j];
	}
	float tr = (m[0][0] + m[1][1] + m[2][2]) / 3;
	float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		    m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	float x[4];
	if (tr <= 0 || det < FLAT_MIN * tr * tr * tr || !solve(m, x))
		return false;
	float centre[3] = { x[0] / 2, x[1] / 2, x[2] / 2 };
	float r2 = x[3] + centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2];
	if (r2 < RADIUS_MIN * RADIUS_MIN || r2 > RADIUS_MAX * RADIUS_MAX)
		return false;
	for (int i = 0; i < 3; i++)
		out->offset[i] = mean[i] + centre[i];
	out->radius = fm_sqrt(r2);
	return true;
}

/* A fit without the samples far off the first one, which a field
 * that changed on the way left behind. */
static bool fit(struct compass *c, struct compass_cal *out)
{
	if (!sphere(c, out))
		return false;
	float e[COMPASS_BINS], sorted[COMPASS_BINS];
	unsigned n = 0;
	for (int b = 0; b < COMPASS_BINS; b++)
		if (c->filled & 1u << b) {
			float d[3] = { c->bin[b][0] - out->offset[0], c->bin[b][1] - out->offset[1],
				       c->bin[b][2] - out->offset[2] };
			e[b] = fm_abs(len3(d) - out->radius);
			unsigned k = n++;
			for (; k && sorted[k - 1] > e[b]; k--)
				sorted[k] = sorted[k - 1];
			sorted[k] = e[b];
		}
	float limit = OUTLIER * sorted[n / 2];
	if (limit < OUTLIER_MIN)
		limit = OUTLIER_MIN;
	uint32_t keep = c->filled;
	for (int b = 0; b < COMPASS_BINS; b++)
		if (keep & 1u << b && e[b] > limit)
			keep &= ~(1u << b);
	if (keep == c->filled)
		return true;
	c->filled = keep;
	return sphere(c, out);
}

/* With W the turn since the last sample as a matrix (w × v) and u the
 * field's change plus W·m, an offset b leaves u - W·b unexplained of the
 * change W·(m - b) the turn makes: both are kept as sums of squares in
 * b. */
static void turns_add(struct compass_turns *t, const float m[3], const float dm[3], const float w[3])
{
	float wm[3], u[3], uw[3];
	cross(w, m, wm);
	for (int i = 0; i < 3; i++)
		u[i] = dm[i] + wm[i];
	cross(u, w, uw);
	float w2 = dot(w, w);
	float a[6] = { w2 - w[0] * w[0], w2 - w[1] * w[1], w2 - w[2] * w[2], -w[0] * w[1], -w[0] * w[2], -w[1] * w[2] };
	float am[3] = { a[0] * m[0] + a[3] * m[1] + a[4] * m[2], a[3] * m[0] + a[1] * m[1] + a[5] * m[2],
			a[4] * m[0] + a[5] * m[1] + a[2] * m[2] };
	t->uu = t->uu * TURNS_KEEP + dot(u, u);
	t->mm = t->mm * TURNS_KEEP + dot(wm, wm);
	for (int i = 0; i < 3; i++) {
		t->wu[i] = t->wu[i] * TURNS_KEEP + uw[i];
		t->wm[i] = t->wm[i] * TURNS_KEEP + am[i];
	}
	for (int i = 0; i < 6; i++)
		t->ww[i] = t->ww[i] * TURNS_KEEP + a[i];
	t->angle += fm_sqrt(w2);
	t->n = t->n * TURNS_KEEP + 1;
}

/* The offset that explains the turns best; false (b untouched) while
 * they do not tell it. */
static bool turns_offset(const struct compass *c, float b[3])
{
	const float *a = c->turns.ww;
	float m[3][3] = { { a[0], a[3], a[4] }, { a[3], a[1], a[5] }, { a[4], a[5], a[2] } };
	float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		    m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	float tr = (a[0] + a[1] + a[2]) / 3;
	if (!c->gyro_seen || tr <= 0 || det < TURNS_FLAT * tr * tr * tr)
		return false;
	for (int i = 0; i < 3; i++) {
		float t[3][3];
		memcpy(t, m, sizeof t);
		for (int r = 0; r < 3; r++)
			t[r][i] = c->turns.wu[r];
		b[i] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
			t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) /
		       det;
	}
	return true;
}

/* Without the gyroscope the turns are not known, and only the samples
 * count. */
static int turns_level(const struct compass *c, const float b[3], const float *angle)
{
	if (!c->gyro_seen)
		return HIGH;
	const struct compass_turns *t = &c->turns;
	const float *a = t->ww;
	float bab = b[0] * (a[0] * b[0] + a[3] * b[1] + a[4] * b[2]) + b[1] * (a[3] * b[0] + a[1] * b[1] + a[5] * b[2]) +
		    b[2] * (a[4] * b[0] + a[5] * b[1] + a[2] * b[2]);
	float left = t->uu - 2 * dot(b, t->wu) + bab, change = t->mm - 2 * dot(b, t->wm) + bab;
	for (int l = HIGH; l > UNRELIABLE; l--)
		if (t->angle >= angle[l] && change >= turned[l] &&
		    left <= unexplained[l] * unexplained[l] * change + CHANGE_NOISE * t->n)
			return l;
	return UNRELIABLE;
}

static int geometry_level(unsigned bins, float part, const unsigned *need)
{
	for (int l = HIGH; l > UNRELIABLE; l--)
		if (bins >= need[l] && part < fit_part[l])
			return l;
	return UNRELIABLE;
}

static int min3(int a, int b, int c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }

static int rate(const struct compass *c, struct compass_cal *f)
{
	f->level = min3(geometry_level(coverage(c, f->offset), off_sphere(c, f->offset, f->radius),
				       c->gyro_seen ? new_bins : alone_bins),
			turns_level(c, f->offset, new_angle), HIGH);
	return f->level;
}

static bool mean_radius(const struct compass *c, struct compass_cal *f)
{
	float sum = 0, n = 0;
	for (int b = 0; b < COMPASS_BINS; b++)
		if (c->filled & 1u << b) {
			float d[3] = { c->bin[b][0] - f->offset[0], c->bin[b][1] - f->offset[1], c->bin[b][2] - f->offset[2] };
			sum += len3(d);
			n++;
		}
	f->radius = n ? sum / n : 0;
	return f->radius >= RADIUS_MIN && f->radius <= RADIUS_MAX;
}

/* The offset found before counts again once new samples fit it; a new
 * one replaces it when it is trusted as far, or when the samples say
 * the old one is wrong. */
static void qualify(struct compass *c)
{
	int known = UNRELIABLE;
	bool wrong = false;
	if (c->cal.level) {
		float part = off_sphere(c, c->cal.offset, c->cal.radius);
		int turns = turns_level(c, c->cal.offset, known_angle);
		known = min3(geometry_level(coverage(c, c->cal.offset), part, known_bins), turns, c->cal.level);
		float g[3];
		wrong = (compass_bins(c) >= MIN_BINS && part >= fit_part[LOW]) ||
			(!turns && turns_offset(c, g) && turns_level(c, g, new_angle) >= MEDIUM);
	}
	struct compass_cal f, best = { .level = UNRELIABLE };
	if (fit(c, &f) && rate(c, &f) > best.level)
		best = f;
	if (turns_offset(c, f.offset) && mean_radius(c, &f) && rate(c, &f) > best.level)
		best = f;
	if (best.level && (best.level >= c->cal.level || wrong)) {
		c->cal = best;
		c->status = best.level;
		return;
	}
	if (known > c->status)
		c->status = known;
}

static void start_over(struct compass *c)
{
	c->filled = 0;
	c->have_last = c->have_range = false;
	c->off_field = 0;
	c->updates = 0;
	c->turns = (struct compass_turns){ 0 };
}

static void learn(struct compass *c, const float a[3])
{
	float seed[3], d[3];
	for (int i = 0; i < 3; i++) {
		if (!c->have_range || a[i] < c->lo[i])
			c->lo[i] = a[i];
		if (!c->have_range || a[i] > c->hi[i])
			c->hi[i] = a[i];
		seed[i] = (c->lo[i] + c->hi[i]) / 2;
	}
	if (c->status)
		memcpy(seed, c->cal.offset, sizeof seed);
	else
		turns_offset(c, seed);
	for (int i = 0; i < 3; i++)
		d[i] = a[i] - seed[i];
	c->have_range = true;
	if (len3(d) < RADIUS_MIN / 2)
		return;
	/* A sample takes the place of one in the same direction, else a
	 * free one, else one of two in a direction. */
	int b = bin_of(d), slot = -1, spare = -1, free = -1;
	uint32_t seen = 0;
	for (int k = 0; k < COMPASS_BINS; k++) {
		if (!(c->filled & 1u << k)) {
			free = free < 0 ? k : free;
			continue;
		}
		float e[3] = { c->bin[k][0] - seed[0], c->bin[k][1] - seed[1], c->bin[k][2] - seed[2] };
		int kb = len3(e) < RADIUS_MIN / 2 ? -1 : bin_of(e);
		if (kb == b)
			slot = k;
		else if (kb < 0 || seen & 1u << kb)
			spare = k;
		if (kb >= 0)
			seen |= 1u << kb;
	}
	bool fresh = !(seen & 1u << b);
	if (slot < 0)
		slot = free >= 0 ? free : spare;
	if (slot < 0)
		return;
	memcpy(c->bin[slot], a, sizeof c->bin[slot]);
	c->filled |= 1u << slot;
	if (!fresh && ++c->updates < REFIT_UPDATES)
		return;
	c->updates = 0;
	qualify(c);
}

int compass_overflow(struct compass *c)
{
	c->have_ref = false;
	c->jumps++;
	c->status = UNRELIABLE;
	c->quiet = QUIET;
	start_over(c);
	return UNRELIABLE;
}

int compass_feed(struct compass *c, const float raw[3], const float gyro[3], float dt, float out[3])
{
	float m[3];
	if (c->uncorrected)
		memcpy(m, raw, sizeof m);
	else
		compass_correct(raw, m);
	bool jump = false;
	for (int i = 0; i < 3 && c->have_ref; i++)
		jump |= fm_abs(m[i] - c->ref[i]) >= JUMP_UT;
	if (!c->have_ref)
		memcpy(c->ref, m, sizeof m);
	c->have_ref = true;
	if (jump) {
		memcpy(c->ref, m, sizeof m);
		c->jumps++;
		c->status = UNRELIABLE;
		c->quiet = QUIET;
		start_over(c);
		memcpy(out, c->out, sizeof c->out);
		return UNRELIABLE;
	}
	if (dt > GAP_S)
		compass_restart(c);
	unsigned prev = (c->avg_at + COMPASS_AVG - 1) % COMPASS_AVG;
	memcpy(c->avg[c->avg_at], m, sizeof m);
	if (gyro)
		memcpy(c->gyro[c->avg_at], gyro, sizeof c->gyro[0]);
	else
		memcpy(c->gyro[c->avg_at], c->gyro[prev], sizeof c->gyro[0]);
	c->dt[c->avg_at] = dt;
	c->avg_at = (c->avg_at + 1) % COMPASS_AVG;
	c->fed++;
	if (c->avg_n < COMPASS_AVG)
		c->avg_n++;
	/* The average moved by the turns of all its samples, each over its
	 * own time. */
	float a[3] = { 0, 0, 0 }, turn[3] = { 0, 0, 0 };
	for (unsigned k = 0; k < c->avg_n; k++)
		for (int i = 0; i < 3; i++) {
			a[i] += c->avg[k][i] / (float)c->avg_n;
			turn[i] += c->gyro[k][i] * c->dt[k] / (float)c->avg_n;
		}
	for (int i = 0; i < 3; i++)
		out[i] = c->out[i] = a[i] - c->cal.offset[i];
	if (c->quiet) {
		c->quiet--;
		return c->status;
	}
	bool full = c->avg_n == COMPASS_AVG;
	if (gyro) {
		c->gyro_seen = true;
		if (c->have_last && full) {
			/* More change than any turn makes is the field's own. */
			float dm[3] = { a[0] - c->last[0], a[1] - c->last[1], a[2] - c->last[2] };
			if (len3(dm) > len3(turn) * RADIUS_MAX + TURN_SLACK_UT) {
				c->turns = (struct compass_turns){ 0 };
				c->turn_resets++;
			} else
				turns_add(&c->turns, a, dm, turn);
		}
	}
	learn(c, a);
	memcpy(c->last, a, sizeof a);
	/* A filling average moves by less than its samples turned. */
	c->have_last = gyro != NULL && full;
	if (!c->cal.level || c->status == UNRELIABLE) {
		c->off_field = 0;
		return c->status;
	}
	float off = fm_abs(len3(out) - c->cal.radius) / c->cal.radius;
	c->off_field = off > OFF_FIELD ? c->off_field + 1 : 0;
	if (c->off_field >= OFF_FIELD_MAX) {
		c->status = UNRELIABLE;
		start_over(c);
		return UNRELIABLE;
	}
	return off > OFF_FIELD && c->status > LOW ? LOW : c->status;
}
