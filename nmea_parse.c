#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "gps.h"
#include "gpsd.h"

/**************************************************************************
 *
 * Parser helpers begin here
 *
 **************************************************************************/

/* field() returns a string containing the nth comma delimited
   field from sentence string
 */

static char *field(char *sentence, short n)
{
    static char result[100];
    char c, *p = sentence;
    int i;

    while (n-- > 0)
        while ((c = *p++) != ',' && c != '\0');
    strncpy(result, p, 100);
    p = result;
    i = 0;
    while (*p && *p != ',' && *p != '*' && *p != '\r' && ++i<100)
	p++;

    *p = '\0';
    return result;
}

/* ----------------------------------------------------------------------- */

static void do_lat_lon(char *sentence, int begin, struct gps_data_t *out)
{
    double lat, lon, d, m;
    char str[20], *p;
    int updated = 0;

    if (*(p = field(sentence, begin + 0)) != '\0') {
	strncpy(str, p, 20);
	sscanf(p, "%lf", &lat);
	m = 100.0 * modf(lat / 100.0, &d);
	lat = d + m / 60.0;
	p = field(sentence, begin + 1);
	if (*p == 'S')
	    lat = -lat;
	if (out->latitude != lat) {
	    out->latitude = lat;
	}
	updated++;
    }
    if (*(p = field(sentence, begin + 2)) != '\0') {
	strncpy(str, p, 20);
	sscanf(p, "%lf", &lon);
	m = 100.0 * modf(lon / 100.0, &d);
	lon = d + m / 60.0;

	p = field(sentence, begin + 3);
	if (*p == 'W')
	    lon = -lon;
	if (out->longitude != lon) {
	    out->longitude = lon;
	}
	updated++;
    }
    if (updated == 2)
	out->latlon_stamp.changed = 1;
    REFRESH(out->latlon_stamp);
}

/* ----------------------------------------------------------------------- */

static int update_field_i(char *sentence, int fld, int *dest)
{
    int tmp, changed;

    tmp = atoi(field(sentence, fld));
    changed = (tmp != *dest);
    *dest = tmp;
    return changed;
}

static int update_field_f(char *sentence, int fld, double *dest)
{
    int changed;
    double tmp;

    tmp = atof(field(sentence, fld));
    changed = (tmp != *dest);
    *dest = tmp;
    return changed;
}

/**************************************************************************
 *
 * Scary timestamp fudging begins here
 *
 **************************************************************************/

/*
   Three sentences, GGA and GGL and RMC, contain timestamps.
   Timestamps always look like hhmmss.ss, with the trailing .ss part optional.
   RMC alone has a date field, in the format ddmmyy.  

   We want the output to be in ISO 8601 format:

   yyyy-mm-ddThh:mm:ss.sssZ
   012345678901234567890123

   (where part or all of the decimal second suffix may be omitted).
   This means that for GPRMC we must supply a century and for GGA and
   GGL we must supply a century, year, and day.

   We get the missing data from the host machine's clock time.  That
   is, the machine where this *daemon* is running -- which is probably
   connected to the GPS by a link short enough that it doesn't cross
   the International Date Line.  Even if it does, this hack could only
   screw the year number up for two hours around the first midnight of
   a new century.
 */

static void merge_ddmmyy(char *ddmmyy, struct gps_data_t *out)
/* sentence supplied ddmmyy, but no century part */
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    strftime(out->utc + 6, 3, "%C", tm);
    strncpy(out->utc, ddmmyy + 4, 2);	/* copy year */
    out->utc[4] = '-';
    strncpy(out->utc+5, ddmmyy + 2, 2);	/* copy month */
    out->utc[7] = '-';
    strncpy(out->utc + 8, ddmmyy, 2);	/* copy date */
    out->utc[10] = 'T';
}

static void fake_mmddyyyy(struct gps_data_t *out)
/* sentence didn't sypply mm/dd/yyy, so we have to fake it */
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    strftime(out->utc, sizeof(out->utc), "%Y-%m-%dT", tm);
}

