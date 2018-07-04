#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#if NVIDIA
#include <nvml.h>
#endif
#if XEONPHI
#include <miclib.h>
#endif

/* Global variables */

/* BEGIN CONFGURATION */
#define VERSION "1.4"
//#define VERBOSE 1
/* default interval beween measurements */
useconds_t interval = 500000;
/* Maximum number of cores in a machine */
#define MAX_CORES	256
/* END CONFGURATION */

#if NVIDIA
/* Flag to know if the NVIDIA API has been initialized */
int nvml_up = 0;
/* List and count of NVIDIA devices */
nvmlDevice_t device_list[4];
unsigned int device_count;
/* Cumulative energy for NVIDIA devices */
double nvml_energy[4];
#endif
/* Flag to know if perf RAPL events have been initialized */
int rapl_up = 0;
#if XEONPHI
/* Cumulative energy for MIC devices */
double mic_energy;
/* Flag to know if mic connection has been initialized */
int mic_up = 0;
/* Handle for the mic device */
struct mic_device *mdh;
#endif
/* The last time a measurement was made. Needed to convert energy to power in RAPL measurements */
struct timeval last_time;
/* Number of cores detected in the machine */
int core_count = 2;
int query_cores[] = {0,6};

/* Textual description of the RAPL domains */
#define NUM_RAPL_DOMAINS	4
char rapl_domain_names[NUM_RAPL_DOMAINS][30]= {
	"cores",
	"gpu",
	"pkg",
	"ram",
};
/* File descriptors to read the RAPL counters */
int fd[MAX_CORES][NUM_RAPL_DOMAINS];
/* Energy at the begining of the ROI */
long long first_value[MAX_CORES][NUM_RAPL_DOMAINS];
/* Last value read from RAPL counters to compute power from energy */
long long last_value[MAX_CORES][NUM_RAPL_DOMAINS];
/* Scale factor when reading RAPL counters */
double scale[NUM_RAPL_DOMAINS];
/* File desctiptor for output file */
FILE *out;

/* Functions */
void usage(int argc, char **argv);
void help(int argc, char **argv);

#if NVIDIA
int list_nvidia_devices(nvmlDevice_t *device_list, unsigned int *device_count);
void reset_nvml();
void query_nvml_device_power(int device, long long delta);
void query_nvml_device_energy(int device);
#endif

#if XEONPHI
int init_mic();
void reset_mic();
void query_mic_device_power(long long delta);
void query_mic_device_energy();
int close_mic();
void print_mic_error(const char *msg, const char *device_name);
#endif

void close_and_exit();
void alarm_handler (int signo);
void print_total_energy();
int init_rapl_perf();
void reset_rapl_perf();
void query_rapl_device_power(int core,long long delta);
void query_rapl_device_energy(int core);
void close_rapl_perf();

