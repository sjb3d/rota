#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <time.h>

#ifdef _MSC_VER
#pragma warning(disable: 4702) // unreachable code
#endif

#define INVALID_TIME			((time_t)-1)
#define TIME_DELTA_DAY			((time_t)(24*60*60))
#define EPOCH_YEAR				1900

typedef struct tm tm_t;

#define DIV_ROUND_UP(N, D)	(((N) + ((D) - 1)) / (D))
#define MIN(A, B)			(((A) < (B)) ? (A) : (B))

/*
	Goals that fail the schedule:

	* Shifts cannot overlap!
	* Do not work holidays at all.
	* Do not be on call for invalid on call days.
	* Do not be on the ward for invalid ward weeks.
	* Do not work the day after an on call.
	* Do not do a weekday on call the day before holiday.

	Goals that do not fail the schedule:

	* Do not work disliked ward weeks.
	* Do not work disliked on call days.
	* Number of weekends on call should be even.
	* Number of days on call should be even.
	* Number of ward weeks should be even.
	* Person on ward week preferred for that weekend on call.
	* Prefer even sized blocks of days off.
	* Prefer even sized blocks between ward weeks.

	Unimplemented:

	* Bank holidays, make number of bank holiday on calls even?
*/


#define MAX_PERSON_COUNT	32
#define MAX_WEEK_COUNT		32

#define MAX_PERSON_NAME_LENGTH		64

typedef unsigned int uint;

typedef struct
{
	char name[MAX_PERSON_NAME_LENGTH];
	int first_day;
	int last_day;
	int total_non_holiday_days;

	float full_time_amount;
	float effective_full_time_amount;
	bool cannot_do_ward_weeks;

	float on_call_day_bias;
	float on_call_weekend_bias;
	float ward_week_bias;
	float bank_holiday_bias;

	float target_ward_weeks;
	float target_on_call_days;
	float target_on_call_weekends;
	float target_on_call_bank_holidays;

	float target_day_off_block_size;
	float target_ward_week_spacing;
} person_config_t;

typedef struct
{
	int person_count;
	int week_count;
	time_t first_day;

	person_config_t people[MAX_PERSON_COUNT];

	uint bank_holiday_bits[DIV_ROUND_UP(MAX_WEEK_COUNT*7, 32)];
	uint holiday_day_bits[MAX_WEEK_COUNT*7 + 1];
	uint invalid_on_call_day_bits[MAX_WEEK_COUNT*7];
	uint invalid_ward_week_bits[MAX_WEEK_COUNT];
	uint disliked_on_call_day_bits[MAX_WEEK_COUNT*7];
	uint disliked_ward_week_bits[MAX_WEEK_COUNT];
	int forced_on_call_people[MAX_WEEK_COUNT*7];

	float total_on_call_days_and_bias;
	float total_on_call_weekends_and_bias;
	float total_ward_weeks_and_bias;
	float total_bank_holidays_and_bias;
	float effective_on_call_person_count;
	float effective_ward_person_count;
} config_t;

enum
{
	POINTS_SHIFT_OVERLAP,
	POINTS_WORK_ON_HOLIDAY,
	POINTS_ON_CALL_ON_INVALID_DAY,
	POINTS_ON_WARD_ON_INVALID_WEEK,
	POINTS_NOT_ON_CALL_WHEN_FORCED,
	POINTS_WORK_FOLLOWING_ON_CALL,
	POINTS_ON_CALL_ON_DISLIKED_DAY,
	POINTS_WARD_WEEK_ON_DISLIKED_WEEK,
	POINTS_ON_CALL_DAY_DIFFERENCE,
	POINTS_ON_CALL_BANK_HOLIDAY_DIFFERENCE,
	POINTS_ON_CALL_WEEKEND_DIFFERENCE,
	POINTS_WARD_WEEK_DIFFERENCE,
	POINTS_ON_CALL_WEEKEND_FOLLOWS_WARD_WEEK,
	POINTS_MULTIPLE_ON_CALLS_PER_WEEK,
	POINTS_WARD_WEEK_ONE_WEEK_AGO,
	POINTS_WARD_WEEK_TWO_WEEKS_AGO,
	POINTS_DAY_OFF,
	POINTS_DAY_OFF_DECAY,
	POINTS_NO_WARD_WEEK,
	POINTS_NO_WARD_WEEK_DECAY,
	POINTS_COUNT
};

static const char *const g_points_names[POINTS_COUNT] =
{
	"shift_overlap",
	"work_on_holiday",
	"on_call_on_invalid_day",
	"on_ward_on_invalid_week",
	"not_on_call_when_forced",
	"work_following_on_call",
	"on_call_on_disliked_day",
	"ward_week_on_disliked_week",
	"on_call_day_difference",
	"on_call_bank_holiday_difference",
	"on_call_weekend_difference",
	"ward_week_difference",
	"on_call_weekend_follows_ward_week",
	"multiple_on_calls_per_week",
	"ward_week_one_week_ago",
	"ward_week_two_weeks_ago",
	"day_off",
	"day_off_decay",
	"no_ward_week",
	"no_ward_week_decay"
};

typedef struct
{
	float values[POINTS_COUNT];
} points_t;

enum
{
	SHIFT_ON_CALL_MON,
	SHIFT_ON_CALL_TUE,
	SHIFT_ON_CALL_WED,
	SHIFT_ON_CALL_THU,
	SHIFT_ON_CALL_FRI,
	SHIFT_ON_CALL_WEEKEND,
	SHIFT_WARD_WEEK,
	SHIFT_COUNT
};

typedef struct
{
	int shifts[SHIFT_COUNT];
} week_t;

typedef struct
{
	week_t weeks[MAX_WEEK_COUNT];
} rota_t;

enum
{
	FAILURE_MULTIPLE_SHIFTS_AT_ONCE,
	FAILURE_WORK_ON_HOLIDAY,
	FAILURE_WORK_JUST_BEFORE_HOLIDAY,
	FAILURE_NOT_ON_CALL_WHEN_FORCED,
	FAILURE_ON_WARD_WHEN_CANNOT,
	FAILURE_ON_CALL_WHEN_CANNOT,
	FAILURE_WORK_FOLLOWING_ON_CALL,
	FAILURE_COUNT
};

static char const *const g_failure_names[FAILURE_COUNT] =
{
	"multiple shifts at once",
	"working on holiday",
	"working just before holiday",
	"not on call when forced",
	"on ward when cannot",
	"on call when cannot",
	"work following on call"
};

typedef struct
{
	int total_ward_weeks;
	int total_on_call_days;
	int total_on_call_weekends;
	int total_on_call_bank_holidays;
	float remainder_ward_weeks;
	float remainder_on_call_days;
	float remainder_on_call_weekends;
	float remainder_on_call_bank_holidays;
} person_score_t;

typedef struct
{
	int failure;
	int person_index;
	int rota_day_index;
} failure_data_t;

#define MAX_FAILURE_COUNT		16

typedef struct
{
	person_score_t people[MAX_PERSON_COUNT];
	float value;
	int failure_count;
	failure_data_t failure_data[MAX_FAILURE_COUNT];
} score_t;