static void merge_hhmmss(char *hhmmss, struct gps_data_t *out)
/* update last-fix field from a UTC time */
{
    strncpy(out->utc + 11, hhmmss, 2);	/* copy hours */
    out->utc[13] = ':';
    strncpy(out->utc + 14, hhmmss + 2, 2);	/* copy minutes */
    out->utc[16] = ':';
    strncpy(out->utc + 17 , 
	    hhmmss + 4, sizeof(out->utc)-17);	/* copy seconds */
    strcat(out->utc, "Z");
}

/**************************************************************************
 *
 * NMEA sentence handling begin here
 *
 **************************************************************************/

static void processGPRMC(char *sentence, struct gps_data_t *out)
/* Recommend Minimum Specific GPS/TRANSIT Data */
{
    /*
        RMC - Recommended minimum specific GPS/Transit data
        RMC,225446.33,A,4916.45,N,12311.12,W,000.5,054.7,191194,020.3,E*68
           225446.33    Time of fix 22:54:46 UTC
           A            Navigation receiver warning A = OK, V = warning
           4916.45,N    Latitude 49 deg. 16.45 min North
           12311.12,W   Longitude 123 deg. 11.12 min West
           000.5        Speed over ground, Knots
           054.7        Course Made Good, True
           191194       Date of fix  19 November 1994
           020.3,E      Magnetic variation 20.3 deg East
           *68          mandatory nmea_checksum

     */
    merge_ddmmyy(field(sentence, 9), out);
    merge_hhmmss(field(sentence, 1), out);

    do_lat_lon(sentence, 3, out);

    out->speed_stamp.changed = update_field_f(sentence, 7, &out->speed);
    REFRESH(out->speed_stamp);
    out->track_stamp.changed = update_field_f(sentence, 8, &out->track);
    REFRESH(out->track_stamp);
}

/* ----------------------------------------------------------------------- */

static void processGPGLL(char *sentence, struct gps_data_t *out)
/* Geographic position - Latitude, Longitude */
{
    /*
     * Described at 
     * <http://www.tri-m.com/products/royaltek/files/manual/teb1000_man.pdf>
     * as part of NMEA 3.0.  Here are the fields:
     *
     * 1,2 Latitude, N (North) or S (South)
     * 3,4 Longitude, E (East) or W (West)
     * 5 UTC of position
     * 6 Status: A=Valid, V=Invalid
     * 7 Mode Indicator
     *   A = Autonomous mode
     *   D = Differential Mode
     *   E = Estimated (dead-reckoning) mode
     *   M = Manual Input Mode
     *   S = Simulated Mode
     *   N = Data Not Valid
     *
     * I found a note at <http://www.secoh.ru/windows/gps/nmfqexep.txt>
     * indicating that the Garmin 65 does not return time and status.
     * This code copes gracefully.
     */
    char *status = field(sentence, 7);
    int newstatus = out->status;

    if (strcmp(field(sentence, 6), "V"))
    {
	do_lat_lon(sentence, 1, out);

	fake_mmddyyyy(out);
	merge_hhmmss(field(sentence, 5), out);
	if (status[0] == 'N')
	    newstatus = STATUS_NO_FIX;
	else if (status[0] == 'D')
	    newstatus = STATUS_DGPS_FIX;	/* differential */
	else
	    newstatus = STATUS_FIX;
	out->status_stamp.changed = (out->status != newstatus);
	out->status = newstatus;
	REFRESH(out->status_stamp);
	gpscli_report(3, "GPGLL sets status %d\n", out->status);
    }
}

/* ----------------------------------------------------------------------- */

