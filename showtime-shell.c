#define _GNU_SOURCE

#include <linux/loop.h>
#include <linux/reboot.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <pthread.h>

#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <syslog.h>
#include <time.h>

#define STOS_BASE "/stos"

#define PERSISTENTPATH STOS_BASE"/persistent"
#define CACHEPATH      STOS_BASE"/cache"
#define MNTPATH        STOS_BASE"/mnt"
#define SHOWTIMEMOUNTPATH MNTPATH"/showtime"

#define DEFAULT_PERSISTENT_PART "/dev/mmcblk0p2"
#define DEFAULT_CACHE_PART      "/dev/mmcblk0p3"
#define DEFAULT_FLASH_DEV       "/dev/mmcblk0"
#define PERSISTENT_SIZE (256 * 1024 * 1024)

#define SHOWTIME_PKG_PATH PERSISTENTPATH"/packages/showtime.sqfs"
#define SHOWTIME_DEFAULT_PATH "/boot/showtime.sqfs"

const char *persistent_part;
const char *cache_part;
const char *flash_dev;

static int got_sigint;
static int reboot_on_failure;

/**
 *
 */
typedef struct loopmount {
  char devpath[128];
  int loopfd;
} loopmount_t;


/**
 *
 */
static void
trace(int level, const char *fmt, ...)
{
  va_list ap, copy;
  va_start(ap, fmt);
  va_copy(copy, ap);

  vsyslog(level, fmt, ap);
  va_end(ap);

  if(isatty(2)) {
    vfprintf(stderr, fmt, copy);
    fprintf(stderr, "\n");
  }

  va_end(copy);
}


/**
 *
 */
static void
doumount(const char *path)
{
  while(!umount2(path, MNT_DETACH)) {
    trace(LOG_DEBUG, "Unmounted %s", path);
  }

}



/**
 *
 */
static int
loopmount(const char *image, const char *mountpoint,
	  const char *fstype, int mountflags, int ro,
	  loopmount_t *lm)
{
  struct stat st;
  struct loop_info64 li;
  int mode = ro ? O_RDONLY : O_RDWR;

  if(ro)
    mountflags |= MS_RDONLY;

  trace(LOG_INFO, "loopmount: opening image %s as %s",
	 image, ro ? "read-only" : "read-write");

  int fd = open(image, mode | O_CLOEXEC);
  if(fd < 0) {
    trace(LOG_ERR, "loopmount: failed to open image %s -- %s",
	   image, strerror(errno));
    return -1;
  }
  int i;
  for(i = 0; i < 256; i++) {
    snprintf(lm->devpath, sizeof(lm->devpath), "/dev/loop%d", i);
    printf("Testing %s\n", lm->devpath);


    if(stat(lm->devpath, &st)) {
      // Nothing there, try to create node
      if(mknod(lm->devpath, S_IFBLK|0644, makedev(7, i))) {
	trace(LOG_ERR, "loopmount: failed to create %s -- %s",
	       lm->devpath, strerror(errno));
	continue;
      }
    } else {
      // Something there
      if(!S_ISBLK(st.st_mode)) {
	trace(LOG_ERR, "loopmount: %s is not a block device",
	       lm->devpath);
	continue; // Not a block device, scary
      }
    }

    lm->loopfd = open(lm->devpath, mode | O_CLOEXEC);
    if(lm->loopfd == -1) {
      trace(LOG_ERR, "loopmount: Unable to open %s -- %s",
	     lm->devpath, strerror(errno));
      continue;
    }

    int rc = ioctl(lm->loopfd, LOOP_GET_STATUS64, &li);
    if(rc && errno == ENXIO) {
      // Device is free, use it 
      memset(&li, 0, sizeof(li));
      snprintf((char *)li.lo_file_name, sizeof(li.lo_file_name), "%s", image);
      if(ioctl(lm->loopfd, LOOP_SET_FD, fd)) {
	trace(LOG_ERR, "loopmount: Failed to SET_FD on %s -- %s",
	       lm->devpath, strerror(errno));
	close(lm->loopfd);
	continue;
      }
      if(ioctl(lm->loopfd, LOOP_SET_STATUS64, &li)) {
	trace(LOG_ERR, "loopmount: Failed to SET_STATUS64 on %s -- %s",
	       lm->devpath, strerror(errno));
	ioctl(lm->loopfd, LOOP_CLR_FD, 0);
	close(lm->loopfd);
	continue;
      }

      close(fd);

      doumount(mountpoint);

      if(mount(lm->devpath, mountpoint, fstype, mountflags, "")) {
	trace(LOG_ERR, "loopmount: Unable to mount loop device %s on %s -- %s",
	       lm->devpath, mountpoint, strerror(errno));
	ioctl(lm->loopfd, LOOP_CLR_FD, 0);
	close(lm->loopfd);
	return -1;
      }
      return 0;
    }
    close(lm->loopfd);
  }
  return -1;
}