void add_failure(score_t *score, int failure, int person_index, int rota_day_index)
{
	if (score->failure_count < MAX_FAILURE_COUNT) {
		int const failure_index = score->failure_count++;
		failure_data_t *const data = &score->failure_data[failure_index];
		data->failure = failure;
		data->person_index = person_index;
		data->rota_day_index = rota_day_index;
	}
}

extern void init_genrand(unsigned long s);
extern unsigned long genrand_int32(void);

int rota_rand(int person_count)
{
	return genrand_int32() % person_count;
}

void set_bank_holiday(config_t *config, int rota_day_index)
{
	uint const entry_index = rota_day_index/32;
	uint const bit_index = rota_day_index % 32;
	config->bank_holiday_bits[entry_index] |= (1U << bit_index);
}

bool is_bank_holiday(config_t const *config, int rota_day_index)
{
	uint const entry_index = rota_day_index/32;
	uint const bit_index = rota_day_index % 32;
	return (config->bank_holiday_bits[entry_index] & (1U << bit_index)) != 0;
}

void set_holiday_day(config_t *config, int rota_day_index, int person)
{
	config->holiday_day_bits[rota_day_index] |= (1U << person);
}

bool is_holiday_day(config_t const *config, int rota_day_index, int person)
{
	return (config->holiday_day_bits[rota_day_index] & (1U << person)) != 0;
}

void set_invalid_on_call_day(config_t *config, int rota_day_index, int person)
{
	config->invalid_on_call_day_bits[rota_day_index] |= (1U << person);
}

bool is_invalid_on_call_day(config_t const *config, int rota_day_index, int person)
{
	return (config->invalid_on_call_day_bits[rota_day_index] & (1U << person)) != 0;
}

void set_invalid_ward_week(config_t *config, int week_index, int person)
{
	config->invalid_ward_week_bits[week_index] |= (1U << person);
}

bool is_invalid_ward_week(config_t const *config, int week_index, int person)
{
	return (config->invalid_ward_week_bits[week_index] & (1U << person)) != 0;
}

void set_disliked_ward_week(config_t *config, int week_index, int person)
{
	config->disliked_ward_week_bits[week_index] |= (1U << person);
}

bool is_disliked_ward_week(config_t const *config, int week_index, int person)
{
	return (config->disliked_ward_week_bits[week_index] & (1U << person)) != 0;
}

void set_disliked_on_call_day(config_t *config, int rota_day_index, int person)
{
	config->disliked_on_call_day_bits[rota_day_index] |= (1U << person);
}

bool is_disliked_on_call_day(config_t const *config, int rota_day_index, int person)
{
	return (config->disliked_on_call_day_bits[rota_day_index] & (1U << person)) != 0;
}

float sqr(float x)
{
	return x*x;
}

float get_days_off_score(points_t const *points, int day_difference)
{
	float sum = 0.f;
	float score = points->values[POINTS_DAY_OFF];
	float const decay = points->values[POINTS_DAY_OFF_DECAY];
	for (int i = 1; i < day_difference; ++i) {
		sum += score;
		score *= decay;
	}
	return sum;
}

float get_no_ward_week_score(points_t const *points, int week_difference)
{
	float sum = 0.f;
	float score = points->values[POINTS_NO_WARD_WEEK];
	float const decay = points->values[POINTS_NO_WARD_WEEK_DECAY];
	for (int i = 1; i < week_difference; ++i) {
		sum += score;
		score *= decay;
	}
	return sum;
}

