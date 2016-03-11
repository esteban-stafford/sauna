#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <nvml.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

/*
   Build with:

   gcc -o sauna sauna.c  -I /usr/local/gdk/usr/include/nvidia/gdk -L /usr/local/gdk/usr/src/gdk/nvml/lib -lnvidia-ml

   TODO

   1 second and above intervals don't work.

*/

/* Global variables */

/* BEGIN CONFGURATION */
#define VERSION "1.0"
//#define VERBOSE 1
/* default interval beween measurements */
useconds_t interval = 500000;
/* Number of cores in the machine */
int core_count = 1;
/* END CONFGURATION */

/* Flag to know if the NVIDIA API has been initialized */
int nvml_up = 0;
/* List and count of NVIDIA devices */
nvmlDevice_t device_list[4];
unsigned int device_count;
/* Energy accumulator for NVIDIA devices */
double nvml_energy[4];
/* Flag to know if perf RAPL events have been initialized */
int rapl_up = 0;
/* The last time a measurement was made. Needed to convert energy to power in RAPL measurements */
struct timeval last_time;

/* Functions */
void usage(int argc, char **argv);
void help(int argc, char **argv);
int list_nvidia_devices(nvmlDevice_t *device_list, unsigned int *device_count);
void query_nvml_device(int device, long long delta);
void close_and_exit();
void alarm_handler (int signo);
int init_rapl_perf();
void reset_rapl_perf();
void query_rapl_device(int core,long long delta);
void close_rapl_perf();
void print_energy();
void energy_nvml_device(int device);
void energy_rapl_device(int core);

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
   while ((c = getopt (argc, argv, "r::h::v::i::")) != -1)
      switch (c) {
         case 'r':
            flag_roi = 1;
            break;
         case 'i':
            endp = NULL;
            l = -1;
            if (!optarg || (l=strtol(optarg, &endp, 10)), (endp && *endp)) {
               printf("Invalid interval %s - expecting a number of miliseconds\n", optarg?optarg:"(null)");
               close_and_exit(EXIT_FAILURE);
            };
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

   /* Prepare a NULL terminated array of trings to pass to execv, after the fork. */
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
      reset_rapl_perf();
      ualarm(interval, interval);
      gettimeofday(&last_time,NULL);
   }
   /* The master process reads stdin of the child process */
   while ((read = getline(&line, &len, child_stdout)) != -1) {
      /* If ROI analysis is set, and begining of ROI is detected start measurements */
      if(flag_roi && strstr(line, "++ROI")) {
         reset_rapl_perf();
         ualarm(interval, interval);
         gettimeofday(&last_time,NULL);
      }
      /* Stop measurements at the end of the ROI */
      else if(flag_roi && strstr(line, "--ROI")) {
         flag_roi = 2;
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
 
void query_nvml_device(int device, long long delta) {
   nvmlReturn_t result;
   int power_usage;
   if ((result = nvmlDeviceGetPowerUsage (device_list[device], &power_usage)) != NVML_SUCCESS) {
      if (result == NVML_ERROR_NOT_SUPPORTED) {
         printf("\t This is not CUDA capable device\n");
      }
      else {
         close_and_exit(0);
      }
   }
   printf("nvd[%d].power = %f\n",device,(double)power_usage/1000);
   nvml_energy[device] += (double)power_usage*interval*1e-9;
}

void energy_nvml_device(int device) {
   printf("nvd[%d].energy = %f\n",device,nvml_energy[device]);
}

void close_and_exit(int code) {
   nvmlReturn_t result;
   if(nvml_up) {
      if (nvmlShutdown() != NVML_SUCCESS) {
         printf("Failed to shutdown NVML: %s\n", nvmlErrorString(result));
      }
   }
   if(rapl_up)
      close_rapl_perf();
   exit(code);
}

void print_energy() {
   for(int i=0; i<device_count; i++)
      energy_nvml_device(i);
   for(int i=0; i<core_count; i++)
      energy_rapl_device(i);
}

void alarm_handler (int signo)
{
   int i;
   FILE *fd;
   size_t len;
   char buffer[256];
   struct timeval time;
   long long delta;

   gettimeofday(&time,NULL);

   for(int i=0; i<device_count; i++)
      query_nvml_device(i,interval);
   for(int i=0; i<core_count; i++)
      query_rapl_device(i,interval);

   delta = (time.tv_sec-last_time.tv_sec)*1e6+(time.tv_usec-last_time.tv_usec);
   printf("time = %lld\n",delta);
}

int perf_event_open(struct perf_event_attr *hw_event_uptr,
                    pid_t pid, int cpu, int group_fd, unsigned long flags) {

        return syscall(__NR_perf_event_open,hw_event_uptr, pid, cpu,
                        group_fd, flags);
}

#define NUM_RAPL_DOMAINS	4
#define NUM_CORES	4

char rapl_domain_names[NUM_RAPL_DOMAINS][30]= {
	"cores",
	"gpu",
	"pkg",
	"ram",
};

char units[BUFSIZ];
int fd[NUM_CORES][NUM_RAPL_DOMAINS];
long long last_value[NUM_CORES][NUM_RAPL_DOMAINS];
double scale[NUM_RAPL_DOMAINS];

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

         fd[core][i]=perf_event_open(&attr,-1,core,-1,0);
         if (fd[core][i]<0) {
            if (errno==EACCES) {
               printf("Permission denied; run as root or adjust paranoid value\n");
               return -1;
            }
            else {
               printf("error opening: %s\n",strerror(errno));
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
   long long value;

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