int main(int argc, char **argv)
{
   /* Generic iterators */
   int i,j;
   /* getopt support variables */
   int c = 0;
   /* Flag to indicate if only the ROI has to be measured */
   int flag_roi = 0;
   /* Flag to force output of total energy and time */
   int flag_total = 0;

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
#if NVIDIA
   /* Return value of NVIDIA API */
   nvmlReturn_t result;
#endif
   /* To convert options to integers */
   char* endp;
   long l;
   /* Set default output file */
   out = stderr;

   /* Disable getopt error reporting */
   opterr = 0;
   /* Process options with getopt */
   while ((c = getopt (argc, argv, "o::c::r::h::v::i::t::")) != -1)
      switch (c) {
         case 'o':
            if((out = fopen(optarg,"w")) == NULL) {
               fprintf(stderr,"Could not open output file %s for writing. %s\n", optarg, strerror(errno));
               close_and_exit(EXIT_FAILURE);
            }
            break;
         case 'r':
            flag_roi = 1;
            break;
         case 't':
            flag_total = 1;
            break;
/*         case 'c':
            endp = NULL;
            l = -1;
            if (!optarg || (l=strtol(optarg, &endp, 10)), (endp && *endp)) {
               fprintf(stderr,"Invalid core count %s - expecting a number.\n", optarg?optarg:"(null)");
               close_and_exit(EXIT_FAILURE);
            }
            core_count = l;
            break; */
         case 'i':
            endp = NULL;
            l = -1;
            if (!optarg || (l=strtol(optarg, &endp, 10)), (endp && *endp)) {
               fprintf(stderr,"Invalid interval %s - expecting a number of miliseconds.\n", optarg?optarg:"(null)");
               close_and_exit(EXIT_FAILURE);
            }
            if (l >= 1000) {
               fprintf(stderr,"Interval %ld too long - should be less than 1000.\n", l);
               close_and_exit(EXIT_FAILURE);
            }
            interval = l*1000;
            break;
         case 'v':
            fprintf(stderr,"sauna %s\n",VERSION);
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

#if NVIDIA
   /* Initialize NVIDIA API */
   if ((result = nvmlInit()) != NVML_SUCCESS) {
      fprintf(stderr,"Error: Failed to initialize NVML: %s\n", nvmlErrorString(result));
      close_and_exit(0);
   }
   nvml_up = 1;

   if((result = list_nvidia_devices(device_list,&device_count)) != NVML_SUCCESS) {
      fprintf(stderr,"Error: Failed to list NVIDIA devices: %s\n", nvmlErrorString(result));
      close_and_exit(0);
   }
   /* TODO Check that MAX_NVML > device_count */
#endif

   /* Initialize perf RAPL */
   if(init_rapl_perf() < 0) {
      printf ("Error: Failed to intialize perf RAPL events.\n");
      close_and_exit (0);
   }

#if XEONPHI
   /* Initialize perf RAPL */
   if(init_mic() < 0) {
      printf ("Error: Failed to intialize XeonPhi device.\n");
      close_and_exit (0);
   }
#endif

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
              fprintf(stderr,"%s%s",exec_args[i],exec_args[i+1] != NULL ? " ": "");
           fprintf(stderr,"\n");
          */
      }
      close_and_exit(1);
   }

   signal (SIGALRM, alarm_handler);
   close(pipe_stdout[1]); 
   
   /* Print headers */
   fprintf(out,"time");
   for(i=0; i<core_count; i++)
      for(j=0;j<NUM_RAPL_DOMAINS;j++) {
         if (fd[i][j]!=-1) {
            fprintf(out," core_%d_%s",query_cores[i],rapl_domain_names[j]);
         }
      }
#if NVIDIA
   for(i=0; i<device_count; i++)
     fprintf(out," nvd_%d",i);
#endif
#if XEONPHI
   fprintf(out," mic");
#endif
   fprintf(out,"\n");

   /* If the ROI analysis flag is not set, start measurements immediately */
   if(! flag_roi) {
      reset_rapl_perf();
#if NVIDIA
      reset_nvml();
#endif
      ualarm(interval, interval);
      gettimeofday(&last_time,NULL);
   }
   /* The master process reads stdin of the child process */
   while ((read = getline(&line, &len, child_stdout)) != -1) {
      /* If ROI analysis is set, and begining of ROI is detected start measurements */
      if(flag_roi && strstr(line, "++ROI")) {
         reset_rapl_perf();
#if NVIDIA
         reset_nvml();
#endif
         ualarm(interval, interval);
         gettimeofday(&last_time,NULL);
      }
      /* Stop measurements at the end of the ROI */
      else if(flag_roi && strstr(line, "--ROI")) {
         flag_roi = 1;
         ualarm(0, interval);
         if(flag_total != 0) print_total_energy();
      }
      fputs(line, stdout);
   }
   /* Stop measurements when the child dies */
   if(flag_roi < 1) {
      ualarm(0, interval);
      if(flag_total != 0) print_total_energy();
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
      printf ("Usage: %s [-rtvh] [-o<file>] [-i<ms>] <command> [<arguments>]\n", argv[0]);
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
            "   -t Causes the total time and energy to be written to the output file.\n"
            "\n"
            "   -o Sets the output file. By default it sends data to stdout. \n"
            "\n"
            "   -i Sets the sampling interval in ms. Default 500ms. Interval can not exceed 999ms. \n"
            "\n"
            "   -v Show version number.\n"
            "\n"
            "   -h Displays this message.\n"
            "\n"
            );
}