void score_rota(
	config_t const *config,
	points_t const *points,
	rota_t const *rota,
	score_t *score)
{
	memset(score, 0, sizeof(score_t));

	// sweep as much as possible in one pass
	int last_on_call_week[MAX_PERSON_COUNT];
	int last_work_day[MAX_PERSON_COUNT];
	int last_ward_week[MAX_PERSON_COUNT];
	for (int i = 0; i < config->person_count; ++i) {
		last_on_call_week[i] = -1;
		last_work_day[i] = config->people[i].first_day - 1;
		last_ward_week[i] = config->people[i].first_day/7 - 1;
	}
	int person_on_call_yesterday = -1;
	for (int week_index = 0; week_index < config->week_count; ++week_index) {
		week_t const *const week = &rota->weeks[week_index];

		// loop over the days
		for (int day_index = 0; day_index < 7; ++day_index) {
			int rota_day_index = week_index*7 + day_index;
			if (day_index < 5) {
				int const person_on_call = week->shifts[day_index];
				int const person_on_ward = week->shifts[SHIFT_WARD_WEEK];

				// check for shift overlap
				if (person_on_call == person_on_ward) {
					score->value += points->values[POINTS_SHIFT_OVERLAP];
					add_failure(score, FAILURE_MULTIPLE_SHIFTS_AT_ONCE, person_on_call, rota_day_index);
				}
				if (day_index == 0 && config->people[person_on_ward].cannot_do_ward_weeks) {
					score->value += points->values[POINTS_ON_WARD_ON_INVALID_WEEK];
					add_failure(score, FAILURE_ON_WARD_WHEN_CANNOT, person_on_ward, rota_day_index);
				}

				// check holidays
				if (is_holiday_day(config, rota_day_index, person_on_call)) {
					score->value += points->values[POINTS_WORK_ON_HOLIDAY];
					add_failure(score, FAILURE_WORK_ON_HOLIDAY, person_on_call, rota_day_index);
				}
				if (is_holiday_day(config, rota_day_index, person_on_ward)) {
					score->value += points->values[POINTS_WORK_ON_HOLIDAY];
					add_failure(score, FAILURE_WORK_ON_HOLIDAY, person_on_ward, rota_day_index);
				}
				if (is_holiday_day(config, rota_day_index + 1, person_on_call)) {
					score->value += points->values[POINTS_WORK_ON_HOLIDAY];
					add_failure(score, FAILURE_WORK_JUST_BEFORE_HOLIDAY, person_on_call, rota_day_index);
				}

				// check invalid days
				if (day_index == 0 && is_invalid_ward_week(config, week_index, person_on_ward)) {
					score->value += points->values[POINTS_ON_WARD_ON_INVALID_WEEK];
					add_failure(score, FAILURE_ON_WARD_WHEN_CANNOT, person_on_ward, rota_day_index);
				}
				if (is_invalid_on_call_day(config, rota_day_index, person_on_call)) {
					score->value += points->values[POINTS_ON_CALL_ON_INVALID_DAY];
					add_failure(score, FAILURE_ON_CALL_WHEN_CANNOT, person_on_call, rota_day_index);
				}

				// check forced on call days
				int const forced_on_call_person = config->forced_on_call_people[rota_day_index];
				if (forced_on_call_person != -1 && forced_on_call_person != person_on_call) {
					score->value += points->values[POINTS_NOT_ON_CALL_WHEN_FORCED];
					add_failure(score, FAILURE_NOT_ON_CALL_WHEN_FORCED, forced_on_call_person, rota_day_index);
				}

				// check for not being on call yesterday
				if (day_index == 0 && person_on_ward == person_on_call_yesterday) {
					score->value += points->values[POINTS_WORK_FOLLOWING_ON_CALL];
					add_failure(score, FAILURE_WORK_FOLLOWING_ON_CALL, person_on_ward, rota_day_index);
				}
				if (person_on_call == person_on_call_yesterday) {
					score->value += points->values[POINTS_WORK_FOLLOWING_ON_CALL];
					add_failure(score, FAILURE_WORK_FOLLOWING_ON_CALL, person_on_call, rota_day_index);
				}

				// check for not being on call this week
				if (last_on_call_week[person_on_call] == week_index) {
					score->value += points->values[POINTS_MULTIPLE_ON_CALLS_PER_WEEK];
				}

				// check for not being on ward last week
				if (day_index == 0 && week_index > 0 && rota->weeks[week_index - 1].shifts[SHIFT_WARD_WEEK] == person_on_ward) {
					score->value += points->values[POINTS_WARD_WEEK_ONE_WEEK_AGO];
				}
				if (day_index == 0 && week_index > 1 && rota->weeks[week_index - 2].shifts[SHIFT_WARD_WEEK] == person_on_ward) {
					score->value += points->values[POINTS_WARD_WEEK_TWO_WEEKS_AGO];
				}

				// check disliked days
				if (is_disliked_on_call_day(config, rota_day_index, person_on_call)) {
					score->value += points->values[POINTS_ON_CALL_ON_DISLIKED_DAY];
				}
				if (day_index == 0 && is_disliked_ward_week(config, week_index, person_on_ward)) {
					score->value += points->values[POINTS_WARD_WEEK_ON_DISLIKED_WEEK];
				}

				// check days off
				score->value += get_days_off_score(points, rota_day_index - last_work_day[person_on_call]);
				if (day_index == 0) {
					score->value += get_days_off_score(points, rota_day_index - last_work_day[person_on_ward]);
				}

				// check last ward week
				if (day_index == 0) {
					score->value += get_no_ward_week_score(points, week_index - last_ward_week[person_on_ward]);
				}

				// update tracking
				last_on_call_week[person_on_call] = week_index;
				last_ward_week[person_on_ward] = week_index;
				last_work_day[person_on_call] = rota_day_index;
				last_work_day[person_on_ward] = rota_day_index;
				person_on_call_yesterday = person_on_call;

				// update counters
				if (is_bank_holiday(config, rota_day_index)) {
					++score->people[person_on_call].total_on_call_bank_holidays;
				}
				++score->people[person_on_call].total_on_call_days;
				if (day_index == 0) {
					++score->people[person_on_ward].total_ward_weeks;
				}
			} else {
				int const person_on_call = week->shifts[SHIFT_ON_CALL_WEEKEND];

				// check holidays
				if (is_holiday_day(config, rota_day_index, person_on_call)) {
					score->value += points->values[POINTS_WORK_ON_HOLIDAY];
					add_failure(score, FAILURE_WORK_ON_HOLIDAY, person_on_call, rota_day_index);
				}

				// check invalid on call days
				if (is_invalid_on_call_day(config, rota_day_index, person_on_call)) {
					score->value += points->values[POINTS_ON_CALL_ON_INVALID_DAY];
					add_failure(score, FAILURE_ON_CALL_WHEN_CANNOT, person_on_call, rota_day_index);
				}

				// check forced on call days
				int const forced_on_call_person = config->forced_on_call_people[rota_day_index];
				if (forced_on_call_person != -1 && forced_on_call_person != person_on_call) {
					score->value += points->values[POINTS_NOT_ON_CALL_WHEN_FORCED];
					add_failure(score, FAILURE_NOT_ON_CALL_WHEN_FORCED, forced_on_call_person, rota_day_index);
				}

				// check for not being on call yesterday
				if (day_index == 5 && person_on_call == person_on_call_yesterday) {
					score->value += points->values[POINTS_WORK_FOLLOWING_ON_CALL];
					add_failure(score, FAILURE_WORK_FOLLOWING_ON_CALL, person_on_call, rota_day_index);
				}

				// check for not being on call this week
				if (day_index == 5 && last_on_call_week[person_on_call] == week_index) {
					score->value += points->values[POINTS_MULTIPLE_ON_CALLS_PER_WEEK];
				}

				// check disliked days
				if (is_disliked_on_call_day(config, rota_day_index, person_on_call)) {
					score->value += points->values[POINTS_ON_CALL_ON_DISLIKED_DAY];
				}

				// check days off
				if (day_index == 5) {
					score->value += get_days_off_score(points, rota_day_index - last_work_day[person_on_call]);
				}

				// update tracking
				last_on_call_week[person_on_call] = week_index;
				last_work_day[person_on_call] = rota_day_index;
				person_on_call_yesterday = person_on_call;

				// update counters
				if (day_index == 5) {
					++score->people[person_on_call].total_on_call_weekends;
				}
			}
		}

		// check on call weekends follow from ward weeks
		if (week->shifts[SHIFT_ON_CALL_WEEKEND] == week->shifts[SHIFT_WARD_WEEK]) {
			score->value += points->values[POINTS_ON_CALL_WEEKEND_FOLLOWS_WARD_WEEK];
		}
	}

	// check days off after last work day
	for (int i = 0; i < config->person_count; ++i) {
		int const last_day = config->people[i].last_day;
		score->value += get_days_off_score(points, last_day - last_work_day[i]);
		int const last_week = last_day/7;
		score->value += get_no_ward_week_score(points, last_week - last_ward_week[i]);
	}

	// check for even distribution of shifts
	for (int i = 0; i < config->person_count; ++i) {
		person_config_t const *const person_config = &config->people[i];
		person_score_t *const person_score = &score->people[i];

		float const remainder_on_call_days = person_score->total_on_call_days + person_config->on_call_day_bias - person_config->target_on_call_days;
		float const remainder_on_call_weekends = person_score->total_on_call_weekends + person_config->on_call_weekend_bias - person_config->target_on_call_weekends;
		float const remainder_ward_weeks = person_score->total_ward_weeks + person_config->ward_week_bias - person_config->target_ward_weeks;
		float const remainder_on_call_bank_holidays = person_score->total_on_call_bank_holidays + person_config->bank_holiday_bias - person_config->target_on_call_bank_holidays;

		person_score->remainder_on_call_days = remainder_on_call_days;
		person_score->remainder_on_call_weekends = remainder_on_call_weekends;
		person_score->remainder_ward_weeks = remainder_ward_weeks;
		person_score->remainder_on_call_bank_holidays = remainder_on_call_bank_holidays;

		score->value += points->values[POINTS_ON_CALL_DAY_DIFFERENCE]*sqr(remainder_on_call_days);
		score->value += points->values[POINTS_ON_CALL_WEEKEND_DIFFERENCE]*sqr(remainder_on_call_weekends);
		score->value += points->values[POINTS_WARD_WEEK_DIFFERENCE]*sqr(remainder_ward_weeks);
		score->value += points->values[POINTS_ON_CALL_BANK_HOLIDAY_DIFFERENCE]*sqr(remainder_on_call_bank_holidays);
	}
}

