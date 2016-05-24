#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <linux/perf_event.h>
#include <nvml.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Global variables */

/* BEGIN CONFGURATION */
#define VERSION "1.2"
//#define VERBOSE 1
/* default interval beween measurements */
useconds_t interval = 500000;
/* Maximum number of cores in a machine */
#define MAX_CORES	256
/* Maximum number of NVLM devices */
#define MAX_NVML 4
/* END CONFGURATION */

/* Flag to know if the NVIDIA API has been initialized */
int nvml_up = 0;
/* List and count of NVIDIA devices */
nvmlDevice_t device_list[MAX_NVML];
unsigned int device_count;
/* Energy accumulator for NVIDIA devices */
double nvml_energy[MAX_NVML];
/* Flag to know if perf RAPL events have been initialized */
int rapl_up = 0;
/* The last time a measurement was made. Needed to convert energy to power in RAPL measurements */
struct timeval last_time;
/* Number of cores detected in the machine */
int core_count = -1;

/* Thread and mutex to access nvml thread */
pthread_t nvml_thread;
pthread_mutex_t nvml_mutex;
pthread_barrier_t nvml_barrier;

#define NUM_RAPL_DOMAINS	4

char rapl_domain_names[NUM_RAPL_DOMAINS][30]= {
	"cores",
	"gpu",
	"pkg",
	"ram",
};

char units[BUFSIZ];
int fd[MAX_CORES][NUM_RAPL_DOMAINS];
long long last_value[MAX_CORES][NUM_RAPL_DOMAINS];
double last_value_nvml[MAX_NVML];
double scale[NUM_RAPL_DOMAINS];

/* Functions */
void usage(int argc, char **argv);
void help(int argc, char **argv);
int list_nvidia_devices(nvmlDevice_t *device_list, unsigned int *device_count);
void query_nvml_device(int device, long long delta);
void close_and_exit();
void alarm_handler (int signo);
int init_rapl_perf();
void reset_nvml();
void reset_rapl_perf();
void query_rapl_device(int core,long long delta);
void close_rapl_perf();
void print_energy();
void energy_nvml_device(int device);
void energy_rapl_device(int core);
void * monitor_nvml(void *arg);