static void
loopunmount(loopmount_t *lm, const char *mountpath)
{
  trace(LOG_INFO, "Unmounting %s", mountpath);
  doumount(mountpath);
  ioctl(lm->loopfd, LOOP_CLR_FD, 0);
  close(lm->loopfd);
  unlink(lm->devpath);
}


#define RUN_BUNDLE_MOUNT_PROBLEMS    -1
#define RUN_BUNDLE_RESOURCE_FAILURE  -2
#define RUN_BUNDLE_COMMAND_NOT_FOUND -3
#define RUN_BUNDLE_COMMAND_CRASH     -4

extern char **environ;

/**
 *
 */
static int
run_squashfs_bundle(const char *path, const char *mountpath,
		    const char *binpath, const char *argv[],
		    int pipefd[2])
{
  loopmount_t lm;
  char cmd[1024];

  mkdir(mountpath, 0777);

  if(loopmount(path, mountpath, "squashfs", MS_NOSUID | MS_NODEV, 1, &lm))
    return RUN_BUNDLE_MOUNT_PROBLEMS;


  snprintf(cmd, sizeof(cmd), "%s/%s", mountpath, binpath);

  trace(LOG_INFO, "bundle: Starting '%s' pipe:[%d,%d]", cmd,
	pipefd[0], pipefd[1]);


  pid_t p = fork();
  if(p == -1) {
    trace(LOG_INFO, "bundle: Unable to start '%s' -- %s", cmd,
	   strerror(errno));
    return RUN_BUNDLE_RESOURCE_FAILURE;
  }


  if(p == 0) {
    close(pipefd[0]);
    execve(cmd, (char **)argv, environ);
    exit(1);
  }
  int ret;

  got_sigint = 0;

  pid_t pp = waitpid(p, &ret, 0);

  loopunmount(&lm, mountpath);

  if(got_sigint) {
    trace(LOG_INFO, "bundle: Killing %s because ^C", cmd);
    kill(p, SIGINT);
    exit(0);
  }

  if(pp == -1) {
    trace(LOG_INFO, "bundle: Wait error %s' -- %s", cmd,
	   strerror(errno));
    return RUN_BUNDLE_RESOURCE_FAILURE;
  }


  if(WIFSIGNALED(ret)) {

    if(WTERMSIG(ret) == SIGINT || WTERMSIG(ret) == SIGQUIT)
      exit(0); // ^C - Happens only when dev:ing (I think)

    trace(LOG_INFO, "bundle: '%s' exited due to signal %d",
	  cmd, WTERMSIG(ret));

    return RUN_BUNDLE_COMMAND_CRASH;
  }

  if(WEXITSTATUS(ret) == 127) {
    trace(LOG_INFO, "bundle: Unable to start '%s' -- command not found",
	  cmd);
    return RUN_BUNDLE_COMMAND_NOT_FOUND;
  }

  trace(LOG_INFO, "bundle: '%s' exited with code %d",
	cmd, WEXITSTATUS(ret));

  return WEXITSTATUS(ret);
}



static int sshd_running;

/**
 *
 */
