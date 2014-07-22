/**************************************************************************

S0/Impulse to Volkszaehler 'RaspberryPI deamon'.

https://github.com/w3llschmidt/s0vz.git

Henrik Wellschmidt  <w3llschmidt@gmail.com>

**************************************************************************/

#define DAEMON_NAME "s0vz"
#define DAEMON_VERSION "1.4"
#define DAEMON_BUILD "4"

/**************************************************************************

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

**************************************************************************/

#include <stdio.h>              /* standard library functions for file input and output */
#include <stdlib.h>             /* standard library for the C programming language, */
#include <string.h>             /* functions implementing operations on strings  */
#include <unistd.h>             /* provides access to the POSIX operating system API */
#include <sys/stat.h>           /* declares the stat() functions; umask */
#include <fcntl.h>              /* file descriptors */
#include <syslog.h>             /* send messages to the system logger */
#include <errno.h>              /* macros to report error conditions through error codes */
#include <signal.h>             /* signal processing */
#include <stddef.h>             /* defines the macros NULL and offsetof as well as the types ptrdiff_t, wchar_t, and size_t */
#include <dirent.h>				/* constructs that facilitate directory traversing */

#include <libconfig.h>          /* reading, manipulating, and writing structured configuration files */
#include <curl/curl.h>          /* multiprotocol file transfer library */
#include <poll.h>			/* wait for events on file descriptors */
#include <pthread.h>
#include <semaphore.h>


#include <sys/ioctl.h>		/* */

#define BUF_LEN 64

void daemonShutdown();
void signal_handler(int sig);
void daemonize(char *rundir, char *pidfile);

int pidFilehandle, vzport, len, running_handles, rc, count, tempSensors;

const char *Datafolder, *Messstellenname, *Impulswerte[6],*uuid, *W1Sensor[100];
int Mittelwertzeit;


char crc_ok[] = "YES";
char not_found[] = "not found.";

char gpio_pin_id[] = { 17, 18, 27, 22, 23, 24 }, url[128];

int inputs = sizeof(gpio_pin_id)/sizeof(gpio_pin_id[0]);


double temp;
config_t cfg;

struct timeval tv;

struct valuePack
{
	double valuesAsSumm;
	int numberOfValues;
	int impulsConst;
	long lastTs;
};

struct valuePack *values;

sem_t sem_averrage;

CURL *easyhandle[sizeof(gpio_pin_id)/sizeof(gpio_pin_id[0])];
CURLM *multihandle;
CURLMcode multihandle_res;

static char errorBuffer[CURL_ERROR_SIZE+1];

void signal_handler(int sig) {

	switch(sig)
	{
		case SIGHUP:
		syslog(LOG_WARNING, "Received SIGHUP signal.");
		break;
		case SIGINT:
		case SIGTERM:
		syslog(LOG_INFO, "Daemon exiting");
		daemonShutdown();
		exit(EXIT_SUCCESS);
		break;
		default:
		syslog(LOG_WARNING, "Unhandled signal %s", strsignal(sig));
		break;
	}
}

void daemonShutdown() {
		close(pidFilehandle);
		remove("/tmp/s0vz.pid");
}