#if NVIDIA
int list_nvidia_devices(nvmlDevice_t *device_list, unsigned int *device_count) {
   int i;
   nvmlReturn_t result;
   char name[NVML_DEVICE_NAME_BUFFER_SIZE];

   if ((result = nvmlDeviceGetCount(device_count)) != NVML_SUCCESS) {
      fprintf(stderr,"Error: Failed to query device count: %s\n", nvmlErrorString(result));
      close_and_exit(0);
   }
#ifdef VERBOSE
   fprintf(stderr,"Found %d NVI device%s\n\n", *device_count, *device_count != 1 ? "s" : "");
#endif

   for (i = 0; i < *device_count; i++)
   {
       // Query for device handle to perform operations on a device
       // You can also query device handle by other features like:
       // nvmlDeviceGetHandleBySerial
       // nvmlDeviceGetHandleByPciBusId
       if ((result = nvmlDeviceGetHandleByIndex(i, &device_list[i])) != NVML_SUCCESS)
       { 
       //   fprintf(stderr,"Failed to get handle for device %i: %s\n", i, nvmlErrorString(result));
          return result;
       }

       if ((result = nvmlDeviceGetName(device_list[i], name, NVML_DEVICE_NAME_BUFFER_SIZE)) != NVML_SUCCESS)
       { 
       //   fprintf(stderr,"Failed to get name of device %i: %s\n", i, nvmlErrorString(result));
          return result;
       }
       nvml_energy[i] = 0;
   }
   return NVML_SUCCESS;
}

void reset_nvml() {
   int i;
   for (i = 0; i < device_count; i++)
      nvml_energy[i] = 0;
}
 
void query_nvml_device_power(int device, long long delta) {
   nvmlReturn_t result;
   unsigned int power_usage;
   
   if ((result = nvmlDeviceGetPowerUsage (device_list[device], &power_usage)) != NVML_SUCCESS) {
      if (result == NVML_ERROR_NOT_SUPPORTED) {
         fprintf(stderr,"\t This is not CUDA capable device\n");
      }
      else {
         close_and_exit(0);
      }
   }
   nvml_energy[device] += (double)power_usage/1000*delta*1e-6;
   fprintf(out,"%f ",(double)power_usage/1000);
}

void query_nvml_device_energy(int device) {
   fprintf(out,"%f ",nvml_energy[device]);
}

#endif
 
#if XEONPHI
void reset_mic() {
   mic_energy = 0;
}
void query_mic_device_power(long long delta) {
   struct mic_power_util_info *pinfo;
   uint32_t power_usage;

   /* Power utilization examples */
   if (mic_get_power_utilization_info(mdh, &pinfo) != E_MIC_SUCCESS) {
      print_mic_error("Failed to get power utilization information",
            mic_get_device_name(mdh));
      close_and_exit(0);
   }

   if (mic_get_inst_power_readings(pinfo, &power_usage) != E_MIC_SUCCESS) {
      print_mic_error("Failed to get instant power readings",
            mic_get_device_name(mdh));
      (void)mic_free_power_utilization_info(pinfo);
      close_and_exit(0);
   }
   mic_energy += (double)power_usage/1000000*delta*1e-6;
   fprintf(out,"%f ",(double)power_usage/1000000);

   (void)mic_free_power_utilization_info(pinfo);
}

void query_mic_device_energy() {
   fprintf(out,"%f ",mic_energy);
}