static void processGPVTG(char *sentence, struct gps_data_t *out)
/* Track Made Good and Ground Speed */
{
    /* OK, there seem to be two variants of GPVTG
     * One, described at <http://www.sintrade.ch/nmea.htm>, looks like this:

	GPVTG Track Made Good and Ground Speed with GPS Talker ID
	(1) True course over ground (degrees) 000 to 359
	(2) Magnetic course over ground 000 to 359
	(3) Speed over ground (knots) 00.0 to 99.9
	(4) Speed over ground (kilometers) 00.0 to 99.9

     * Up to and including 1.10, gpsd assumed this and extracted field
     * 3 for ground speed.  There's a GPS manual at 
     * <http://www.tri-m.com/products/royaltek/files/manual/teb1000_man.pdf>
     * tha suggests this information was good for NMEA 3.0 at least.
     *
     * But, if you look in <http://www.kh-gps.de/nmea-faq.htm>. it says:

	$GPVTG,t,T,,,s.ss,N,s.ss,K*hh

	VTG  = Actual track made good and speed over ground

	1    = Track made good
	2    = Fixed text 'T' indicates that track made good is relative to 
	       true north
	3    = not used
	4    = not used
	5    = Speed over ground in knots
	6    = Fixed text 'N' indicates that speed over ground in in knots
	7    = Speed over ground in kilometers/hour
	8    = Fixed text 'K' indicates that speed over ground is in 
               kilometers/hour
	9    = Checksum

     * The actual NMEA spec, version 3.01, dated 1/1/2002, agrees with the
     * second source:

	1    = Track made good
	2    = Fixed text 'T' indicates that track made good is relative to 
	       true north
	3    = Magnetic course over ground
	4    = Fixed text 'M' indicates that course is relative to magnetic 
               north.
	5    = Speed over ground in knots
	6    = Fixed text 'N' indicates that speed over ground in in knots
	7    = Speed over ground in kilometers/hour
	8    = Fixed text 'K' indicates that speed over ground is in 
               kilometers/hour
	9    = Checksum

     * which means we want to extract field 5.  We'll deal with both
     * possibilities here.
     */
    int changed;

    changed = update_field_f(sentence, 1, &out->track);
    out->track_stamp.changed = changed;
    REFRESH(out->track_stamp);
    changed = 0;
    if (field(sentence, 2)[0] == 'T')
	changed |= update_field_f(sentence, 5, &out->speed);
    else
	changed |= update_field_f(sentence, 3, &out->speed);
    out->speed_stamp.changed = changed;
    REFRESH(out->speed_stamp);
}

/* ----------------------------------------------------------------------- */

static void processGPGGA(char *sentence, struct gps_data_t *out)
/* Global Positioning System Fix Data */
{
    /*
       GGA - Global Positioning System Fix Data
        GGA,123519,4807.038,N,01131.324,E,1,08,0.9,545.4,M,46.9,M, , *42
           123519       Fix taken at 12:35:19 UTC
           4807.038,N   Latitude 48 deg 07.038' N
           01131.324,E  Longitude 11 deg 31.324' E
           1            Fix quality: 0 = invalid
                                     1 = GPS fix
                                     2 = DGPS fix
           08           Number of satellites being tracked
           0.9          Horizontal dilution of position
           545.4,M      Altitude, Metres above mean sea level
           46.9,M       Height of geoid (mean sea level) above WGS84
                        ellipsoid, in Meters
           (empty field) time in seconds since last DGPS update
           (empty field) DGPS station ID number (0000-1023)
    */
    fake_mmddyyyy(out);
    merge_hhmmss(field(sentence, 1), out);
    do_lat_lon(sentence, 2, out);
    out->status_stamp.changed = update_field_i(sentence, 6, &out->status);
    REFRESH(out->status_stamp);
    gpscli_report(3, "GPGGA sets status %d\n", out->status);
    update_field_i(sentence, 7, &out->satellites_used);
    out->altitude_stamp.changed = update_field_f(sentence, 9, &out->altitude);
    REFRESH(out->altitude_stamp);
}

/* ----------------------------------------------------------------------- */

static void processGPGSA(char *sentence, struct gps_data_t *out)
/* GPS DOP and Active Satellites */
{
    /*
	eg1. $GPGSA,A,3,,,,,,16,18,,22,24,,,3.6,2.1,2.2*3C
	eg2. $GPGSA,A,3,19,28,14,18,27,22,31,39,,,,,1.7,1.0,1.3*35

	1    = Mode:
	       M=Manual, forced to operate in 2D or 3D
	       A=Automatic, 3D/2D
	2    = Mode:
	       1=Fix not available
	       2=2D
	       3=3D
	3-14 = PRNs of satellites used in position fix (null for unused fields)
	15   = PDOP
	16   = HDOP
	17   = VDOP
     */
    int i, changed = 0;
    
    out->mode_stamp.changed = update_field_i(sentence, 2, &out->mode);
    REFRESH(out->mode_stamp);
    gpscli_report(3, "GPGSA sets mode %d\n", out->mode);
    changed |= update_field_f(sentence, 15, &out->pdop);
    changed |= update_field_f(sentence, 16, &out->hdop);
    changed |= update_field_f(sentence, 17, &out->vdop);
    for (i = 0; i < MAXCHANNELS; i++)
	out->used[i] = 0;
    out->satellites_used = 0;
    for (i = 0; i < MAXCHANNELS; i++) {
	out->used[out->satellites_used++] = atoi(field(sentence, i));
    }
    out->fix_quality_stamp.changed = changed;
    REFRESH(out->fix_quality_stamp);
}