void print_rota_html(
	char const *filename,
	config_t const *config,
	rota_t const *rota,
	score_t const *score)
{
	FILE *const fp = fopen(filename, "w");
	if (!fp) {
		fprintf(stderr, "failed to open \"%s\" for writing!\n", filename);
		exit(-1);
	}

	fprintf(fp, "<!DOCTYPE html>\n\
<html>\n\
<head>\n\
<style>\n\
table, th, td {\n\
border: 1px solid black;\n\
border-collapse: collapse;\n\
}\n\
th, td {\n\
padding: 5px;\n\
}\n\
table tr td {\n\
width: 10px;\n\
}\n\
</style>\n\
</head>\n\
<body>\n\
<h1>Rota Output</h1>\n\
<table>\n\
<tr><th colspan=\"2\">Key</th></tr>\n\
<tr><td style=\"white-space: nowrap\">holiday</td><td style=\"background-color:grey\"></td></tr>\n\
<tr><td style=\"white-space: nowrap\">bank holiday</td><td style=\"background-color:lightblue\"></td></tr>\n\
<tr><td style=\"white-space: nowrap\">weekend</td><td style=\"background-color:cyan\"></td></tr>\n\
<tr><td style=\"white-space: nowrap\">ward week</td><td style=\"background-color:yellow\"></td></tr>\n\
<tr><td style=\"white-space: nowrap\">on call</td><td style=\"background-color:red\"></td></tr>\n\
</table>\n\
<br>\n\
");

	fprintf(fp, "<table>\n<tr>\n<th>Name</th>\n");
	for (int week_index = 0; week_index < config->week_count; ++week_index) {
		time_t const first_day = config->first_day + week_index*7*TIME_DELTA_DAY;
		tm_t *const tm = localtime(&first_day);
		fprintf(fp, "<th colspan=\"7\">%d/%d/%d</th>\n", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + EPOCH_YEAR);
	}
	fprintf(fp, "</tr>");
	for (int person_index = 0; person_index < config->person_count; ++person_index) {
		fprintf(fp, "<tr><td style=\"white-space: nowrap\">%s</td>\n", config->people[person_index].name);
		for (int week_index = 0; week_index < config->week_count; ++week_index) {
			week_t const *const week = &rota->weeks[week_index];
			bool const is_ward_week = (week->shifts[SHIFT_WARD_WEEK] == person_index);
			bool const mark_ward_week = is_disliked_ward_week(config, week_index, person_index);
			for (int weekday_index = 0; weekday_index < 7; ++weekday_index) {
				int const rota_day_index = 7*week_index + weekday_index;
				int const person_on_call = (weekday_index < 5) ? week->shifts[weekday_index] : week->shifts[SHIFT_ON_CALL_WEEKEND];
				bool const mark_on_call = is_disliked_on_call_day(config, rota_day_index, person_index);
				char const *extra = "";
				char const *contents = "";
				if (person_index == person_on_call) {
					extra = " style=\"background-color:red\"";
					if (mark_on_call) {
						contents = "x";
					}
				} else if (weekday_index < 5 && is_ward_week) {
					extra = " style=\"background-color:yellow\"";
					if (mark_ward_week) {
						contents = "x";
					}
				} else if (is_holiday_day(config, rota_day_index, person_index)) {
					extra = " style=\"background-color:grey\"";
				} else if (is_bank_holiday(config, rota_day_index)) {
					extra = " style=\"background-color:lightblue\"";
				} else if (weekday_index >= 5) {
					extra = " style=\"background-color:cyan\"";
				}
				fprintf(fp, "<td%s>%s</td>\n", extra, contents);
			}
		}
		fprintf(fp, "</tr>");
	}
	fprintf(fp, "</table>\n");

	fprintf(fp, "<h1>Summary</h1>\n<table>\n");
	fprintf(fp, "<tr>\n\
<th rowspan=\"2\">Name</th>\n\
<th colspan=\"2\">Full Time</th>\n\
<th colspan=\"3\">Input Bias</th>\n\
<th colspan=\"3\">Rota Target</th>\n\
<th colspan=\"3\">Rota Result</th>\n\
<th colspan=\"3\">Output Bias</th>\n\
</tr>\n\
");
	fprintf(fp, "<tr>\n\
<th>Input</th><th>Effective</th>\n\
<th>On Call Days (Bank Hols)</th><th>On Call Weekends</th><th>Ward Weeks</th>\n\
<th>On Call Days (Bank Hols)</th><th>On Call Weekends</th><th>Ward Weeks</th>\n\
<th>On Call Days (Bank Hols)</th><th>On Call Weekends</th><th>Ward Weeks</th>\n\
<th>On Call Days (Bank Hols)</th><th>On Call Weekends</th><th>Ward Weeks</th>\n\
</tr>\n\
");
	for (int person_index = 0; person_index < config->person_count; ++person_index) {
		person_config_t const *person_config = &config->people[person_index];
		person_score_t const *person_score = &score->people[person_index];
		fprintf(fp, "<tr><td>%s</td>\n", person_config->name);
		fprintf(fp, "<td>%.3f</td><td>%.3f</td>", person_config->full_time_amount, person_config->effective_full_time_amount);
		fprintf(fp, "<td>%f (%f)</td><td>%f</td><td>%f</td>",
			person_config->on_call_day_bias,
			person_config->bank_holiday_bias,
			person_config->on_call_weekend_bias,
			person_config->ward_week_bias);
		fprintf(fp, "<td>%.1f (%.1f)</td><td>%.1f</td><td>%.1f</td>",
			person_config->target_on_call_days,
			person_config->target_on_call_bank_holidays,
			person_config->target_on_call_weekends,
			person_config->target_ward_weeks);
		fprintf(fp, "<td>%d (%d)</td><td>%d</td><td>%d</td>",
			person_score->total_on_call_days,
			person_score->total_on_call_bank_holidays,
			person_score->total_on_call_weekends,
			person_score->total_ward_weeks);
		fprintf(fp, "<td>%.1f (%.1f)</td><td>%.1f</td><td>%.1f</td>",
			person_score->remainder_on_call_days,
			person_score->remainder_on_call_bank_holidays,
			person_score->remainder_on_call_weekends,
			person_score->remainder_ward_weeks);
		fprintf(fp, "</tr>\n");
	}
	fprintf(fp, "</table>\n");

	fprintf(fp, "</body>\n</html>\n");
	fclose(fp);

	printf("written output to \"%s\"\n", filename);
}

void print_rota_csv(char const *filename, config_t const *config, rota_t const *rota)
{
	FILE *const fp = fopen(filename, "w");
	if (!fp) {
		fprintf(stderr, "failed to open \"%s\" for writing!\n", filename);
		exit(-1);
	}

	for (int week_index = 0; week_index < config->week_count; ++week_index) {
		fprintf(fp, "Date");
		for (int weekday_index = 0; weekday_index < 7; ++weekday_index) {
			int const rota_day_index = 7*week_index + weekday_index;
			time_t const day_time = config->first_day + rota_day_index*TIME_DELTA_DAY;
			tm_t *const tm = localtime(&day_time);
			fprintf(fp, ",%d/%d/%d", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + EPOCH_YEAR);
		}
		fprintf(fp, "\n");

		week_t const *const week = &rota->weeks[week_index];

		fprintf(fp, "On Call");
		for (int weekday_index = 0; weekday_index < 7; ++weekday_index) {
			int const person_on_call = week->shifts[(weekday_index < 5) ? weekday_index : SHIFT_ON_CALL_WEEKEND];
			fprintf(fp, ",%s", config->people[person_on_call].name);
		}
		fprintf(fp, "\n");

		fprintf(fp, "Ward");
		int const person_on_ward = week->shifts[SHIFT_WARD_WEEK];
		for (int weekday_index = 0; weekday_index < 5; ++weekday_index) {
			fprintf(fp, ",%s", config->people[person_on_ward].name);
		}
		fprintf(fp, ",,\n");

		fprintf(fp, ",,,,,,,\n");
	}

	fclose(fp);

	printf("written output to \"%s\"\n", filename);
}

