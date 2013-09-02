#define _GNU_SOURCE

#include <linux/loop.h>
#include <linux/reboot.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/reboot.h>
#include <sys/wait.h>

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
#define SHOWTIMEMOUNTPATH MNTPATH"/mnt/showtime"

#define PERSISTENTDEV "/dev/mmcblk0p3"
#define CACHEDEV      "/dev/mmcblk0p4"

#define PERSISTENT_SIZE (256 * 1024 * 1024)

#define SHOWTIME_PKG_PATH PERSISTENTPATH"/packages/showtime.sqfs"
#define SHOWTIME_DEFAULT_PATH "/boot/showtime.sqfs"

static int got_sigint;

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

  int fd = open(image, mode);
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

    lm->loopfd = open(lm->devpath, mode);
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
#define RUN_BUNDLE_FORK_FAILURE      -2
#define RUN_BUNDLE_COMMAND_NOT_FOUND -3
#define RUN_BUNDLE_COMMAND_CRASH     -4

extern char **environ;

/**
 *
 */
static int
run_squashfs_bundle(const char *path, const char *mountpath,
		    const char *binpath, const char *argv[])
{
  loopmount_t lm;
  char cmd[1024];

  mkdir(mountpath, 0777);

  if(loopmount(path, mountpath, "squashfs", MS_NOSUID | MS_NODEV, 1, &lm))
    return RUN_BUNDLE_MOUNT_PROBLEMS;


  snprintf(cmd, sizeof(cmd), "%s/%s", mountpath, binpath);

  trace(LOG_INFO, "bundle: Starting %s", cmd);


  pid_t p = fork();
  if(p == -1) {
    trace(LOG_INFO, "bundle: Unable to start '%s' -- %s", cmd,
	   strerror(errno));
    return RUN_BUNDLE_FORK_FAILURE;
  }


  if(p == 0) {
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
    return RUN_BUNDLE_FORK_FAILURE;
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


/**
 *
 */
static int
start_showtime_from_bundle(const char *bundle)
{
  const char *args[] = {
    SHOWTIMEMOUNTPATH"/bin/showtime",
    "-d",
    "--cache",
    CACHEPATH"/showtime",
    "--persistent",
    PERSISTENTPATH"/showtime",
    "--upgrade-path",
    SHOWTIME_PKG_PATH,
    NULL
  };

  return run_squashfs_bundle(bundle, SHOWTIMEMOUNTPATH, "bin/showtime", args);
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

  trace(LOG_NOTICE, "Creating %s partition [%s] start:%d end:%d (%d KiB)",
	type, fstype, start, end, (end - start) / 2);

  snprintf(cmdline, sizeof(cmdline),
	   "parted -m /dev/mmcblk0 unit s mkpart %s %s %d %d",
	   type, fstype, start, end);

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
  switch(partid) {
  case 3:
    label = "persistent";
    break;
  case 4:
    label = "cache";
    break;
  default:
    trace(LOG_ERR, "Don't know how to format partition %d", partid);
    return -1;
  }

  trace(LOG_NOTICE, "Formatting partition %d [%s]", partid, label);

  snprintf(cmdline, sizeof(cmdline), 
	   "mkfs.ext4 -O ^has_journal -L %s -E stride=2,stripe-width=1024 -b 4096 /dev/mmcblk0p%d", 
	   label, partid);

  return runcmd(cmdline);
}


/**
 *
 */
static int
setup_partitions(void)
{
  FILE *fp = popen("parted -m /dev/mmcblk0 unit s print free", "r");
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

  if(!partfound[3]) {
    // Create persistent partition

    trace(LOG_INFO, "Need to create partition for persistent data");

    int start = SD_ALIGN(free_start);
    int end   = start + (PERSISTENT_SIZE / 512) - 1;
    if(create_partition(start, end, "primary", "ext4")) {
      trace(LOG_ERR, "Failed to create partition for persistent data");
      return -1;
    }
    format_partition(3);
    free_start = end + 1;
  }

  if(!partfound[4]) {
    // Create persistent partition

    trace(LOG_INFO, "Need to create partition for cached data");

    int start = SD_ALIGN(free_start);
    int end   = free_end;
    if(create_partition(start, end, "primary", "ext4")) {
      trace(LOG_ERR, "Failed to create partition for cached data");
      return -1;
    }
    format_partition(4);
  }
  return 0;
}

/**
 *
 */
static int
domount(const char *dev, const char *path)
{
  doumount(path);
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
  trace(LOG_ALERT, "Will restart in one minute");
  sleep(60);
  restart();
}


static void
dosigint(int x)
{
  got_sigint = 1;
}

/**
 *
 */
int
main(void)
{
  signal(SIGINT, dosigint);

  mkdir("/tmp/stos", 0777);

  mkdir(MNTPATH, 0777);

  openlog("showtimeshell", LOG_PID, LOG_USER);

  trace(LOG_INFO, "Checking SD card disk layout");

  setup_partitions();
  trace(LOG_INFO, "Done checking SD card disk layout");


  if(domount(PERSISTENTDEV, PERSISTENTPATH)) {
    format_partition(3);
    if(domount(PERSISTENTDEV, PERSISTENTPATH))
      panic("Unable to mount partition for persistent data after formatting");
  }

  if(domount(CACHEDEV, CACHEPATH)) {
    format_partition(4);
    if(domount(CACHEDEV, CACHEPATH))
      panic("Unable to mount partition for cache data after formatting");
  }

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
      format_partition(4);
      if(domount(CACHEDEV, CACHEPATH))
	panic("Unable to mount partition for cache data after formatting");
      continue;
    }

    if(shortrun == 6) {

      trace(LOG_ERR, "Showtime keeps respawning quickly, clearing persistent partition");
      doumount(CACHEPATH);
      format_partition(4);
      if(domount(CACHEDEV, CACHEPATH))
	panic("Unable to mount partition for persistent data after formatting");
      continue;
    }

    if(shortrun == 7) {
      panic("Guru meditation. Showtime can no longer start.");
    }

    if(exitcode == RUN_BUNDLE_FORK_FAILURE)
      restart();

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

    if(exitcode < 0)
      restart();

    if(shortrun)
      sleep(1);
  }

  return 0;
}