static void
start_sshd(void)
{
  struct stat st;
  if(sshd_running)
    return;

  mkdir(PERSISTENTPATH"/etc",0700);
  mkdir(PERSISTENTPATH"/etc/dropbear",0700);

  if(access(PERSISTENTPATH"/etc/dropbear/dropbear_rsa_host_key", R_OK) ||
     stat(PERSISTENTPATH"/etc/dropbear/dropbear_rsa_host_key", &st) ||
     st.st_size < 10)
    system("/usr/bin/dropbearkey -t rsa -f "PERSISTENTPATH"/etc/dropbear/dropbear_rsa_host_key");

  const char *cmd = "/usr/sbin/dropbear";

  trace(LOG_INFO, "Starting %s", cmd);


  const char *args[] = {
    cmd,
    "-r",
    PERSISTENTPATH"/etc/dropbear/dropbear_rsa_host_key",
    "-F",
    NULL
  };

  pid_t p = fork();
  if(p == -1) {
    trace(LOG_INFO, "Unable to start '%s' -- %s", cmd,
	  strerror(errno));
    return;
  }

  if(p == 0) {
    int i;
    for(i = 3; i < 1024; i++)
      close(i);

    execve(cmd, (char **)args, environ);
    trace(LOG_INFO, "Unable to execve %s -- %s", cmd, strerror(errno));

    exit(1);
  }

  sshd_running = p;
}


/**
 *
 */
static void
stop_sshd(void)
{
  if(!sshd_running)
    return;

  kill(sshd_running, SIGTERM);

  int ret;
  waitpid(sshd_running, &ret, 0);

  trace(LOG_INFO, "sshd exited with %d", ret);
  sshd_running = 0;
}


/**
 *
 */
static void *
showtime_com_thread(void *aux)
{
  int fd = *(int *)aux;
  while(1) {
    char cmd;
    if(read(fd, &cmd, 1) != 1)
      break;

    switch(cmd) {
    case 1:
      start_sshd();
      break;
    case 2:
      stop_sshd();
      break;
    }

  }

  return NULL;
}



/**
 *
 */
static int
start_showtime_from_bundle(const char *bundle)
{
  int pipefd[2];
  pthread_t tid;
  char fdtxt[10];

  if(pipe(pipefd))
    return RUN_BUNDLE_RESOURCE_FAILURE;

  pthread_create(&tid, NULL, showtime_com_thread, &pipefd[0]);

  snprintf(fdtxt, sizeof(fdtxt), "%d", pipefd[1]);

  const char *args[] = {
    SHOWTIMEMOUNTPATH"/bin/showtime",
    "-d",
    "--cache",
    CACHEPATH"/showtime",
    "--persistent",
    PERSISTENTPATH"/showtime",
    "--upgrade-path",
    SHOWTIME_PKG_PATH,
    "--showtime-shell-fd",
    fdtxt,
    NULL
  };

  int r = run_squashfs_bundle(bundle, SHOWTIMEMOUNTPATH, "bin/showtime", args,
			      pipefd);


  close(pipefd[0]);
  close(pipefd[1]);
  trace(LOG_INFO, "Waiting for thread");
  pthread_join(tid, NULL);
  trace(LOG_INFO, "Thread joined");
  return r;
}


// We want to align to 16384 sector boundary for better
// erase performance on SD cards
#define SD_ALIGN(x) (((x) + 16383) & ~16383)


/**
 *
 */
static int
runcmd(const char *cmdline)
{
  int ret = system(cmdline);

  if(ret == -1) {
    trace(LOG_INFO, "Unable to start '%s' -- %s", cmdline,
	   strerror(errno));
    return -1;
  }
  
 if(WEXITSTATUS(ret) == 127) {
    trace(LOG_INFO, "Unable to start '%s' -- command not found",
	  cmdline);
    return -1;
  }

  trace(LOG_INFO, "'%s' exited with code %d", cmdline, WEXITSTATUS(ret));

  return WEXITSTATUS(ret);
}



/**
 *
 */
static int
create_partition(int start, int end, const char *type, const char *fstype)
{
  char cmdline[512];

  trace(LOG_NOTICE, "Creating %s partition [%s] start:%d end:%d (%d KiB) on %s",
	type, fstype, start, end, (end - start) / 2, flash_dev);

  snprintf(cmdline, sizeof(cmdline),
	   "parted -m %s unit s mkpart %s %s %d %d",
	   flash_dev, type, fstype, start, end);

  return runcmd(cmdline);
}


/**
 *
 */