void mutate_random_reassign(
	config_t const *config,
	rota_t const *rota_in,
	rota_t *rota_out)
{
	memcpy(rota_out, rota_in, sizeof(rota_t));

	int const week = rota_rand(config->week_count);
	int const shift = rota_rand(SHIFT_COUNT);

	rota_out->weeks[week].shifts[shift] = rota_rand(config->person_count);
}

void mutate_random_swap(
	config_t const *config,
	rota_t const *rota_in,
	rota_t *rota_out)
{
	memcpy(rota_out, rota_in, sizeof(rota_t));

	int const week_a = rota_rand(config->week_count);
	int const shift_a = rota_rand(SHIFT_COUNT);

	int const week_b = rota_rand(config->week_count);
	int shift_b = shift_a;
	if (shift_a < 5) {
		shift_b = rota_rand(5);
	}

	rota_out->weeks[week_a].shifts[shift_a] = rota_in->weeks[week_b].shifts[shift_b];
	rota_out->weeks[week_b].shifts[shift_b] = rota_in->weeks[week_a].shifts[shift_a];
}

#define MAX_LINE_LENGTH			(16*1024)

char *scan_for_next_column(char *p)
{
	// skip to delimiter
	if (p) {
		for (;;) {
			char const c = *p;
			if (c ==  ',') {
				*p = '\0';
				++p;
				break;
			}
			if (c == '\n' || c == '\r' || c == '\0') {
				*p = '\0';
				p = NULL;
				break;
			}
			++p;
		}
	}
	return p;
}

time_t parse_date(char const *str)
{
	int day,month,year;
	if (sscanf(str, "%d/%d/%d", &day, &month, &year) != 3) {
		fprintf(stderr, "failed to parse date \"%s\"!\n", str);
		exit(-1);
	}

	tm_t tm;
	memset(&tm, 0, sizeof(tm_t));
	tm.tm_year = year - EPOCH_YEAR;
	tm.tm_mday = day;
	tm.tm_mon = month - 1;
	tm.tm_hour = 12;
	tm.tm_isdst = 1;

	time_t const t = mktime(&tm);
	if (t == INVALID_TIME) {
		fprintf(stderr, "failed to convert date \"%s\"!\n", str);
		exit(-1);
	}
	return t;
}

int find_or_add_person(config_t *config, char const *name)
{
	if (*name == '\0') {
		fprintf(stderr, "name must not be empty!\n");
		exit(-1);
	}

	int person = 0;
	for (;;) {
		if (person == config->person_count) {
			person_config_t *const info = &config->people[person];
			strcpy(info->name, name);
			info->full_time_amount = 1.f;
			info->first_day = 0;
			info->last_day = config->week_count*7 - 1;
			info->effective_full_time_amount = 1.f;
			++config->person_count;
			break;
		}
		if (strcmp(config->people[person].name, name) == 0) {
			break;
		}
		++person;
	}
	return person;
}

enum
{
	CATEGORY_HOLIDAY,
	CATEGORY_CANNOT_ON_CALL_DAY,
	CATEGORY_CANNOT_ON_CALL_DAY_ALWAYS,
	CATEGORY_CANNOT_WARD_WEEK,
	CATEGORY_DISLIKE_ON_CALL_DAY,
	CATEGORY_DISLIKE_ON_CALL_DAY_ALWAYS,
	CATEGORY_DISLIKE_WARD_WEEK,
	CATEGORY_PART_TIME,
	CATEGORY_START_DATE,
	CATEGORY_END_DATE,
	CATEGORY_BANK_HOLIDAY,
	CATEGORY_BANK_HOLIDAY_BIAS,
	CATEGORY_WARD_WEEK_BIAS,
	CATEGORY_ON_CALL_DAY_BIAS,
	CATEGORY_ON_CALL_WEEKEND_BIAS,
	CATEGORY_NO_WARD_WEEKS,
	CATEGORY_FORCE_ON_CALL_DAY,
	CATEGORY_COUNT
};

#define NAME_HOLIDAY				"holiday"
#define NAME_CANNOT_ON_CALL_DAY		"cannot be on call"
#define NAME_CANNOT_WARD_WEEK		"cannot be on ward"
#define NAME_DISLIKE_ON_CALL_DAY	"prefer not on call"
#define NAME_DISLIKE_WARD_WEEK		"prefer not on ward"
#define NAME_PART_TIME				"part time"
#define NAME_START_DATE				"start date"
#define NAME_END_DATE				"end date"
#define NAME_BANK_HOLIDAY			"bank holiday"
#define NAME_BANK_HOLIDAY_BIAS		"bank holiday bias"
#define NAME_WARD_WEEK_BIAS			"ward week bias"
#define NAME_ON_CALL_DAY_BIAS		"on call day bias"
#define NAME_ON_CALL_WEEKEND_BIAS	"on call weekend bias"
#define NAME_NO_WARD_WEEKS			"no ward weeks"
#define NAME_FORCE_ON_CALL_DAY		"must be on call"

static char const *const g_category_names[CATEGORY_COUNT] =
{
	NAME_HOLIDAY,
	NAME_CANNOT_ON_CALL_DAY,
	"always " NAME_CANNOT_ON_CALL_DAY,
	NAME_CANNOT_WARD_WEEK,
	NAME_DISLIKE_ON_CALL_DAY,
	"always " NAME_DISLIKE_ON_CALL_DAY,
	NAME_DISLIKE_WARD_WEEK,
	NAME_PART_TIME,
	NAME_START_DATE,
	NAME_END_DATE,
	NAME_BANK_HOLIDAY,
	NAME_BANK_HOLIDAY_BIAS,
	NAME_WARD_WEEK_BIAS,
	NAME_ON_CALL_DAY_BIAS,
	NAME_ON_CALL_WEEKEND_BIAS,
	NAME_NO_WARD_WEEKS,
	NAME_FORCE_ON_CALL_DAY
};

int match_category_by_tag(char const *tag)
{
	for (int i = 0; i < CATEGORY_COUNT; ++i) {
		if (strcmp(tag, g_category_names[i]) == 0) {
			return i;
		}
	}
	fprintf(stderr, "unknown category \"%s\"!\n", tag);
	exit(-1);
	return -1;
}

#define FIRST_DAY_COLUMN		2