int init_mic()
{
   int ncards, card_num, card;
   struct mic_devices_list *mdl;
   int ret;
   uint32_t device_type;

   ret = mic_get_devices(&mdl);
   if (ret == E_MIC_DRIVER_NOT_LOADED) {
      fprintf(stderr, "Error: The driver is not loaded! ");
      fprintf(stderr, "Load the driver before using this tool.\n");
      return 1;
   } else if (ret == E_MIC_ACCESS) {
      fprintf(stderr, "Error: Access is denied to the driver! ");
      fprintf(stderr, "Do you have permissions to access the driver?\n");
      return 1;
   } else if (ret != E_MIC_SUCCESS) {
      fprintf(stderr, "Failed to get cards list: %s: %s\n",
            mic_get_error_string(), strerror(errno));
      return 1;
   }

   if (mic_get_ndevices(mdl, &ncards) != E_MIC_SUCCESS) {
      print_mic_error("Failed to get number of cards", NULL);
      (void)mic_free_devices(mdl);
      return 2;
   }

   if (ncards == 0) {
      print_mic_error("No MIC card found", NULL);
      (void)mic_free_devices(mdl);
      return 3;
   }

   /* Get card at index 0 */
   card_num = 0;
   if (mic_get_device_at_index(mdl, card_num, &card) != E_MIC_SUCCESS) {
      fprintf(stderr, "Error: Failed to get card at index %d: %s: %s\n",
            card_num, mic_get_error_string(), strerror(errno));
      mic_free_devices(mdl);
      return 4;
   }

   (void)mic_free_devices(mdl);

   if (mic_open_device(&mdh, card) != E_MIC_SUCCESS) {
      fprintf(stderr, "Error: Failed to open card %d: %s: %s\n",
            card_num, mic_get_error_string(), strerror(errno));
      return 5;
   }

   if (mic_get_device_type(mdh, &device_type) != E_MIC_SUCCESS) {
      print_mic_error("Failed to get device type", mic_get_device_name(mdh));
      (void)mic_close_device(mdh);
      return 6;
   }

   if (device_type != KNC_ID) {
      fprintf(stderr, "Error: Unknown device Type: %u\n", device_type);
      (void)mic_close_device(mdh);
      return 7;
   }
   //printf("    Found KNC device '%s'\n", mic_get_device_name(mdh));
   mic_energy = 0;
   mic_up = 0;
   return 0;
}

int close_mic()
{

    (void)mic_close_device(mdh);
    return 0;
}

void print_mic_error(const char *msg, const char *device_name)
{
    const char *mic_err_str = mic_get_error_string();

    fprintf(stderr, "Error");
    if (device_name != NULL)
        fprintf(stderr, ": %s", device_name);
    fprintf(stderr, ": %s", msg);
    if (strcmp("No error registered", mic_err_str) != 0)
        fprintf(stderr, ": %s", mic_err_str);
    if (errno == 0)
        fprintf(stderr, "\n");
    else
        fprintf(stderr, ": %s\n", strerror(errno));
}
#endif

void close_and_exit(int code) {
#if NVIDIA
   nvmlReturn_t result;
   if(nvml_up) {
      if ((result = nvmlShutdown()) != NVML_SUCCESS) {
         fprintf(stderr,"Failed to shutdown NVML: %s\n", nvmlErrorString(result));
      }
   }
#endif
   if(rapl_up)
      close_rapl_perf();
#if XEONPHI
   if(mic_up)
      close_mic();
#endif
   exit(code);
}

void alarm_handler (int signo)
{
   int i;
   struct timeval time;
   double now;
   static double before = 0;

   gettimeofday(&time,NULL);

   now = (time.tv_sec-last_time.tv_sec)+(time.tv_usec-last_time.tv_usec)*1e-6;
   if(before == 0) before = now-interval;
   fprintf(out,"%f ",(double) now);
   for(i=0; i<core_count; i++)
      query_rapl_device_power(i,(now-before)*1e6);
#if NVIDIA
   for(i=0; i<device_count; i++)
      query_nvml_device_power(i,interval);
#endif
#if XEONPHI
   query_mic_device_power(interval);
#endif
   fprintf(out,"\n");
   before = now;
}

void print_total_energy() {
   int i;
   struct timeval time;

   fprintf(out,"Totals: ");
   gettimeofday(&time,NULL);

   fprintf(out,"%f ",(double) (time.tv_sec-last_time.tv_sec)+(time.tv_usec-last_time.tv_usec)*1e-6);
   for(i=0; i<core_count; i++)
      query_rapl_device_energy(i);
#if NVIDIA
   for(i=0; i<device_count; i++)
      query_nvml_device_energy(i);
#endif
#if XEONPHI
   query_mic_device_energy();
#endif
   fprintf(out,"\n");
}