int main(int argc, char **argv)
{
   /* Generic iterators */
   int i,j;
   /* getopt support variables */
   int c = 0;
   /* Flag to indicate if only the ROI has to be measured */
   int flag_roi = 0;

   /* Pid of child and return status */
   pid_t child_id;
   int status;
   /* Pipe to connect child's stdout to parent and file descrptor for the parent */
   int pipe_stdout[2];
   FILE *child_stdout;
   /* Array of strings to pass command line to child */
   char *exec_args[99];
   /* Pointer to the line read from the child, its current
    * size and the number of characters in it */
   char *line = NULL;
   size_t len = 0;
   ssize_t read;
   /* Return value of NVIDIA API */
   nvmlReturn_t result;
   /* To convert options to integers */
   char* endp;
   long l;

   /* Disable getopt error reporting */
   opterr = 0;
   /* Process options with getopt */
   while ((c = getopt (argc, argv, "c::r::h::v::i::")) != -1)
      switch (c) {
         case 'r':
            flag_roi = 1;
            break;
         case 'c':
            endp = NULL;
            l = -1;
            if (!optarg || (l=strtol(optarg, &endp, 10)), (endp && *endp)) {
               printf("Invalid core count %s - expecting a number.\n", optarg?optarg:"(null)");
               close_and_exit(EXIT_FAILURE);
            }
            core_count = l;
            break;
         case 'i':
            endp = NULL;
            l = -1;
            if (!optarg || (l=strtol(optarg, &endp, 10)), (endp && *endp)) {
               printf("Invalid interval %s - expecting a number of miliseconds.\n", optarg?optarg:"(null)");
               close_and_exit(EXIT_FAILURE);
            }
            if (l >= 1000) {
               printf("Interval %ld too long - should be less than 1000.\n", l);
               close_and_exit(EXIT_FAILURE);
            }
            interval = l*1000;
            break;
         case 'v':
            printf("sauna %s\n",VERSION);
            close_and_exit(0);
         case 'h':
            help(argc, argv);
            close_and_exit(0);
         case '?':
            if (isprint (optopt))
               printf ("Error: Unknown option `-%c'.\n", optopt);
            else
               printf ("Error: Unknown option character `\\x%x'.\n", optopt);
         default:
            usage(argc, argv);
            close_and_exit (0);
      }
  
   /* Ensure that the number of arguments is correct. */
   if(optind == argc) {
      printf ("Error: Insufficient arguments.\n");
      usage(argc, argv);
      close_and_exit (0);
   }

   /* Prepare a NULL terminated array of strings to pass to execv, after the fork. */
   for(i = optind, j = 0; i < argc; i++, j++) {
      exec_args[j] = argv[i];
   }
   exec_args[j] = NULL;

   /* Prepare communication channel with the child process. */
   if(pipe(pipe_stdout) < 0) {
      printf ("Error: could not open pipe.\n");
      close_and_exit(0);
   }
   
   /* Create a file descriptor to the reading end of the pipe. */
   if((child_stdout = fdopen(pipe_stdout[0], "r")) == NULL) {
      printf ("Error: could create file descriptor to pipe.\n");
      close_and_exit(0);
   }

   /* Initialize NVIDIA API */
   if ((result = nvmlInit()) != NVML_SUCCESS) {
      printf("Error: Failed to initialize NVML: %s\n", nvmlErrorString(result));
      close_and_exit(0);
   }
   nvml_up = 1;

   if((result = list_nvidia_devices(device_list,&device_count)) != NVML_SUCCESS) {
      printf("Error: Failed to list NVIDIA devices: %s\n", nvmlErrorString(result));
      close_and_exit(0);
   }

   /* TODO Check that MAX_NVML > device_count */

   /* Initialize NVIDIA monitor mutex */
   if(pthread_mutex_init(&nvml_mutex, NULL) != 0) {
      printf("Error: Failed to initialize NVLM mutex.");
      close_and_exit(0);
   }

   if(pthread_barrier_init(&nvml_barrier, NULL, 2) != 0) {
      printf("Error: Failed to initialize NVLM barrier.");
      close_and_exit(0);
   }

   /* Start NVIDIA monitor thread */
   if(pthread_create(&nvml_thread, NULL, &monitor_nvml, NULL ) != 0) {
      printf("Error: Failed to start NVLM monitoring thread.");
      close_and_exit(0);
   }
   /* Wait for nvml thread to make first measurement */
   pthread_barrier_wait(&nvml_barrier);


   /* Initialize perf RAPL */
   if(init_rapl_perf() < 0) {
      printf ("Error: Failed to intialize perf RAPL events.\n");
      close_and_exit (0);
   }

   /* Fork child process */
   if((child_id = fork()) < 0) {
      printf ("Error: unable to fork child process.\n");
      close_and_exit (0);
   }

   if(child_id == 0) {
      /* Connect stdout of child process to pipe. */
      close(pipe_stdout[0]); 
      if(dup2(pipe_stdout[1],1) < 0) {
         printf ("Error: failed to duplicate file descriptor in child process.\n");
         close_and_exit(1);
      }

      /* The child process is replaced by the program supplied by the user. */
      if(execvp(exec_args[0],exec_args) == -1) {
         printf ("Error: failed to exec \"%s\" in child process. %s\n",exec_args[0],strerror(errno));
/*         for(i = 0; exec_args[i] != NULL; i++) 
              printf("%s%s",exec_args[i],exec_args[i+1] != NULL ? " ": "");
           printf("\n");
          */
      }
      close_and_exit(1);
   }

   signal (SIGALRM, alarm_handler);
   close(pipe_stdout[1]); 

   /* If the ROI analysis flag is not set, start measurements immediately */
   if(! flag_roi) {
      reset_nvml();
      reset_rapl_perf();
      ualarm(interval, interval);
      gettimeofday(&last_time,NULL);
   }
   /* The master process reads stdin of the child process */
   while ((read = getline(&line, &len, child_stdout)) != -1) {
      /* If ROI analysis is set, and begining of ROI is detected start measurements */
      if(flag_roi && strstr(line, "++ROI")) {
         reset_nvml();
         reset_rapl_perf();
         ualarm(interval, interval);
         gettimeofday(&last_time,NULL);
      }
      /* Stop measurements at the end of the ROI */
      else if(flag_roi && strstr(line, "--ROI")) {
         flag_roi = 1;
         ualarm(0, interval);
         print_energy();
      }
      fputs(line, stdout);
   }
   /* Stop measurements when the child dies */
   if(flag_roi < 1) {
      ualarm(0, interval);
      print_energy();
   }

   /* Reap child */
   waitpid(child_id,&status,0);
   fclose(child_stdout);

   /* Release memory allocated to line */
   if(line)
      free(line);

   close_and_exit(1);
   return 0;
}