/* ----------------------------------------------------------------------- */

int nmea_sane_satellites(struct gps_data_t *out)
{
    /* data may be incomplete */
    if (out->part < out->await)
	return 0;

    /*
     * This sanity check catches an odd behavior of the BU-303, and thus
     * possibly of other SiRF-II based GPSes.  When they can't see any
     * satellites at all (like, inside a building) they sometimes cough
     * up a hairball in the form of a GSV packet with all the azimuth 
     * and entries 0 (but nonzero elevations).  This
     * was observed under SiRF firmware revision 231.000.000_A2.
     */
    int n;

    for (n = 0; n < out->satellites; n++)
	if (out->azimuth[n]) {
	    return 1;
	}
    return 0;
}

static void processGPGSV(char *sentence, struct gps_data_t *out)
/* GPS Satellites in View */
{
    /*
       GSV - Satellites in view
        GSV,2,1,08,01,40,083,46,02,17,308,41,12,07,344,39,14,22,228,45*75
           2            Number of sentences for full data
           1            sentence 1 of 2
           08           Total number of satellites in view
           01           Satellite PRN number
           40           Elevation, degrees
           083          Azimuth, degrees
           46           Signal-to-noise ratio in decibels
           <repeat for up to 4 satellites per sentence>
                There my be up to three GSV sentences in a data packet
     */

    int changed, lower, upper, fldnum = 4;

    out->await = atoi(field(sentence, 1));
    if (sscanf(field(sentence, 2), "%d", &out->part) < 1)
        return;

    changed = update_field_i(sentence, 3, &out->satellites);
    if (out->satellites > MAXCHANNELS) out->satellites = MAXCHANNELS;

    lower = (out->part - 1) * 4;
    upper = lower + 4;

    while (lower < out->satellites && lower < upper) {
	changed |= update_field_i(sentence, fldnum++, &out->PRN[lower]);
	changed |= update_field_i(sentence, fldnum++, &out->elevation[lower]);
	changed |= update_field_i(sentence, fldnum++, &out->azimuth[lower]);
	if (*(field(sentence, fldnum))) {
	    changed |= update_field_i(sentence, fldnum, &out->ss[lower]);
	}
	fldnum++;
	lower++;
    }

    /* not valid data until we've seen a complete set of parts */
    if (out->part < out->await)
	gpscli_report(3, "Partial satellite data (%d of %d).\n", out->part, out->await);
    else
    {
	/* trim off PRNs with spurious data attached */
	while (out->satellites
		    && !out->elevation[out->satellites-1]
		    && !out->azimuth[out->satellites-1]
		    && !out->ss[out->satellites-1])
	    out->satellites--;

	if (nmea_sane_satellites(out)) {
	    gpscli_report(3, "Satellite data OK.\n");
	    out->satellite_stamp.changed = changed;
	    REFRESH(out->satellite_stamp);
	}
	else
	    gpscli_report(3, "Satellite data no good.\n");
    }
}

/* ----------------------------------------------------------------------- */