int perf_event_open(struct perf_event_attr *hw_event_uptr,
                    pid_t pid, int cpu, int group_fd, unsigned long flags) {

        return syscall(__NR_perf_event_open,hw_event_uptr, pid, cpu,
                        group_fd, flags);
}

int init_rapl_perf() {

   FILE *fff;
   int type;
   int config=0;
   char filename[BUFSIZ];
   char units[BUFSIZ];
   struct perf_event_attr attr;
   int i,j;

   fff=fopen("/sys/bus/event_source/devices/power/type","r");
   if (fff==NULL) {
      fprintf(stderr,"No perf_event rapl support found (requires Linux 3.14)\n");
      return -1;
   }
   fscanf(fff,"%d",&type);
   fclose(fff);

/*   core_count = sysconf(_SC_NPROCESSORS_ONLN);
   if(core_count > MAX_CORES) {
      fprintf(stderr,"Too many processors. Increase MAX_CORES and recompile.\n");
      return -1;
   } */

   for(i=0; i<core_count; i++) {
      for(j=0;j<NUM_RAPL_DOMAINS;j++) {

#ifdef VERBOSE
         fprintf(stderr,"Trying core %d with RAPL domain %s (%d)\n",query_cores[i],rapl_domain_names[j],j);
#endif
         fd[i][j]=-1;

         sprintf(filename,"/sys/bus/event_source/devices/power/events/energy-%s",
               rapl_domain_names[j]);

         fff=fopen(filename,"r");

         if (fff!=NULL) {
            fscanf(fff,"event=%x",&config);
#ifdef VERBOSE
            fprintf(stderr,"Found config=%d\n",config);
#endif
            fclose(fff);
         } else {
            continue;
         }

         sprintf(filename,"/sys/bus/event_source/devices/power/events/energy-%s.scale",
               rapl_domain_names[j]);
         fff=fopen(filename,"r");

         if (fff!=NULL) {
            fscanf(fff,"%lf",&scale[j]);
#ifdef VERBOSE
            fprintf(stderr,"Found scale=%g\n",scale[j]);
#endif
            fclose(fff);
         }

         sprintf(filename,"/sys/bus/event_source/devices/power/events/energy-%s.unit",
               rapl_domain_names[j]);
         fff=fopen(filename,"r");

         if (fff!=NULL) {
            fscanf(fff,"%s",units);
#ifdef VERBOSE
            fprintf(stderr,"Found units=%s\n",units);
#endif
            fclose(fff);
         }

         attr.type=type;
         attr.config=config;

         fd[i][j]=perf_event_open(&attr,-1,query_cores[i],-1,PERF_FLAG_FD_CLOEXEC);
         if (fd[i][j]<0) {
            if (errno==EACCES) {
               fprintf(stderr,"Permission denied; run as root or adjust paranoid value\n");
               return -1;
            }
            else {
               fprintf(stderr,"error opening perf events: %s\n",strerror(errno));
               return -1;
            }
         }
         first_value[i][j] = 0;
         last_value[i][j] = 0;
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
            first_value[core][i] = last_value[core][i];
         }
      }
   }
}

void query_rapl_device_power(int core,long long delta) {
   int i;
   long long value;
   for(i=0;i<NUM_RAPL_DOMAINS;i++) {
      if (fd[core][i]!=-1) {
         read(fd[core][i],&value,8);
         fprintf(out,"%lf ",(double)(value-last_value[core][i])*scale[i]/delta/1e-6);
         last_value[core][i] = value;
      }
   }
}

void query_rapl_device_energy(int core) {
   int i;
   long long value;
   for(i=0;i<NUM_RAPL_DOMAINS;i++) {
      if (fd[core][i]!=-1) {
         read(fd[core][i],&value,8);
         fprintf(out,"%lf ",(double)(value-first_value[core][i])*scale[i]);
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