char *rota_get_line(char *line_buf, int max_length, FILE *fp)
{
	int const last = max_length - 1;
	int i = 0;
	do {
		int const c = fgetc(fp);
		if (c == EOF) {
			if (i == 0) {
				return NULL;
			}
			break;
		}
		if (c == '\r' || c == '\n') {
			break;
		}
		line_buf[i++] = (char)c;
	} while(i < last);
	line_buf[i] = '\0';
	return line_buf;
}

void read_config(char const *filename, config_t *config)
{
	memset(config, 0, sizeof(config_t));
	for (int i = 0; i < 7*MAX_WEEK_COUNT; ++i) {
		config->forced_on_call_people[i] = -1;
	}

	FILE *const fp = fopen(filename, "r");
	if (!fp) {
		fprintf(stderr, "failed to open file \"%s\" for reading!\n", filename);
		exit(-1);
	}

	char *const line_buf = malloc(MAX_LINE_LENGTH);

	// check first row headers and get date range
	char *line = rota_get_line(line_buf, MAX_LINE_LENGTH, fp);
	if (!line) {
		fprintf(stderr, "failed to read first line of input file!\n");
		exit(-1);
	}
	int day_count = 0;
	for (int col = 0;; ++col) {
		char *const next = scan_for_next_column(line);
		if (!line || *line == '\0') {
			day_count = col - FIRST_DAY_COLUMN;
			if ((day_count % 7) != 0) {
				fprintf(stderr, "rota must be a whole number of weeks!\n");
				exit(-1);
			}
			config->week_count = day_count/7;
			if (config->week_count > MAX_WEEK_COUNT) {
				fprintf(stderr, "rota can be at most %d weeks!\n", MAX_WEEK_COUNT);
				exit(-1);
			}
			break;
		}
		if (col == FIRST_DAY_COLUMN) {
			config->first_day = parse_date(line);

			// check first day is the first day of the week
			tm_t *t = localtime(&config->first_day);
			if (!t || t->tm_wday != 1) {
				fprintf(stderr, "first rota day must be a Monday!\n");
				exit(-1);
			}
		}
		if (col > FIRST_DAY_COLUMN) {
			time_t const t = parse_date(line);
			if (t != config->first_day + (col - FIRST_DAY_COLUMN)*TIME_DELTA_DAY) {
				fprintf(stderr, "column %d has unexpected day!\n", col);
				exit(-1);
			}
		}
		line = next;
	}
	config->total_on_call_days_and_bias = 5*config->week_count;
	config->total_on_call_weekends_and_bias = config->week_count;
	config->total_ward_weeks_and_bias = config->week_count;

	// handle each row
	for (;;) {
		line = rota_get_line(line_buf, MAX_LINE_LENGTH, fp);
		if (!line) {
			break;
		}
		int person = -1;
		int category = -1;
		for (int col = 0; col < FIRST_DAY_COLUMN + day_count && line; ++col) {
			char *const next = scan_for_next_column(line);
			if (col == 0) {
				// skip rows with no person
				if (line && *line != '\0') {
					person = find_or_add_person(config, line);
				}
			} else if (col == 1) {
				// only allow bank holidays with no person
				if (!line || *line == '\0') {
					if (person == -1) {
						break;
					}
					fprintf(stderr, "expected category!\n");
					exit(-1);
				}
				category = match_category_by_tag(line);
				if (person == -1 && category != CATEGORY_BANK_HOLIDAY) {
					break;
				}
			} else if (*line != '\0') {
				int const rota_day_index = col - FIRST_DAY_COLUMN;
				int const weekday_index = rota_day_index % 7;
				float amount = 0.f;
				switch (category) {
					case CATEGORY_HOLIDAY:
						set_holiday_day(config, rota_day_index, person);
						break;

					case CATEGORY_CANNOT_ON_CALL_DAY:
						set_invalid_on_call_day(config, rota_day_index, person);
						break;

					case CATEGORY_CANNOT_ON_CALL_DAY_ALWAYS:
						for (int week_index = 0; week_index < config->week_count; ++week_index) {
							int const day_index_always = 7*week_index + weekday_index;
							set_invalid_on_call_day(config, day_index_always, person);
						}
						break;

					case CATEGORY_DISLIKE_ON_CALL_DAY:
						set_disliked_on_call_day(config, rota_day_index, person);
						break;

					case CATEGORY_DISLIKE_ON_CALL_DAY_ALWAYS:
						for (int week_index = 0; week_index < config->week_count; ++week_index) {
							int const day_index_always = 7*week_index + weekday_index;
							set_disliked_on_call_day(config, day_index_always, person);
						}
						break;

					case CATEGORY_CANNOT_WARD_WEEK:
						if (weekday_index < 5) {
							set_invalid_ward_week(config, rota_day_index/7, person);
						} else {
							fprintf(stderr, "found cannot ward week on a weekend day, ignoring it!\n");
						}
						break;

					case CATEGORY_DISLIKE_WARD_WEEK:
						if (weekday_index < 5) {
							set_disliked_ward_week(config, rota_day_index/7, person);
						} else {
							fprintf(stderr, "found dislike of ward week on a weekend day, ignoring it!\n");
						}
						break;

					case CATEGORY_PART_TIME:
						if (sscanf(line, "%f", &amount) != 1) {
							fprintf(stderr, "part time amount \"%s\" is not valid!\n", line);
							exit(-1);
						}
						if (amount < 0.f || 1.f < amount) {
							fprintf(stderr, "part time amount %f is not valid!\n", amount);
							exit(-1);
						}
						config->people[person].full_time_amount = amount;
						break;

					case CATEGORY_START_DATE:
						if (config->people[person].first_day != 0) {
							fprintf(stderr, "cannot set multiple start dates per person!\n");
							exit(-1);
						}
						config->people[person].first_day = rota_day_index;
						break;

					case CATEGORY_END_DATE:
						if (config->people[person].last_day != 7*config->week_count - 1) {
							fprintf(stderr, "cannot set multiple end dates per person!\n");
							exit(-1);
						}
						config->people[person].last_day = rota_day_index;
						break;

					case CATEGORY_BANK_HOLIDAY:
						set_bank_holiday(config, rota_day_index);
						++config->total_bank_holidays_and_bias;
						break;

					case CATEGORY_BANK_HOLIDAY_BIAS:
						config->people[person].bank_holiday_bias = atof(line);
						break;

					case CATEGORY_WARD_WEEK_BIAS:
						config->people[person].ward_week_bias = atof(line);
						break;

					case CATEGORY_ON_CALL_DAY_BIAS:
						config->people[person].on_call_day_bias = atof(line);
						break;

					case CATEGORY_ON_CALL_WEEKEND_BIAS:
						config->people[person].on_call_weekend_bias = atof(line);
						break;

					case CATEGORY_NO_WARD_WEEKS:
						config->people[person].cannot_do_ward_weeks = true;
						break;

					case CATEGORY_FORCE_ON_CALL_DAY:
						if (config->forced_on_call_people[rota_day_index] != -1) {
							fprintf(stderr, "multiple people are set as must be on call on the same day!\n");
							exit(-1);
						}
						config->forced_on_call_people[rota_day_index] = person;
						break;

					default:
						fprintf(stderr, "internal error: unknown category!\n");
						exit(-1);
						break;
				}
			}
			line = next;
		}
	}

	free(line_buf);
	fclose(fp);

	// compute effective full time rate according to first and last days, biased totals
	int const total_day_count = 7*config->week_count;
	for (int person_index = 0; person_index < config->person_count; ++person_index) {
		person_config_t *const person = &config->people[person_index];
		for (int i = 0; i < person->first_day; ++i) {
			set_holiday_day(config, i, person_index);
		}
		for (int i = person->first_day; i <= person->last_day; ++i) {
			if (!is_holiday_day(config, i, person_index)) {
				++person->total_non_holiday_days;
			}
		}
		for (int i = person->last_day + 1; i < total_day_count; ++i) {
			set_holiday_day(config, i, person_index);
		}

		int const person_day_count = 1 + person->last_day - person->first_day;
		float const rota_amount = (float)person_day_count/(float)total_day_count;
		person->effective_full_time_amount = person->full_time_amount*rota_amount;

		if (person->cannot_do_ward_weeks && person->ward_week_bias != 0) {
			fprintf(stderr, "person that cannot do ward weeks cannot have a ward week bias!\n");
			exit(-1);
		}

		config->total_on_call_days_and_bias += person->on_call_day_bias;
		config->total_on_call_weekends_and_bias += person->on_call_weekend_bias;
		config->total_ward_weeks_and_bias += person->ward_week_bias;
		config->total_bank_holidays_and_bias += person->bank_holiday_bias;
		config->effective_on_call_person_count += person->effective_full_time_amount;
		if (!person->cannot_do_ward_weeks) {
			config->effective_ward_person_count += person->effective_full_time_amount;
		}
	}

	// compute target working days and day off block sizes for each person
	for (int person_index = 0; person_index < config->person_count; ++person_index) {
		person_config_t *const person = &config->people[person_index];

		float const on_call_ratio = person->effective_full_time_amount/config->effective_on_call_person_count;
		float const ward_ratio = person->cannot_do_ward_weeks ? 0.f : (person->effective_full_time_amount/config->effective_ward_person_count);

		person->target_on_call_days = on_call_ratio*config->total_on_call_days_and_bias;
		person->target_on_call_weekends = on_call_ratio*config->total_on_call_weekends_and_bias;
		person->target_ward_weeks = ward_ratio*config->total_ward_weeks_and_bias;
		person->target_on_call_bank_holidays = on_call_ratio*config->total_bank_holidays_and_bias;

		float const expected_on_call_days = person->target_on_call_days - person->on_call_day_bias;
		float const expected_on_call_weekends = person->target_on_call_weekends - person->on_call_weekend_bias;
		float const expected_ward_weeks = person->target_ward_weeks - person->ward_week_bias;

		float const expected_working_days = expected_on_call_days + 2.f*expected_on_call_weekends + 5.f*expected_ward_weeks;

		float const expected_follow_on_shifts = MIN(expected_ward_weeks, expected_on_call_weekends);
		float const expected_shift_count = expected_on_call_days + expected_on_call_weekends + expected_ward_weeks - expected_follow_on_shifts;

		person->target_day_off_block_size = (person->total_non_holiday_days - expected_working_days)/expected_shift_count;
		person->target_ward_week_spacing = person->total_non_holiday_days/expected_ward_weeks;
	}
}

