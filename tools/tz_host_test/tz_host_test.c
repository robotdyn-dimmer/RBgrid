/* Host test: rbgrid_tz against glibc localtime_r/mktime, 31 POSIX zones, every 15 min 2023-2027.
 * gcc -O2 -Icomponents/rbpower/include -o /tmp/tz tools/tz_host_test/tz_host_test.c components/rbpower/src/rbgrid_tz.c && /tmp/tz */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rbgrid_tz.h"
static const char *ZONES[] = {
  "UTC0", "CET-1CEST,M3.5.0,M10.5.0/3", "EET-2EEST,M3.5.0/3,M10.5.0/4", "WET0WEST,M3.5.0/1,M10.5.0",
  "GMT0BST,M3.5.0/1,M10.5.0", "EST5EDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0", "MST7", "HST10",
  "AEST-10AEDT,M10.1.0,M4.1.0/3", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", "NZST-12NZDT,M9.5.0,M4.1.0/3",
  "IST-5:30", "<+0545>-5:45", "<-03>3", "<-03>3<-02>,M3.5.0/-2,M10.5.0/-1", "MSK-3", "HKT-8", "CST-8",
  "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0", "IST-1GMT0,M10.5.0,M3.5.0/1", "<-01>1<+00>,M3.5.0/0,M10.5.0/1",
  "XXX5YYY,J60/2,J300/2", "XXX5YYY,59,299", "EST5EDT", "CST6CDT,M3.2.0/2:00:00,M11.1.0/2:00:00",
  "<+13>-13", "<-12>12", "CET-1CEST,M3.5.0/2,M10.5.0/3", "AWST-8", "<+0330>-3:30",
};
int main(void) {
  long bad_lt = 0, bad_mk = 0, amb = 0, checked = 0;
  for (size_t z = 0; z < sizeof(ZONES)/sizeof(ZONES[0]); z++) {
    if (!rbgrid_tz_set(ZONES[z])) { printf("PARSE FAIL %s\n", ZONES[z]); bad_lt++; continue; }
    setenv("TZ", ZONES[z], 1); tzset();
    long zb = 0;
    /* every 15 min, 2023-01-01 .. 2028-01-01 */
    for (time_t t = 1672531200; t < 1830297600; t += 900) {
      struct tm a, b; localtime_r(&t, &a); rbgrid_localtime_r(&t, &b); checked++;
      if (a.tm_year!=b.tm_year||a.tm_mon!=b.tm_mon||a.tm_mday!=b.tm_mday||a.tm_hour!=b.tm_hour||a.tm_min!=b.tm_min||
          a.tm_sec!=b.tm_sec||a.tm_wday!=b.tm_wday||a.tm_yday!=b.tm_yday||a.tm_isdst!=b.tm_isdst) {
        if (zb++ < 3) printf("LT %s t=%ld glibc %04d-%02d-%02d %02d:%02d dst%d wd%d yd%d | rb %04d-%02d-%02d %02d:%02d dst%d wd%d yd%d\n",
          ZONES[z], (long)t, a.tm_year+1900,a.tm_mon+1,a.tm_mday,a.tm_hour,a.tm_min,a.tm_isdst,a.tm_wday,a.tm_yday,
          b.tm_year+1900,b.tm_mon+1,b.tm_mday,b.tm_hour,b.tm_min,b.tm_isdst,b.tm_wday,b.tm_yday);
        bad_lt++;
      }
      /* round trip: local fields -> mktime with isdst -1 */
      struct tm c = a; c.tm_isdst = -1; struct tm d = a; d.tm_isdst = -1;
      time_t g = mktime(&c), r = rbgrid_mktime(&d);
      if (g != r) { if (g == t || r == t) { if (r != t) { bad_mk++; if (zb++ < 3) printf("MK %s t=%ld glibc %ld rb %ld\n", ZONES[z], (long)t, (long)g, (long)r); } else amb++; } else bad_mk++; }
    }
    /* day boundary math: local midnight + mktime normalization (tm_mday+1) */
    for (time_t t = 1672531200; t < 1830297600; t += 86400 + 3601) {
      struct tm a; localtime_r(&t, &a); a.tm_hour=0; a.tm_min=0; a.tm_sec=0; a.tm_mday += 1; a.tm_isdst=-1;
      struct tm b = a; time_t g = mktime(&a), r = rbgrid_mktime(&b);
      if (g != r) { bad_mk++; if (zb++ < 3) printf("MID %s g=%ld r=%ld\n", ZONES[z], (long)g, (long)r); }
    }
  }
  printf("checked %ld instants: localtime mismatches %ld, mktime mismatches %ld (ambiguous-hour picks differing: %ld)\n", checked, bad_lt, bad_mk, amb);
  return (bad_lt || bad_mk) ? 1 : 0;
}