void daemonize(char *rundir, char *pidfile) {
	int pid, sid, i;
	char str[10];
	struct sigaction newSigAction;
	sigset_t newSigSet;

	if (getppid() == 1)
	{
		return;
	}

	sigemptyset(&newSigSet);
	sigaddset(&newSigSet, SIGCHLD);
	sigaddset(&newSigSet, SIGTSTP);
	sigaddset(&newSigSet, SIGTTOU);
	sigaddset(&newSigSet, SIGTTIN);
	sigprocmask(SIG_BLOCK, &newSigSet, NULL);

	newSigAction.sa_handler = signal_handler;
	sigemptyset(&newSigAction.sa_mask);
	newSigAction.sa_flags = 0;

	sigaction(SIGHUP, &newSigAction, NULL);
	sigaction(SIGTERM, &newSigAction, NULL);
	sigaction(SIGINT, &newSigAction, NULL);

	pid = fork();

	if (pid < 0)
	{
		exit(EXIT_FAILURE);
	}

	if (pid > 0)
	{
		printf("Child process created: %d\n", pid);
		exit(EXIT_SUCCESS);
	}

	umask(027);

	sid = setsid();
	if (sid < 0)
	{
		exit(EXIT_FAILURE);
	}

	for (i = getdtablesize(); i >= 0; --i)
	{
		close(i);
	}

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	chdir(rundir);

	pidFilehandle = open(pidfile, O_RDWR|O_CREAT, 0600);

	if (pidFilehandle == -1 )
	{
		syslog(LOG_INFO, "Could not open PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}

	if (lockf(pidFilehandle,F_TLOCK,0) == -1)
	{
		syslog(LOG_INFO, "Could not lock PID lock file %s, exiting", pidfile);
		exit(EXIT_FAILURE);
	}

	sprintf(str,"%d\n",getpid());

	write(pidFilehandle, str, strlen(str));
}

void cfile() {
    int i = 0;
	config_t cfg;

	//config_setting_t *setting;

	config_init(&cfg);

	int chdir(const char *path);

	//chdir ("/etc");

	if(!config_read_file(&cfg, DAEMON_NAME".cfg"))
	{
		syslog(LOG_INFO, "Config error > %s - %s\n", config_error_file(&cfg),config_error_text(&cfg));
		config_destroy(&cfg);
		daemonShutdown();
		exit(EXIT_FAILURE);
	}

	if (!config_lookup_string(&cfg, "Datafolder", &Datafolder))
	{
		syslog(LOG_INFO, "Missing 'Datafolder' setting in configuration file.");
		config_destroy(&cfg);
		daemonShutdown();
		exit(EXIT_FAILURE);
	}
	else
	syslog(LOG_INFO, "Datafolder:%s", Datafolder);

	if (!config_lookup_string(&cfg, "Messstelle", &Messstellenname))
	{
		syslog(LOG_INFO, "Missing 'Messstelle' setting in configuration file.");
		config_destroy(&cfg);
		daemonShutdown();
		exit(EXIT_FAILURE);
	}
	else
	syslog(LOG_INFO, "Messstelle:%s", Messstellenname);


	if (!config_lookup_int(&cfg, "Mittelwertzeit", &Mittelwertzeit))
	{
		syslog(LOG_INFO, "Missing 'Mittelwertzeit' setting in configuration file.");
		config_destroy(&cfg);
		daemonShutdown();
		exit(EXIT_FAILURE);
	}
	else
	syslog(LOG_INFO, "Mittelwertzeit:%i", Mittelwertzeit);

	for (i=0; i<inputs; i++)
	{
		char gpio[6];
		sprintf ( gpio, "GPIO%01d", i );
		if ( config_lookup_string( &cfg, gpio, &Impulswerte[i]) == CONFIG_TRUE )
		syslog ( LOG_INFO, "%s = %s", gpio, Impulswerte[i] );
	}

	tempSensors = 0;
	for (i=0; i<100; i++)
	{
		char name[8];
		sprintf ( name, "W1Dev%01d", i );
		if ( config_lookup_string( &cfg, name, &W1Sensor[i]) == CONFIG_TRUE )
		{
			syslog ( LOG_INFO, "%s = %s", name, W1Sensor[i] );
			tempSensors++;
		}
	}

}

unsigned long long unixtime() {

	gettimeofday(&tv,NULL);
	unsigned long long ms_timestamp = (unsigned long long)(tv.tv_sec) * 1000 + (unsigned long long)(tv.tv_usec) / 1000;

return ms_timestamp;
}

int appendToFile(const char *filename, char *str)
{
	FILE *fd;
	struct stat st = {0};
	struct tm* ptm;
	char time_string[11];
	char filepath[200];

	/* Create directory if not exist*/
	if (stat(filename, &st) == -1) {
	    mkdir(filename, 0700);
	}

	/* Filename ermitteln anhand des Datums */
	gettimeofday (&tv, NULL);
	ptm = localtime (&tv.tv_sec);
	strftime (time_string, sizeof (time_string), "%Y-%m-%d", ptm);
	sprintf(filepath,"%s/%s.csv",filename, time_string);
	printf("Now will add to file: %s this string: %s",filepath, str);

	fd = fopen(filepath, "a");
	if (fd != NULL)
	{
		fputs(str, fd);
		fclose(fd);
		return 0;
	}
	return 1;
}

void update_average_values(struct valuePack *vP) {
	unsigned long ts = unixtime();
	int time = 0;
	double wattProImpuls = 0;
	double tmp_value = 0;
	if (vP->lastTs != 0)
	{
		sem_wait(&sem_averrage);
		time = (int)(ts-vP->lastTs);
		wattProImpuls = 1000.0 / (double)vP->impulsConst;
        tmp_value = wattProImpuls * (3.6 / (double)time) * 1000000.0; // Zeit in MS
	    vP->valuesAsSumm += tmp_value / 1000.0;
	    vP->numberOfValues++;
	    sem_post(&sem_averrage);
	    printf("Summe: %.3f Anzahl %d TMPValue: %.3f Zeit: %d ms \n", vP->valuesAsSumm, vP->numberOfValues, tmp_value, time );
	}

	vP->lastTs = ts;

}

void *intervallFunction(void *time) { // Der Type ist wichtig: void* als Parameter und Rückgabe
	int t = *((int*) time);
	int i=0;
	double averrage[6];
	char str[200];
	printf("Thread created\n");

	while(1)
	{
		sleep(t);
		sem_wait(&sem_averrage);
		for (i=0; i<(inputs + tempSensors); i++) {
			if (values[i].numberOfValues > 0 )
			{
				averrage[i] = values[i].valuesAsSumm / values[i].numberOfValues;
			}
			else
			{
				averrage[i] = 0;
			}
			values[i].numberOfValues = 0;
			values[i].valuesAsSumm = 0;
			sprintf(str,"%s%.3f;",str, averrage[i]);
		}
		sem_post(&sem_averrage);

		sprintf(str,"%s%c",str,'\n');
		if (appendToFile(Datafolder, str) != 0)
		{
			printf("Can not append to File %s.", "filename_noch_nicht_vergeben");
		}
		printf("%s\n",str);
		str[0] = '\0';

	}
	printf("Thread wird beendet\n");
    return NULL;  // oder in C++: return 0;// Damit kann man Werte zurückgeben
}

/** *********************************
 *Beginn der Temperatur Funktionen
 */


int ds1820read(const char *sensorid, double *temp) {

	FILE *fp;
	char crc_buffer[64], temp_buffer[64], fn[128];

	printf("Lese Temperatur von %s.\n", sensorid);
	sprintf(fn, "/sys/bus/w1/devices/%s/w1_slave", sensorid );

	if  ( (fp = fopen ( fn, "r"  )) == NULL ) {
		return (-1);
	}
	else
	{

		fgets( crc_buffer, sizeof(crc_buffer), fp );
		if ( !strstr ( crc_buffer, crc_ok ) )
	 	{

			syslog(LOG_INFO, "CRC check failed, SensorID: %s", sensorid);

		fclose ( fp );
		return(-1);
		}

		else

		{

		fgets( temp_buffer, sizeof(temp_buffer), fp );
		fgets( temp_buffer, sizeof(temp_buffer), fp );

		/**************************************************************************
		char *t;
		t = strndup ( temp_buffer +29, 5 ) ;
		temp = atof(t)/1000;
		**************************************************************************/

		char *pos = strstr(temp_buffer, "t=");

		if (pos == NULL)
			return -1;

		pos += 2;

		*temp = atof(pos)/1000;
		fclose ( fp );

		//http_post(temp, vzuuid[i][count]);
		return 0;
		}
	}
}

void *intervallTemperatur(void *time) { // Der Type ist wichtig: void* als Parameter und Rückgabe
	int t = *((int*) time);
	int i = 0;
	int SensorNumber = 0;
	double temp;
	int returnValue;
    printf("Temperatur Thread created\n");

	while(1)
	{
		SensorNumber = 0;
		for (i=0; i<100; i++) {
			if ( W1Sensor[i] != NULL )
			{
				returnValue = ds1820read(W1Sensor[i], &temp);
				if (returnValue == 0)
				{
					printf("Sensor %d %s: Temperatur: %f abspeichern\n", i, W1Sensor[i], temp );
					sem_wait(&sem_averrage);
					values[inputs + SensorNumber].valuesAsSumm += temp;
					values[inputs + SensorNumber].numberOfValues ++;
					sem_post(&sem_averrage);
					printf("Sensor %s: Temperatur: %f\n",W1Sensor[i], temp );
				}
				else
					printf("Sensor %d %s: Temperatur: %f kann nicht abspeichert werden.\n", i, W1Sensor[i], temp );
			}
		}
		SensorNumber++;
		sleep(t);
	}
	printf("Thread wird beendet\n");
    return NULL;  // oder in C++: return 0;// Damit kann man Werte zurückgeben
}

int main(void) {
	int i=0;
	//freopen( "/dev/null", "r", stdin);
	//freopen( "/dev/null", "w", stdout);
	//freopen( "/dev/null", "w", stderr);

	FILE* devnull = NULL;
	devnull = fopen("/dev/null", "w+");

	setlogmask(LOG_UPTO(LOG_INFO));
	openlog(DAEMON_NAME, LOG_CONS | LOG_PERROR, LOG_USER);
	printf("Programm beginnt....");
	syslog ( LOG_INFO, "S0/Impulse to Volkszaehler RaspberryPI deamon %s.%s", DAEMON_VERSION, DAEMON_BUILD );

	cfile();

	char pid_file[16];
	sprintf ( pid_file, "/tmp/%s.pid", DAEMON_NAME );
	//daemonize( "/tmp/", pid_file );

	values = (struct valuePack*) malloc ( sizeof (struct valuePack) * (inputs + tempSensors));

	char buffer[BUF_LEN];
	struct pollfd fds[inputs];

	curl_global_init(CURL_GLOBAL_ALL);
	multihandle = curl_multi_init();

	for (i=0; i<inputs; i++) {
		printf("Current: %d\n", i);
		snprintf ( buffer, BUF_LEN, "/sys/class/gpio/gpio%d/value", gpio_pin_id[i] );

		if((fds[i].fd = open(buffer, O_RDONLY|O_NONBLOCK)) == 0) {

			syslog(LOG_INFO,"Error:%s (%m)", buffer);
			exit(1);

		}

		fds[i].events = POLLPRI;
		fds[i].revents = 0;

		easyhandle[i] = curl_easy_init();

		curl_easy_setopt(easyhandle[i], CURLOPT_URL, url);
		curl_easy_setopt(easyhandle[i], CURLOPT_POSTFIELDS, "");
		curl_easy_setopt(easyhandle[i], CURLOPT_USERAGENT, DAEMON_NAME " " DAEMON_VERSION );
		curl_easy_setopt(easyhandle[i], CURLOPT_WRITEDATA, devnull);
		curl_easy_setopt(easyhandle[i], CURLOPT_ERRORBUFFER, errorBuffer);

		curl_multi_add_handle(multihandle, easyhandle[i]);


		values[i].numberOfValues = 0;
		values[i].valuesAsSumm = 0;
		values[i].impulsConst = 1000;
		values[i].lastTs = 0;

	}
	for (i=inputs; i<(inputs+ tempSensors);i++ )
	{
		values[i].numberOfValues = 0;
		values[i].valuesAsSumm = 0;
		values[i].impulsConst = 1000;
		values[i].lastTs = 0;
	}

	sem_init(&sem_averrage, 0, 1);
	/* Thread erstellen für interval Berechnung*/
	pthread_t intervalThread, intervalTemperaturThread;
	if (pthread_create( &intervalThread, NULL, intervallFunction, (void *) &Mittelwertzeit ) != 0)
	{
		printf("Thread can not be create.");
		exit(1);
	}

	if (pthread_create( &intervalTemperaturThread, NULL, intervallTemperatur, (void *) &Mittelwertzeit ) != 0)
	{
		printf("Thread can not be create.");
		exit(1);
	}

	for ( ;; ) {

		if((multihandle_res = curl_multi_perform(multihandle, &running_handles)) != CURLM_OK) {
		syslog(LOG_INFO, "HTTP_POST(): %s", curl_multi_strerror(multihandle_res) );
		}

		int ret = poll(fds, inputs, 1000);

		if(ret>0) {

			for (i=0; i<inputs; i++) {
				if (fds[i].revents & POLLPRI) {
				len = read(fds[i].fd, buffer, BUF_LEN);
				//update_curl_handle(vzuuid[i]);
				update_average_values( &values[i]);
				}
			}
		}
	}

	curl_global_cleanup();

	return 0;
}