void usage(int argc, char **argv) {
      printf ("Usage: %s [-rh] <command> [<arguments>]\n", argv[0]);
}

void help(int argc, char **argv) {
      usage(argc,argv);
      printf (
            "\n"
            "This program performs a sequence of power measurements during the execution of <command>,\n"
            "that can take any number of <arguments>.\n"
            "\n"
            "   -r Forces the measurements to be performed within a region of interest (ROI). The ROI\n"
            "      is considered from the instant when <command> writes the string \"+++ROI\" to\n"
            "      stdout, to the moment it writes \"---ROI\" or its execution ends.\n"
            "\n"
            "   -h Displays this message.\n"
            "\n"
            );
}

double get_seconds() { /* routine to read time */
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void * monitor_nvml(void *arg) {
   int i;
   unsigned int power[MAX_NVML];
   double last_time, current_time;
    
   /* First iteration */
   last_time = get_seconds();

   usleep(14992); // 66.7 Hz 
   pthread_barrier_wait(&nvml_barrier);
   /* Next iterations */
   while(1) {
      current_time = get_seconds();
      for(i=0; i<device_count; i++) {
         nvmlDeviceGetPowerUsage (device_list[i], &power[i]);
      }
      pthread_mutex_lock( &nvml_mutex );
      for(i=0; i<device_count; i++) {
         nvml_energy[i] += (double)power[i]/1000.0*(current_time-last_time);
      }
      pthread_mutex_unlock( &nvml_mutex ); 
      last_time = current_time;
      usleep(14992); // 66.7 Hz 
   }
   return NULL;
}
/* cat raw.dat | sed 'n; d' | perl -nle '@b=split(" "); push @a,[@b] if($#a < 0 || $b[1] != $a[$#a][1] || $b[0] - $a[$#a][0] > 0.004); sub END { print $#a; print join("\n", map { join(" ",$a[$_][0],$a[$_][1]+0.04*($a[$_+1][1]-$a[$_-1][1])/($a[$_+1][0]-$a[$_-1][0]))} 0..$#a); }' > cook.dat 
      echo 'plot "cook.dat" u 1:2 w lp'  | gnuplot -persist

void * monitor_nvml(void *arg) {
   int i;
   nvmlReturn_t result;
   unsigned int nvml_last[MAX_NVML][2], power_nvml;
   double nvml_timestamp[MAX_NVML][2], current_time, power_true;
    

   for(i=0; i<device_count; i++) {
      nvmlDeviceGetPowerUsage (device_list[i], &power_nvml);
      nvml_timestamp[i][0] = nvml_timestamp[i][1] = get_seconds();
      nvml_last[i][0] = nvml_last[i][1] = power_nvml;
   }

   FILE *f = fopen("raw.dat","w");
   while(1) {
      for(i=0; i<device_count; i++) {
         nvmlDeviceGetPowerUsage (device_list[i], &power_nvml);
         current_time = get_seconds();
         fprintf(f,"%lf %u\n",current_time,power_nvml);
         if(power_nvml == nvml_last[i][0] && current_time-nvml_timestamp[i][0] < 4e3)
            continue;
         
         power_true = (double)nvml_last[i][0]+0.84*(power_nvml-nvml_last[i][1])/(current_time-nvml_timestamp[i][1]);

         if(power_true > 52.5) {
            pthread_mutex_lock( &nvml_mutex );
            nvml_energy[i] += (double)power_true*(current_time-nvml_timestamp[i][0]);
            pthread_mutex_unlock( &nvml_mutex ); 
         }
         nvml_timestamp[i][1] = nvml_timestamp[i][0];
         nvml_timestamp[i][0] = current_time;
         nvml_last[i][1] = nvml_last[i][0];
         nvml_last[i][0] = power_nvml;
      }
      usleep(14992); // 66.7 Hz 
      //usleep(3748); // 266.8 Hz 
   }
   return NULL;
}
*/

int list_nvidia_devices(nvmlDevice_t *device_list, unsigned int *device_count) {
   int i;
   nvmlReturn_t result;
   char name[NVML_DEVICE_NAME_BUFFER_SIZE];

   if ((result = nvmlDeviceGetCount(device_count)) != NVML_SUCCESS) {
      printf("Error: Failed to query device count: %s\n", nvmlErrorString(result));
      close_and_exit(0);
   }
#ifdef VERBOSE
   printf("Found %d device%s\n\n", *device_count, *device_count != 1 ? "s" : "");
#endif

   for (i = 0; i < *device_count; i++)
   {
       // Query for device handle to perform operations on a device
       // You can also query device handle by other features like:
       // nvmlDeviceGetHandleBySerial
       // nvmlDeviceGetHandleByPciBusId
       if ((result = nvmlDeviceGetHandleByIndex(i, &device_list[i])) != NVML_SUCCESS)
       { 
       //   printf("Failed to get handle for device %i: %s\n", i, nvmlErrorString(result));
          return result;
       }

       if ((result = nvmlDeviceGetName(device_list[i], name, NVML_DEVICE_NAME_BUFFER_SIZE)) != NVML_SUCCESS)
       { 
       //   printf("Failed to get name of device %i: %s\n", i, nvmlErrorString(result));
          return result;
       }
       nvml_energy[i] = 0.0;
   }
   return NVML_SUCCESS;
}
 
void reset_nvml() {
   int i;
   pthread_mutex_lock( &nvml_mutex );
   for(i=0; i<device_count; i++) {
      last_value_nvml[i] = nvml_energy[i];
   }
   pthread_mutex_unlock( &nvml_mutex );

}

void query_nvml_device(int device, long long delta) {
   double current_energy;
   
   pthread_mutex_lock( &nvml_mutex );
   current_energy = nvml_energy[device];
   pthread_mutex_unlock( &nvml_mutex );

   printf("nvd[%d].power = %f\n",device,(current_energy-last_value_nvml[device])/delta*1e6);

   last_value_nvml[device] = current_energy;
}

void energy_nvml_device(int device) {
   printf("nvd[%d].energy = %f\n",device,nvml_energy[device]);
}

void close_and_exit(int code) {
   nvmlReturn_t result;
   if(nvml_up) {
      if ((result = nvmlShutdown()) != NVML_SUCCESS) {
         printf("Failed to shutdown NVML: %s\n", nvmlErrorString(result));
      }
//      pthread_mutex_destroy( &nvml_mutex ); 
//      pthread_cancel(nvml_thread);
   }
   if(rapl_up)
      close_rapl_perf();
   exit(code);
}

void print_energy() {
   int i;
   for(i=0; i<device_count; i++)
      energy_nvml_device(i);
   for(i=0; i<core_count; i++)
      energy_rapl_device(i);
}

void alarm_handler (int signo)
{
   int i;
   struct timeval time;
   long long delta;

   gettimeofday(&time,NULL);

   for(i=0; i<device_count; i++)
      query_nvml_device(i,interval);
   for(i=0; i<core_count; i++)
      query_rapl_device(i,interval);

   delta = (time.tv_sec-last_time.tv_sec)*1e6+(time.tv_usec-last_time.tv_usec);
   printf("time = %lld\n",delta);
}

int perf_event_open(struct perf_event_attr *hw_event_uptr,
                    pid_t pid, int cpu, int group_fd, unsigned long flags) {

        return syscall(__NR_perf_event_open,hw_event_uptr, pid, cpu,
                        group_fd, flags);
}

int init_rapl_perf() {

   FILE *fff;
   int type;
   int core;
   int config=0;
   char filename[BUFSIZ];
   struct perf_event_attr attr;
   int i;

   fff=fopen("/sys/bus/event_source/devices/power/type","r");
   if (fff==NULL) {
      printf("No perf_event rapl support found (requires Linux 3.14)\n");
      return -1;
   }
   fscanf(fff,"%d",&type);
   fclose(fff);

   if(core_count < 0)
      core_count = sysconf(_SC_NPROCESSORS_ONLN);
   if(core_count > MAX_CORES) {
      printf("Too many processors. Increase MAX_CORES and recompile.\n");
      return -1;
   }

   for(core=0; core<core_count; core++) {
      for(i=0;i<NUM_RAPL_DOMAINS;i++) {

         fd[core][i]=-1;

         sprintf(filename,"/sys/bus/event_source/devices/power/events/energy-%s",
               rapl_domain_names[i]);

         fff=fopen(filename,"r");

         if (fff!=NULL) {
            fscanf(fff,"event=%x",&config);
#ifdef VERBOSE
            printf("Found config=%d\n",config);
#endif
            fclose(fff);
         } else {
            continue;
         }

         sprintf(filename,"/sys/bus/event_source/devices/power/events/energy-%s.scale",
               rapl_domain_names[i]);
         fff=fopen(filename,"r");

         if (fff!=NULL) {
            fscanf(fff,"%lf",&scale[i]);
#ifdef VERBOSE
            printf("Found scale=%g\n",scale[i]);
#endif
            fclose(fff);
         }

         sprintf(filename,"/sys/bus/event_source/devices/power/events/energy-%s.unit",
               rapl_domain_names[i]);
         fff=fopen(filename,"r");

         if (fff!=NULL) {
            fscanf(fff,"%s",units);
#ifdef VERBOSE
            printf("Found units=%s\n",units);
#endif
            fclose(fff);
         }

         attr.type=type;
         attr.config=config;

         fd[core][i]=perf_event_open(&attr,-1,core,-1,PERF_FLAG_FD_CLOEXEC);
         if (fd[core][i]<0) {
            if (errno==EACCES) {
               printf("Permission denied; run as root or adjust paranoid value\n");
               return -1;
            }
            else {
               printf("error opening perf events: %s\n",strerror(errno));
               return -1;
            }
         }
         last_value[core][i] = 0;
      }
   }
   rapl_up = 1;
   return 0;
}

void reset_rapl_perf() {
   int i,core;

   for(core=0; core<core_count; core++) {
      for(i=0;i<NUM_RAPL_DOMAINS;i++) {
         if (fd[core][i]!=-1) {
            read(fd[core][i],&last_value[core][i],8);
         }
      }
   }
}

void query_rapl_device(int core,long long delta) {
   int i;
   long long value;
   for(i=0;i<NUM_RAPL_DOMAINS;i++) {
      if (fd[core][i]!=-1) {
         read(fd[core][i],&value,8);
         printf("core[%d].%s.power = %lf\n",core,rapl_domain_names[i],
                (double)(value-last_value[core][i])*scale[i]/delta/1e-6);
         last_value[core][i] = value;
      }
   }
}

void energy_rapl_device(int core) {
   int i;
   long long value;
   for(i=0;i<NUM_RAPL_DOMAINS;i++) {
      if (fd[core][i]!=-1) {
         read(fd[core][i],&value,8);
         printf("core[%d].%s.energy = %lf\n",core,rapl_domain_names[i],
                (double)value*scale[i]);
      }
   }
}

void close_rapl_perf() {
   int core,i;
   for(core=0; core<core_count; core++) {
      for(i=0;i<NUM_RAPL_DOMAINS;i++) {
         if (fd[core][i]!=-1) {
            close(fd[core][i]);
         }
      }
   }
   rapl_up = 0;
}