static int
format_partition(int partid)
{
  char cmdline[512];
  const char *label;
  const char *part;
  switch(partid) {
  case 2:
    label = "persistent";
    part = persistent_part;
    break;
  case 3:
    label = "cache";
    part = cache_part;
    break;
  default:
    trace(LOG_ERR, "Don't know how to format partition %d", partid);
    return -1;
  }

  trace(LOG_NOTICE, "Formatting partition %d [%s] device: %s", partid, label, part);

  snprintf(cmdline, sizeof(cmdline),
	   "mkfs.ext4 -L %s -E stride=2,stripe-width=1024 -b 4096 %s",
	   label, part);

  return runcmd(cmdline);
}


/**
 *
 */
static int
setup_partitions(void)
{
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "parted -m %s unit s print free", flash_dev);

  FILE *fp = popen(cmd, "r");
  char *line = NULL;
  size_t len = 0;
  ssize_t read;

  int partfound[5] = {};

  int free_start = 0;
  int free_end = 0;
  int free_size = 0;

  while((read = getline(&line, &len, fp)) != -1) {
    int part, start, end, size;
    char type[512];
    if((sscanf(line, "%d:%ds:%ds:%ds:%[^;];",
	       &part, &start, &end, &size, type)) != 5)
      continue;

    if(strcmp(type, "free") && part < 5)
      partfound[part] = 1;

    if(!strcmp(type, "free")) {
      if(size > free_size) {
	free_start = start;
	free_end   = end;
	free_size  = size;
      }
    }
  }

  free(line);
  fclose(fp);


  int i;
  for(i = 1; i <= 4; i++)
    trace(LOG_INFO,
	  "Partition %d %s", i, partfound[i] ? "available" : "not found");

  trace(LOG_INFO,
	"Biggest free space available: %d sectors at %d - %d",
	free_size, free_start, free_end);

  if(!partfound[2]) {
    // Create persistent partition

    trace(LOG_INFO, "Need to create partition for persistent data");

    int start = SD_ALIGN(free_start);
    int end   = start + (PERSISTENT_SIZE / 512) - 1;
    if(create_partition(start, end, "primary", "ext4")) {
      trace(LOG_ERR, "Failed to create partition for persistent data");
      return -1;
    }
    format_partition(2);
    free_start = end + 1;
  }

  if(!partfound[3]) {
    // Create persistent partition

    trace(LOG_INFO, "Need to create partition for cached data");

    int start = SD_ALIGN(free_start);
    int end   = free_end;
    if(create_partition(start, end, "primary", "ext4")) {
      trace(LOG_ERR, "Failed to create partition for cached data");
      return -1;
    }
    format_partition(3);
  }
  return 0;
}

/**
 *
 */
static int
domount(const char *dev, const char *path)
{
  char fsckcmd[128];
  doumount(path);

  snprintf(fsckcmd, sizeof(fsckcmd), "/usr/sbin/fsck.ext4 -f -p %s", dev);
  system(fsckcmd);

  mkdir(path, 0777);
  if(mount(dev, path, "ext4",
	   MS_NOATIME | MS_NOSUID | MS_NODEV | MS_NOEXEC,
	   "")) {
    trace(LOG_ERR, "Unable to mount %s on %s -- %s",
	  dev, path, strerror(errno));
    return -1;
  }
  return 0;
}


/**
 *
 */
static void
restart(void)
{
  if(!reboot_on_failure) {
    trace(LOG_ALERT, "Exiting due to fatal error");
    exit(2);
  }
  trace(LOG_ALERT, "Will restart in %d seconds", reboot_on_failure);
  sleep(reboot_on_failure);
  sync();
  reboot(LINUX_REBOOT_CMD_RESTART);
  while(1) {
    sleep(1);
  }
}


/**
 *
 */
static void
panic(const char *str)
{
  trace(LOG_ALERT, "%s", str);
  restart();
}


static void
dosigint(int x)
{
  got_sigint = 1;
}

static void
stop_by_pidfile(const char *pidfile)
{
  char buf[32] = {0};

  FILE *fp = fopen(pidfile, "r");
  if(fp == NULL)
    return;

  if(fgets(buf, 31, fp) != NULL) {
    int pid = atoi(buf);
    kill(pid, SIGTERM);
  }
  fclose(fp);
  return;
}


/**
 *
 */