static void processPMGNST(char *sentence, struct gps_data_t *out)
/* Proprietary MaGellan STatus */
{
    /*
      Only supported on Magellan GPSes.

      $PMGNST,02.12,3,T,534,05.0,+03327,00*40 

      where:
	  ST      status information
	  02.12   Version number?
	  3       2D or 3D
	  T       True if we have a fix False otherwise
	  534     numbers change - unknown
	  05.0    time left on the gps battery in hours
	  +03327  numbers change (freq. compensation?)
	  00      PRN number receiving current focus
	  *40    nmea_checksum
     */

   int tmp1, newstatus, newmode;
   char foo;
-
   /* using this for mode and status seems a bit desperate */
   /* only use it if we don't have better info */
   sscanf(field(sentence, 2), "%d", &tmp1);	
   sscanf(field(sentence, 3), "%c", &foo);	
   
   if (!SEEN(out->status_stamp)) {
	if (foo == 'T') {
	    newstatus = STATUS_FIX;
	    newmode = tmp1;
	}
	else {
	    newstatus = STATUS_NO_FIX;
	    newmode = MODE_NO_FIX;
	}
	out->status_stamp.changed = (newstatus != out->status);
	REFRESH(out->status_stamp);
	out->mode_stamp.changed = (newmode != out->mode);
	REFRESH(out->mode_stamp);
	gpscli_report(3, "PMGNST sets status %d, mode %d\n", out->status, out->mode);
    }
}

/* ----------------------------------------------------------------------- */

#ifdef PROCESS_PRWIZCH
static void processPRWIZCH(char *sentence, struct gps_data_t *out)
/*
 * Supported by the Zodiac/Rockwell chipset.
 * Descriptions of this sentence are hard to find, but here is one:
 *
 * $PRWIZCH ,00,0,03,7,31,7,15,7,19,7,01,7,22,2,27,2,13,0,11,7,08,0,02,0*4C
 *	SATELLITE IDENTIFICATION NUMBER - 0-31
 *	SIGNAL QUALITY - 0 low quality - 7 high quality
 *	Repeats 12 times
 */
{
    int i, changed = 0;

    for (i = 0; i < 12; i++) {
	changed |= update_field_i(sentence, 2 * i + 1, &out->Zs[i]);
	changed |= update_field_i(sentence, 2 * i + 2, &out->Zv[i]);
    }
    out->signal_quality_stamp.changed = changed;
    REFRESH(out->signal_quality_stamp);
}
#endif /* PROCESS_PRWIZCH */

/**************************************************************************
 *
 * Entry points begin here
 *
 **************************************************************************/

short nmea_checksum(char *sentence)
{
    unsigned char sum = '\0';
    char c, *p = sentence, csum[3];

    while ((c = *p++) != '*' && c != '\0')
	sum ^= c;

    sprintf(csum, "%02X", sum);
    return (strncmp(csum, p, 2) == 0);
}

void nmea_add_checksum(char *sentence)
{
    unsigned char sum = '\0';
    char c, *p = sentence;

    while ((c = *p++) != '*' && c != '\0')
	sum ^= c;

    sprintf(p, "%02X\r\n", sum);
}

int nmea_parse(char *sentence, struct gps_data_t *outdata)
{
    if (nmea_checksum(sentence+1)) {
	if (strncmp(GPRMC, sentence, sizeof(GPRMC)-1) == 0) {
	    processGPRMC(sentence, outdata);
	} else if (strncmp(GPGGA, sentence, sizeof(GPGGA)-1) == 0) {
	    processGPGGA(sentence, outdata);
	} else if (strncmp(GPGLL, sentence, sizeof(GPGLL)-1) == 0) {
	    processGPGLL(sentence, outdata);
	} else if (strncmp(PMGNST, sentence, sizeof(PMGNST)-1) == 0) {
	    processPMGNST(sentence, outdata);
	} else if (strncmp(GPVTG, sentence, sizeof(GPVTG)-1) == 0) {
	    processGPVTG(sentence, outdata);
	} else if (strncmp(GPGSA, sentence, sizeof(GPGSA)-1) == 0) {
	    processGPGSA(sentence, outdata);
	} else if (strncmp(GPGSV, sentence, sizeof(GPGSV)-1) == 0) {
	    processGPGSV(sentence, outdata);
	} else if (strncmp(PRWIZCH, sentence, sizeof(PRWIZCH)-1) == 0) {
#ifdef PROCESS_PRWIZCH
	    processPRWIZCH(sentence, outdata);
#else
	    /* do nothing */;
#endif /* PROCESS_PRWIZCH */
	} else {
	    return -1;
	}
    }
    return 0;
}

