
#include "postgres.h"

#include <unistd.h>
#include <time.h>
#include "utils/trace.h"

typedef struct {
	uint64_t time;
	char event_type;
	uint64_t relid;
	uint64_t blocknum;
} trace_record_t;

// private variables
static FILE *trace_file = NULL;
static bool atexit_registered = false;
static uint64_t basetime;
static const int MAXFILENAMELEN = 1024;

// on-memory trace buffer
#define TRACE_RECORD_REGION 1 << 30L
#define NUM_TRACE_RECORD (TRACE_RECORD_REGION / sizeof(trace_record_t))
static trace_record_t *records = NULL;
static trace_record_t *cur_record = NULL;

void
start_trace(void)
{
	const char *save_dir;
	char filename[1024];
	char *cp;
	struct timespec tp;

	if (!enable_iotracer)
		return;

	if (trace_file != NULL)
		return;

	ereport(LOG, (errmsg("entered start_trace: pid = %d", getpid())));
	Assert(trace_file == NULL);
	bzero(filename, MAXFILENAMELEN * sizeof(char));
	if ((save_dir = getenv("PGTRACE")) == NULL)
		save_dir = ".";
	if (strlen(save_dir) > MAXFILENAMELEN)
		ereport(ERROR, (errmsg("too long save_dir: %s", save_dir)));
	strncpy(filename, save_dir, strlen(save_dir));
	cp = filename + strlen(save_dir);
	*cp = '/';
	cp ++;
	if (MAXFILENAMELEN - (cp - filename) < 20)
		ereport(ERROR, (errmsg("too long save_dir: %s", save_dir)));
	sprintf(cp, "trace_%d.log", getpid());

	if (!atexit_registered)
	{
		atexit(stop_trace);
		atexit_registered = true;
	}

    if ((trace_file = fopen(filename, "w+")) == NULL)
		ereport(ERROR, (errmsg("failed to open in write mode: %s", filename)));

	ereport(LOG, (errmsg("trace log file opened: %s", filename)));

	if (clock_gettime(CLOCK_REALTIME, &tp) != 0)
		ereport(ERROR, (errmsg("clock_gettime(2) failed.")));

	basetime = tp.tv_sec * 1000000000L + tp.tv_nsec;

	records = (trace_record_t *) calloc(NUM_TRACE_RECORD, sizeof(trace_record_t));
	cur_record = records;
}

void
stop_trace(void)
{
	int res;

	if (trace_file != NULL)
	{
		trace_flush();
		free(records);
		res = fclose(trace_file);
		if (res != 0)
			ereport(ERROR, (errmsg("[%d] stop_trace failed: errno = %d",
								   getpid(), res)));
		else
			ereport(INFO, (errmsg("[%d] stop_trace succeeded",
								  getpid())));

		trace_file = NULL;
	}
}

void
trace_flush(void)
{
	int res;
	trace_record_t *record;

	if (trace_file != NULL)
	{
		Assert(cur_record >= records);
		for (record = records; record < cur_record; record++) {
			fprintf(trace_file,
					"%lx\t" // timestamp in nanosec
					"%c\t" // event type
					"%lx\t" // relid
					"%lx\n", // event type
					record->time,
					record->event_type,
					record->relid,
					record->blocknum);
		}
		res = fflush(trace_file);
		if (res != 0)
			ereport(ERROR, (errmsg("[%d] trace_flush failed: errno = %d",
								   getpid(), res)));
		else
			ereport(DEBUG1, (errmsg("[%d] trace_flush succeeded",
									getpid())));
		cur_record = records;
	}
}

void
__trace_event(trace_event_t event, uint64_t relid, uint64_t blocknum)
{
	struct timespec tp;
	uint64_t time;

	Assert((uint64_t) cur_record - (uint64_t) records < TRACE_RECORD_REGION);

	if (clock_gettime(CLOCK_REALTIME, &tp) != 0)
		ereport(ERROR, (errmsg("clock_gettime(2) failed.")));

	time = tp.tv_sec * 1000000000L + tp.tv_nsec;
	time -= basetime;

	cur_record->time = time;
	cur_record->event_type = event;
	cur_record->relid = relid;
	cur_record->blocknum = blocknum;
	cur_record++;
	if ((uint64_t) cur_record - (uint64_t) records >= TRACE_RECORD_REGION)
		trace_flush();
}