int
main(int argc, char **argv)
{
  int c;
  int prep = 0;
  while((c = getopt(argc, argv, "r:p")) != -1) {
    switch(c) {
    case 'r':
      reboot_on_failure = atoi(optarg);
      break;
    case 'p':
      prep = 1;
      break;
    }
  }

  openlog("showtimeshell", LOG_PID, LOG_USER);

  persistent_part = getenv("STOS_persistent") ?: DEFAULT_PERSISTENT_PART;
  cache_part      = getenv("STOS_cache")      ?: DEFAULT_CACHE_PART;
  flash_dev       = getenv("STOS_flash")      ?: DEFAULT_FLASH_DEV;

  trace(LOG_INFO, "     Flash device on %s", flash_dev);
  trace(LOG_INFO, "Persistent device on %s", persistent_part);
  trace(LOG_INFO, "     Cache device on %s", cache_part);

  if(prep) {

    mkdir("/tmp/stos", 0777);

    mkdir(MNTPATH, 0777);

    trace(LOG_INFO, "Checking SD card disk layout");

    setup_partitions();
    trace(LOG_INFO, "Done checking SD card disk layout");

    if(domount(persistent_part, PERSISTENTPATH)) {
      format_partition(2);
      if(domount(persistent_part, PERSISTENTPATH))
	panic("Unable to mount partition for persistent data after formatting");
    }

    if(domount(cache_part, CACHEPATH)) {
      format_partition(3);
      if(domount(cache_part, CACHEPATH))
	panic("Unable to mount partition for cache data after formatting");
    }

    exit(0);
  }

  if(!access("/boot/noshowtime", R_OK)) {
    start_sshd();
    exit(0);
  }

  signal(SIGINT, dosigint);

  int shortrun = 0;
  int ever_run_ok = 0;
  while(1) {
    int exitcode;
    int from_downloaded = 0;
    mkdir(PERSISTENTPATH"/packages", 0777);

    time_t starttime = time(NULL);

    if(!access(SHOWTIME_PKG_PATH, R_OK)) {
      exitcode = start_showtime_from_bundle(SHOWTIME_PKG_PATH);
      from_downloaded = 1;
    } else {
      exitcode = start_showtime_from_bundle(SHOWTIME_DEFAULT_PATH);
    }

    time_t stoptime = time(NULL);

    if(stoptime - starttime < 5) {
      shortrun++;
    } else {
      shortrun = 0;
      ever_run_ok = 1;
    }

    if(shortrun == 3 && ever_run_ok) {
      restart();
    }

    if(shortrun == 5) {
      trace(LOG_ERR, "Showtime keeps respawning quickly, clearing cache");
      doumount(CACHEPATH);
      format_partition(3);
      if(domount(cache_part, CACHEPATH))
	panic("Unable to mount partition for cache data after formatting");
      continue;
    }

    if(shortrun == 6) {

      trace(LOG_ERR, "Showtime keeps respawning quickly, clearing persistent partition");
      doumount(PERSISTENTPATH);
      format_partition(2);
      if(domount(persistent_part, PERSISTENTPATH))
	panic("Unable to mount partition for persistent data after formatting");
      continue;
    }

    if(shortrun == 7) {
      panic("Guru meditation. Showtime can no longer start.");
    }

    if(exitcode == RUN_BUNDLE_RESOURCE_FAILURE) {
      restart();
    }

    if(exitcode == RUN_BUNDLE_COMMAND_CRASH) {
      sleep(1);
      continue;
    }

    if(exitcode < 0 && from_downloaded) {
      // something is bad with a downloaded bundle, remove it
      trace(LOG_ERR, "Start problems, removing %s", SHOWTIME_PKG_PATH);
      unlink(SHOWTIME_PKG_PATH);
      sync();
      continue;
    }

    if(exitcode < 0) {
      restart();
    }

    if(exitcode == 14) {
      // Exit to shell
      exit(0);
    }

    if(exitcode == 15) {
      // System reboot

      stop_by_pidfile("/var/run/connmand.pid");

      doumount(CACHEPATH);
      doumount(PERSISTENTPATH);

      system("/usr/bin/killall udevd");

      doumount("/lib/modules");
      doumount("/lib/firmware");
      doumount("/boot");
      sync();
      reboot(LINUX_REBOOT_CMD_RESTART);
    }

    if(shortrun)
      sleep(1);
  }

  return 0;
}