enum
{
	CHANNEL_HOLIDAY,
	CHANNEL_FORCED_ON_CALL,
	CHANNEL_CANNOT_ON_CALL,
	CHANNEL_DISLIKE_ON_CALL,
	CHANNEL_CANNOT_WARD_WEEK,
	CHANNEL_DISLIKE_WARD_WEEK,
	CHANNEL_COUNT
};

static char const *const g_channel_names[CHANNEL_COUNT] =
{
	NAME_HOLIDAY,
	NAME_FORCE_ON_CALL_DAY,
	NAME_CANNOT_ON_CALL_DAY,
	NAME_DISLIKE_ON_CALL_DAY,
	NAME_CANNOT_WARD_WEEK,
	NAME_DISLIKE_WARD_WEEK
};

void print_config_html(config_t const *config, points_t const *points, char const *filename)
{
	FILE *const fp = fopen(filename, "w");
	if (!fp) {
		fprintf(stderr, "failed to open \"%s\" for writing!\n", filename);
		exit(-1);
	}

	fprintf(fp, "<!DOCTYPE html>\n\
<html>\n\
<head>\n\
<style>\n\
table, th, td {\n\
border: 1px solid black;\n\
border-collapse: collapse;\n\
}\n\
th, td {\n\
padding: 5px;\n\
}\n\
table tr td {\n\
width: 10px;\n\
}\n\
</style>\n\
</head>\n\
<body>\n\
<h1>Rota Input</h1>\n\
");

	fprintf(fp, "<table>\n<tr>\n<th>Name</th><th>Category</th>\n");
	for (int week_index = 0; week_index < config->week_count; ++week_index) {
		time_t const first_day = config->first_day + week_index*7*TIME_DELTA_DAY;
		tm_t *const tm = localtime(&first_day);
		fprintf(fp, "<th colspan=\"7\">%d/%d/%d</th>\n", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + EPOCH_YEAR);
	}
	fprintf(fp, "</tr>\n");

	for (int person_index = 0; person_index < config->person_count; ++person_index) {
		person_config_t const *const person = &config->people[person_index];
		for (int channel = 0; channel < CHANNEL_COUNT; ++channel) {
			fprintf(fp, "<tr>");
			if (channel == 0) {
				fprintf(fp, "<td rowspan=\"%d\" style=\"white-space: nowrap\"><strong>%s</strong>", CHANNEL_COUNT, person->name);
				if (person->full_time_amount != 1.f) {
					fprintf(fp, "<br>%s: %f", NAME_PART_TIME, person->full_time_amount);
				}
				if (person->on_call_day_bias != 0.f) {
					fprintf(fp, "<br>%s: %f", NAME_ON_CALL_DAY_BIAS, person->on_call_day_bias);
				}
				if (person->on_call_weekend_bias != 0.f) {
					fprintf(fp, "<br>%s: %f", NAME_ON_CALL_WEEKEND_BIAS, person->on_call_weekend_bias);
				}
				if (person->ward_week_bias != 0.f) {
					fprintf(fp, "<br>%s: %f", NAME_WARD_WEEK_BIAS, person->ward_week_bias);
				}
				if (person->bank_holiday_bias != 0.f) {
					fprintf(fp, "<br>%s: %f", NAME_BANK_HOLIDAY_BIAS, person->bank_holiday_bias);
				}
				if (person->cannot_do_ward_weeks) {
					fprintf(fp, "<br>%s", NAME_NO_WARD_WEEKS);
				}
				fprintf(fp, "</td>\n");
			}
			fprintf(fp, "<td style=\"white-space: nowrap\">%s</td>\n", g_channel_names[channel]);
			for (int week_index = 0; week_index < config->week_count; ++week_index) {
				for (int weekday_index = 0; weekday_index < 7; ++weekday_index) {
					int const rota_day_index = 7*week_index + weekday_index;
					char const *extra = "";
					if (is_bank_holiday(config, rota_day_index)) {
						extra = " style=\"background-color:lightblue\"";
					} else if (weekday_index >= 5) {
						extra = " style=\"background-color:cyan\"";
					}
					switch (channel) {
						case CHANNEL_HOLIDAY:
							if (rota_day_index < person->first_day || person->last_day < rota_day_index) {
								extra = " style=\"background-color:black\"";
							} else if (is_holiday_day(config, rota_day_index, person_index)) {
								extra = " style=\"background-color:grey\"";
							}
							break;

						case CHANNEL_FORCED_ON_CALL:
							if (config->forced_on_call_people[rota_day_index] == person_index) {
								extra = " style=\"background-color:green\"";
							}
							break;

						case CHANNEL_CANNOT_ON_CALL:
							if (is_invalid_on_call_day(config, rota_day_index, person_index)) {
								extra = " style=\"background-color:darkred\"";
							}
							break;

						case CHANNEL_DISLIKE_ON_CALL:
							if (is_disliked_on_call_day(config, rota_day_index, person_index)) {
								extra = " style=\"background-color:red\"";
							}
							break;

						case CHANNEL_CANNOT_WARD_WEEK:
							if (weekday_index < 5 && is_invalid_ward_week(config, week_index, person_index)) {
								extra = " style=\"background-color:orange\"";
							}
							break;

						case CHANNEL_DISLIKE_WARD_WEEK:
							if (weekday_index < 5 && is_disliked_ward_week(config, week_index, person_index)) {
								extra = " style=\"background-color:yellow\"";
							}
							break;
					}
					fprintf(fp, "<td%s></td>\n", extra);
				}
			}
			fprintf(fp, "</tr>\n");
		}
	}
	fprintf(fp, "<table>\n");

	fprintf(fp, "<h1>Points</h1>\n<table>\n<tr><th>Name</th><th>Value</th></tr>");
	for (int i = 0; i < POINTS_COUNT; ++i) {
		fprintf(fp, "<tr><td>%s</td><td>%f</td>\n", g_points_names[i], points->values[i]);
	}
	fprintf(fp, "</table>\n");

	fprintf(fp, "</body>\n</html>\n");
	fclose(fp);

	printf("written input to \"%s\"\n", filename);
}

typedef struct
{
	rota_t *rota;
	score_t *score;
} state_t;

void swap_state(state_t *a, state_t *b)
{
	state_t const c = *a;
	*a = *b;
	*b = c;
}

void read_points(char const *filename, points_t *points)
{
	memset(points, 0, sizeof(points_t));

	FILE *const fp = fopen(filename, "r");
	if (!fp) {
		fprintf(stderr, "failed to open file \"%s\" for reading!\n", filename);
		exit(-1);
	}

	char *const line_buf = malloc(MAX_LINE_LENGTH);

	for (;;) {
		char *line = rota_get_line(line_buf, MAX_LINE_LENGTH, fp);
		if (!line) {
			break;
		}
		if (*line == '\0') {
			continue;
		}
		char *value = scan_for_next_column(line);
		for (int i = 0;; ++i) {
			if (i == POINTS_COUNT) {
				fprintf(stderr, "unknown points \"%s\"!\n", line);
				exit(-1);
			}
			if (strcmp(line, g_points_names[i]) == 0) {
				points->values[i] = (float)atof(value);
				break;
			}
		}
	}

	free(line_buf);
	fclose(fp);
}

int main(int argc, char *argv[])
{
	// deterministic seed
	init_genrand(0xABCD0123U);

	// parse arguments
	if (argc > 2) {
		fprintf(stderr, "rota\n");
		exit(-1);
	}
	char const *const input_filename = (argc == 2) ? argv[1] : "input.csv";

	// get some heap
	config_t *const config = (config_t *)malloc(sizeof(config_t));
	state_t current, candidate, best;
	current.rota = (rota_t *)malloc(sizeof(rota_t));
	current.score = (score_t *)malloc(sizeof(score_t));
	candidate.rota = (rota_t *)malloc(sizeof(rota_t));
	candidate.score = (score_t *)malloc(sizeof(score_t));
	best.rota = (rota_t *)malloc(sizeof(rota_t));
	best.score = (score_t *)malloc(sizeof(score_t));
	points_t *const points = malloc(sizeof(points_t));

	// read config from file
	read_config(input_filename, config);
	read_points("points.csv", points);
	print_config_html(config, points, "check.html");

	// randomly assign people to shifts
	for (int i = 0; i < config->week_count; ++i) {
		week_t *const week = &current.rota->weeks[i];
		for (int j = 0; j < SHIFT_COUNT; ++j) {
			week->shifts[j] = rota_rand(config->person_count);
		}
	}
	score_rota(config, points, current.rota, current.score);

	// mutate to global optimum
	int const run_count = 6*1024*1024;
	int const acceptance_half_life = 256*1024;
	int last_percent = 0;
	memcpy(best.rota, current.rota, sizeof(rota_t));
	memcpy(best.score, current.score, sizeof(score_t));
	for (int i = 0; i < run_count; ++i) {
		// progress?
		int const percent = (int)(100.f*(float)i/(float)run_count);
		if (percent != last_percent) {
			printf("\rworking: %d%% (%f/%f points)...          ", percent, best.score->value, current.score->value);
			fflush(stdout);
			last_percent = percent;
		}

		// do mutation
		switch (rota_rand(2)) {
			default:	mutate_random_reassign(config, current.rota, candidate.rota);	break;
			case 1:		mutate_random_swap(config, current.rota, candidate.rota);		break;
		}
		score_rota(config, points, candidate.rota, candidate.score);

		// accept randomly or if better
		float const accept_prob = powf(.5f, 1.f + (float)i/(float)acceptance_half_life);
		float const u = (float)rota_rand(run_count)/(float)run_count;
		if (candidate.score->value > current.score->value || u < accept_prob) {
			swap_state(&current, &candidate);
		}

		// keep track of best ever
		if (i == 0 || current.score->value > best.score->value) {
			memcpy(best.rota, current.rota, sizeof(rota_t));
			memcpy(best.score, current.score, sizeof(score_t));
		}
	}

	// print results
	printf("\rfinished! best score: %f (%s)          \n", best.score->value, (best.score->failure_count == 0) ? "valid" : "invalid");
	for (int i = 0; i < best.score->failure_count; ++i) {
		failure_data_t const *const data = &best.score->failure_data[i];
		person_config_t const *const person = &config->people[data->person_index];
		time_t const day = config->first_day + data->rota_day_index*TIME_DELTA_DAY;
		tm_t *const tm = localtime(&day);
		printf("%s: %s (%d/%d/%d)\n",
			person->name,
			g_failure_names[data->failure],
			tm->tm_mday, tm->tm_mon + 1, tm->tm_year + EPOCH_YEAR);
	}
	if (best.score->failure_count == MAX_FAILURE_COUNT) {
		printf("there are potentially more issues with the rota than those printed above...\n");
	}
	print_rota_html("output.html", config, best.rota, best.score);
	print_rota_csv("output.csv", config, best.rota);
	return 0;
}
