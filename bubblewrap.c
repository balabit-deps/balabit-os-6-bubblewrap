/* bubblewrap
 * Copyright (C) 2016 Alexander Larsson
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <poll.h>
#include <sched.h>
#include <pwd.h>
#include <grp.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/fsuid.h>
#include <sys/signalfd.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <linux/filter.h>

#include "utils.h"
#include "network.h"
#include "bind-mount.h"

#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000 /* New cgroup namespace */
#endif

/* Globals to avoid having to use getuid(), since the uid/gid changes during runtime */
static uid_t real_uid;
static gid_t real_gid;
static uid_t overflow_uid;
static gid_t overflow_gid;
static bool is_privileged;
static const char *argv0;
static const char *host_tty_dev;
static int proc_fd = -1;
static char *opt_exec_label = NULL;
static char *opt_file_label = NULL;

char *opt_chdir_path = NULL;
bool opt_unshare_user = FALSE;
bool opt_unshare_user_try = FALSE;
bool opt_unshare_pid = FALSE;
bool opt_unshare_ipc = FALSE;
bool opt_unshare_net = FALSE;
bool opt_unshare_uts = FALSE;
bool opt_unshare_cgroup = FALSE;
bool opt_unshare_cgroup_try = FALSE;
bool opt_needs_devpts = FALSE;
bool opt_new_session = FALSE;
bool opt_die_with_parent = FALSE;
uid_t opt_sandbox_uid = -1;
gid_t opt_sandbox_gid = -1;
int opt_sync_fd = -1;
int opt_block_fd = -1;
int opt_info_fd = -1;
int opt_seccomp_fd = -1;
char *opt_sandbox_hostname = NULL;

typedef enum {
  SETUP_BIND_MOUNT,
  SETUP_RO_BIND_MOUNT,
  SETUP_DEV_BIND_MOUNT,
  SETUP_MOUNT_PROC,
  SETUP_MOUNT_DEV,
  SETUP_MOUNT_TMPFS,
  SETUP_MOUNT_MQUEUE,
  SETUP_MAKE_DIR,
  SETUP_MAKE_FILE,
  SETUP_MAKE_BIND_FILE,
  SETUP_MAKE_RO_BIND_FILE,
  SETUP_MAKE_SYMLINK,
  SETUP_REMOUNT_RO_NO_RECURSIVE,
  SETUP_SET_HOSTNAME,
} SetupOpType;

typedef enum {
  NO_CREATE_DEST = (1 << 0),
} SetupOpFlag;

typedef struct _SetupOp SetupOp;

struct _SetupOp
{
  SetupOpType type;
  const char *source;
  const char *dest;
  int         fd;
  SetupOpFlag flags;
  SetupOp    *next;
};

typedef struct _LockFile LockFile;

struct _LockFile
{
  const char *path;
  LockFile   *next;
};

static SetupOp *ops = NULL;
static SetupOp *last_op = NULL;
static LockFile *lock_files = NULL;
static LockFile *last_lock_file = NULL;

enum {
  PRIV_SEP_OP_DONE,
  PRIV_SEP_OP_BIND_MOUNT,
  PRIV_SEP_OP_PROC_MOUNT,
  PRIV_SEP_OP_TMPFS_MOUNT,
  PRIV_SEP_OP_DEVPTS_MOUNT,
  PRIV_SEP_OP_MQUEUE_MOUNT,
  PRIV_SEP_OP_REMOUNT_RO_NO_RECURSIVE,
  PRIV_SEP_OP_SET_HOSTNAME,
};

typedef struct
{
  uint32_t op;
  uint32_t flags;
  uint32_t arg1_offset;
  uint32_t arg2_offset;
} PrivSepOp;

static SetupOp *
setup_op_new (SetupOpType type)
{
  SetupOp *op = xcalloc (sizeof (SetupOp));

  op->type = type;
  op->fd = -1;
  op->flags = 0;
  if (last_op != NULL)
    last_op->next = op;
  else
    ops = op;

  last_op = op;
  return op;
}

static LockFile *
lock_file_new (const char *path)
{
  LockFile *lock = xcalloc (sizeof (LockFile));

  lock->path = path;
  if (last_lock_file != NULL)
    last_lock_file->next = lock;
  else
    lock_files = lock;

  last_lock_file = lock;
  return lock;
}


static void
usage (int ecode, FILE *out)
{
  fprintf (out, "usage: %s [OPTIONS...] COMMAND [ARGS...]\n\n", argv0);

  fprintf (out,
           "    --help                       Print this help\n"
           "    --version                    Print version\n"
           "    --args FD                    Parse nul-separated args from FD\n"
           "    --unshare-all                Unshare every namespace we support by default\n"
           "    --share-net                  Retain the network namespace (can only combine with --unshare-all)\n"
           "    --unshare-user               Create new user namespace (may be automatically implied if not setuid)\n"
           "    --unshare-user-try           Create new user namespace if possible else continue by skipping it\n"
           "    --unshare-ipc                Create new ipc namespace\n"
           "    --unshare-pid                Create new pid namespace\n"
           "    --unshare-net                Create new network namespace\n"
           "    --unshare-uts                Create new uts namespace\n"
           "    --unshare-cgroup             Create new cgroup namespace\n"
           "    --unshare-cgroup-try         Create new cgroup namespace if possible else continue by skipping it\n"
           "    --uid UID                    Custom uid in the sandbox (requires --unshare-user)\n"
           "    --gid GID                    Custon gid in the sandbox (requires --unshare-user)\n"
           "    --hostname NAME              Custom hostname in the sandbox (requires --unshare-uts)\n"
           "    --chdir DIR                  Change directory to DIR\n"
           "    --setenv VAR VALUE           Set an environment variable\n"
           "    --unsetenv VAR               Unset an environment variable\n"
           "    --lock-file DEST             Take a lock on DEST while sandbox is running\n"
           "    --sync-fd FD                 Keep this fd open while sandbox is running\n"
           "    --bind SRC DEST              Bind mount the host path SRC on DEST\n"
           "    --dev-bind SRC DEST          Bind mount the host path SRC on DEST, allowing device access\n"
           "    --ro-bind SRC DEST           Bind mount the host path SRC readonly on DEST\n"
           "    --remount-ro DEST            Remount DEST as readonly, it doesn't recursively remount\n"
           "    --exec-label LABEL           Exec Label for the sandbox\n"
           "    --file-label LABEL           File label for temporary sandbox content\n"
           "    --proc DEST                  Mount procfs on DEST\n"
           "    --dev DEST                   Mount new dev on DEST\n"
           "    --tmpfs DEST                 Mount new tmpfs on DEST\n"
           "    --mqueue DEST                Mount new mqueue on DEST\n"
           "    --dir DEST                   Create dir at DEST\n"
           "    --file FD DEST               Copy from FD to dest DEST\n"
           "    --bind-data FD DEST          Copy from FD to file which is bind-mounted on DEST\n"
           "    --ro-bind-data FD DEST       Copy from FD to file which is readonly bind-mounted on DEST\n"
           "    --symlink SRC DEST           Create symlink at DEST with target SRC\n"
           "    --seccomp FD                 Load and use seccomp rules from FD\n"
           "    --block-fd FD                Block on FD until some data to read is available\n"
           "    --info-fd FD                 Write information about the running container to FD\n"
           "    --new-session                Create a new terminal session\n"
           "    --die-with-parent            Kills with SIGKILL child process (COMMAND) when bwrap or bwrap's parent dies.\n"
          );
  exit (ecode);
}

/* If --die-with-parent was specified, use PDEATHSIG to ensure SIGKILL
 * is sent to the current process when our parent dies.
 */
static void
handle_die_with_parent (void)
{
  if (opt_die_with_parent && prctl (PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0)
    die_with_error ("prctl");
}

static void
block_sigchild (void)
{
  sigset_t mask;
  int status;

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);

  if (sigprocmask (SIG_BLOCK, &mask, NULL) == -1)
    die_with_error ("sigprocmask");

  /* Reap any outstanding zombies that we may have inherited */
  while (waitpid (-1, &status, WNOHANG) > 0)
    ;
}

static void
unblock_sigchild (void)
{
  sigset_t mask;

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);

  if (sigprocmask (SIG_UNBLOCK, &mask, NULL) == -1)
    die_with_error ("sigprocmask");
}

/* Closes all fd:s except 0,1,2 and the passed in array of extra fds */
static int
close_extra_fds (void *data, int fd)
{
  int *extra_fds = (int *) data;
  int i;

  for (i = 0; extra_fds[i] != -1; i++)
    if (fd == extra_fds[i])
      return 0;

  if (fd <= 2)
    return 0;

  close (fd);
  return 0;
}

static int
propagate_exit_status (int status)
{
  if (WIFEXITED (status))
    return WEXITSTATUS (status);

  /* The process died of a signal, we can't really report that, but we
   * can at least be bash-compatible. The bash manpage says:
   *   The return value of a simple command is its
   *   exit status, or 128+n if the command is
   *   terminated by signal n.
   */
  if (WIFSIGNALED (status))
    return 128 + WTERMSIG (status);

  /* Weird? */
  return 255;
}

/* This stays around for as long as the initial process in the app does
 * and when that exits it exits, propagating the exit status. We do this
 * by having pid 1 in the sandbox detect this exit and tell the monitor
 * the exit status via a eventfd. We also track the exit of the sandbox
 * pid 1 via a signalfd for SIGCHLD, and exit with an error in this case.
 * This is to catch e.g. problems during setup. */
static void
monitor_child (int event_fd, pid_t child_pid)
{
  int res;
  uint64_t val;
  ssize_t s;
  int signal_fd;
  sigset_t mask;
  struct pollfd fds[2];
  int num_fds;
  struct signalfd_siginfo fdsi;
  int dont_close[] = { event_fd, -1 };
  pid_t died_pid;
  int died_status;

  /* Close all extra fds in the monitoring process.
     Any passed in fds have been passed on to the child anyway. */
  fdwalk (proc_fd, close_extra_fds, dont_close);

  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);

  signal_fd = signalfd (-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
  if (signal_fd == -1)
    die_with_error ("Can't create signalfd");

  num_fds = 1;
  fds[0].fd = signal_fd;
  fds[0].events = POLLIN;
  if (event_fd != -1)
    {
      fds[1].fd = event_fd;
      fds[1].events = POLLIN;
      num_fds++;
    }

  while (1)
    {
      fds[0].revents = fds[1].revents = 0;
      res = poll (fds, num_fds, -1);
      if (res == -1 && errno != EINTR)
        die_with_error ("poll");

      /* Always read from the eventfd first, if pid 2 died then pid 1 often
       * dies too, and we could race, reporting that first and we'd lose
       * the real exit status. */
      if (event_fd != -1)
        {
          s = read (event_fd, &val, 8);
          if (s == -1 && errno != EINTR && errno != EAGAIN)
            die_with_error ("read eventfd");
          else if (s == 8)
            exit ((int) val - 1);
        }

      /* We need to read the signal_fd, or it will keep polling as read,
       * however we ignore the details as we get them from waitpid
       * below anway */
      s = read (signal_fd, &fdsi, sizeof (struct signalfd_siginfo));
      if (s == -1 && errno != EINTR && errno != EAGAIN)
        die_with_error ("read signalfd");

      /* We may actually get several sigchld compressed into one
         SIGCHLD, so we have to handle all of them. */
      while ((died_pid = waitpid (-1, &died_status, WNOHANG)) > 0)
        {
          /* We may be getting sigchild from other children too. For instance if
             someone created a child process, and then exec:ed bubblewrap. Ignore them */
          if (died_pid == child_pid)
            exit (propagate_exit_status (died_status));
        }
    }
}

/* This is pid 1 in the app sandbox. It is needed because we're using
 * pid namespaces, and someone has to reap zombies in it. We also detect
 * when the initial process (pid 2) dies and report its exit status to
 * the monitor so that it can return it to the original spawner.
 *
 * When there are no other processes in the sandbox the wait will return
 * ECHILD, and we then exit pid 1 to clean up the sandbox. */
static int
do_init (int event_fd, pid_t initial_pid, struct sock_fprog *seccomp_prog)
{
  int initial_exit_status = 1;
  LockFile *lock;

  for (lock = lock_files; lock != NULL; lock = lock->next)
    {
      int fd = open (lock->path, O_RDONLY | O_CLOEXEC);
      if (fd == -1)
        die_with_error ("Unable to open lock file %s", lock->path);

      struct flock l = {
        .l_type = F_RDLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0
      };

      if (fcntl (fd, F_SETLK, &l) < 0)
        die_with_error ("Unable to lock file %s", lock->path);

      /* Keep fd open to hang on to lock */
    }

  /* Optionally bind our lifecycle to that of the caller */
  handle_die_with_parent ();

  if (seccomp_prog != NULL &&
      prctl (PR_SET_SECCOMP, SECCOMP_MODE_FILTER, seccomp_prog) != 0)
    die_with_error ("prctl(PR_SET_SECCOMP)");

  while (TRUE)
    {
      pid_t child;
      int status;

      child = wait (&status);
      if (child == initial_pid && event_fd != -1)
        {
          uint64_t val;
          int res UNUSED;

          initial_exit_status = propagate_exit_status (status);

          val = initial_exit_status + 1;
          res = write (event_fd, &val, 8);
          /* Ignore res, if e.g. the parent died and closed event_fd
             we don't want to error out here */
        }

      if (child == -1 && errno != EINTR)
        {
          if (errno != ECHILD)
            die_with_error ("init wait()");
          break;
        }
    }

  return initial_exit_status;
}

/* low 32bit caps needed */
#define REQUIRED_CAPS_0 (CAP_TO_MASK (CAP_SYS_ADMIN) | CAP_TO_MASK (CAP_SYS_CHROOT) | CAP_TO_MASK (CAP_NET_ADMIN) | CAP_TO_MASK (CAP_SETUID) | CAP_TO_MASK (CAP_SETGID))
/* high 32bit caps needed */
#define REQUIRED_CAPS_1 0

static void
set_required_caps (void)
{
  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct data[2] = { { 0 } };

  /* Drop all non-require capabilities */
  data[0].effective = REQUIRED_CAPS_0;
  data[0].permitted = REQUIRED_CAPS_0;
  data[0].inheritable = 0;
  data[1].effective = REQUIRED_CAPS_1;
  data[1].permitted = REQUIRED_CAPS_1;
  data[1].inheritable = 0;
  if (capset (&hdr, data) < 0)
    die_with_error ("capset failed");
}

static void
drop_all_caps (void)
{
  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct data[2] = { { 0 } };

  if (capset (&hdr, data) < 0)
    die_with_error ("capset failed");
}

static bool
has_caps (void)
{
  struct __user_cap_header_struct hdr = { _LINUX_CAPABILITY_VERSION_3, 0 };
  struct __user_cap_data_struct data[2] = { { 0 } };

  if (capget (&hdr, data)  < 0)
    die_with_error ("capget failed");

  return data[0].permitted != 0 || data[1].permitted != 0;
}

static void
drop_cap_bounding_set (void)
{
  unsigned long cap;

  /* We ignore both EINVAL and EPERM, as we are actually relying
   * on PR_SET_NO_NEW_PRIVS to ensure the right capabilities are
   * available.  EPERM in particular can happen with old, buggy
   * kernels.  See:
   *  https://github.com/projectatomic/bubblewrap/pull/175#issuecomment-278051373
   *  https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git/commit/security/commoncap.c?id=160da84dbb39443fdade7151bc63a88f8e953077
   */
  for (cap = 0; cap <= 63; cap++)
    {
      int res = prctl (PR_CAPBSET_DROP, cap, 0, 0, 0);
      if (res == -1 && !(errno == EINVAL || errno == EPERM))
        die_with_error ("Dropping capability %ld from bounds", cap);
    }
}

/* This acquires the privileges that the bwrap will need it to work.
 * If bwrap is not setuid, then this does nothing, and it relies on
 * unprivileged user namespaces to be used. This case is
 * "is_privileged = FALSE".
 *
 * If bwrap is setuid, then we do things in phases.
 * The first part is run as euid 0, but with with fsuid as the real user.
 * The second part, inside the child, is run as the real user but with
 * capabilities.
 * And finally we drop all capabilities.
 * The reason for the above dance is to avoid having the setup phase
 * being able to read files the user can't, while at the same time
 * working around various kernel issues. See below for details.
 */
static void
acquire_privs (void)
{
  uid_t euid, new_fsuid;

  euid = geteuid ();

  /* Are we setuid ? */
  if (real_uid != euid)
    {
      if (euid == 0)
        is_privileged = TRUE;
      else
        die ("Unexpected setuid user %d, should be 0", euid);

      /* We want to keep running as euid=0 until at the clone()
       * operation because doing so will make the user namespace be
       * owned by root, which makes it not ptrace:able by the user as
       * it otherwise would be. After that we will run fully as the
       * user, which is necessary e.g. to be able to read from a fuse
       * mount from the user.
       *
       * However, we don't want to accidentally mis-use euid=0 for
       * escalated filesystem access before the clone(), so we set
       * fsuid to the uid.
       */
      if (setfsuid (real_uid) < 0)
        die_with_error ("Unable to set fsuid");

      /* setfsuid can't properly report errors, check that it worked (as per manpage) */
      new_fsuid = setfsuid (-1);
      if (new_fsuid != real_uid)
        die ("Unable to set fsuid (was %d)", (int)new_fsuid);

      /* We never need capabilies after execve(), so lets drop everything from the bounding set */
      drop_cap_bounding_set ();

      /* Keep only the required capabilities for setup */
      set_required_caps ();
    }
  else if (real_uid != 0 && has_caps ())
    {
      /* We have some capabilities in the non-setuid case, which should not happen.
         Probably caused by the binary being setcap instead of setuid which we
         don't support anymore */
      die ("Unexpected capabilities but not setuid, old file caps config?");
    }

  /* Else, we try unprivileged user namespaces */
}

/* This is called once we're inside the namespace */
static void
switch_to_user_with_privs (void)
{
  /* If we're in a new user namespace, we got back the bounding set, clear it again */
  if (opt_unshare_user)
    drop_cap_bounding_set ();

  if (!is_privileged)
    return;

  /* Tell kernel not clear capabilities when later dropping root uid */
  if (prctl (PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0)
    die_with_error ("prctl(PR_SET_KEEPCAPS) failed");

  if (setuid (opt_sandbox_uid) < 0)
    die_with_error ("unable to drop root uid");

  /* Regain effective required capabilities from permitted */
  set_required_caps ();
}

static void
drop_privs (void)
{
  if (!is_privileged)
    return;

  /* Drop root uid */
  if (setuid (opt_sandbox_uid) < 0)
    die_with_error ("unable to drop root uid");

  drop_all_caps ();
}

static char *
get_newroot_path (const char *path)
{
  while (*path == '/')
    path++;
  return strconcat ("/newroot/", path);
}

static char *
get_oldroot_path (const char *path)
{
  while (*path == '/')
    path++;
  return strconcat ("/oldroot/", path);
}

static void
write_uid_gid_map (uid_t sandbox_uid,
                   uid_t parent_uid,
                   uid_t sandbox_gid,
                   uid_t parent_gid,
                   pid_t pid,
                   bool  deny_groups,
                   bool  map_root)
{
  cleanup_free char *uid_map = NULL;
  cleanup_free char *gid_map = NULL;
  cleanup_free char *dir = NULL;
  cleanup_fd int dir_fd = -1;
  uid_t old_fsuid = -1;

  if (pid == -1)
    dir = xstrdup ("self");
  else
    dir = xasprintf ("%d", pid);

  dir_fd = openat (proc_fd, dir, O_RDONLY | O_PATH);
  if (dir_fd < 0)
    die_with_error ("open /proc/%s failed", dir);

  if (map_root && parent_uid != 0 && sandbox_uid != 0)
    uid_map = xasprintf ("0 %d 1\n"
                         "%d %d 1\n", overflow_uid, sandbox_uid, parent_uid);
  else
    uid_map = xasprintf ("%d %d 1\n", sandbox_uid, parent_uid);

  if (map_root && parent_gid != 0 && sandbox_gid != 0)
    gid_map = xasprintf ("0 %d 1\n"
                         "%d %d 1\n", overflow_gid, sandbox_gid, parent_gid);
  else
    gid_map = xasprintf ("%d %d 1\n", sandbox_gid, parent_gid);

  /* We have to be root to be allowed to write to the uid map
   * for setuid apps, so temporary set fsuid to 0 */
  if (is_privileged)
    old_fsuid = setfsuid (0);

  if (write_file_at (dir_fd, "uid_map", uid_map) != 0)
    die_with_error ("setting up uid map");

  if (deny_groups &&
      write_file_at (dir_fd, "setgroups", "deny\n") != 0)
    {
      /* If /proc/[pid]/setgroups does not exist, assume we are
       * running a linux kernel < 3.19, i.e. we live with the
       * vulnerability known as CVE-2014-8989 in older kernels
       * where setgroups does not exist.
       */
      if (errno != ENOENT)
        die_with_error ("error writing to setgroups");
    }

  if (write_file_at (dir_fd, "gid_map", gid_map) != 0)
    die_with_error ("setting up gid map");

  if (is_privileged)
    {
      setfsuid (old_fsuid);
      if (setfsuid (-1) != real_uid)
        die ("Unable to re-set fsuid");
    }
}

static void
privileged_op (int         privileged_op_socket,
               uint32_t    op,
               uint32_t    flags,
               const char *arg1,
               const char *arg2)
{
  if (privileged_op_socket != -1)
    {
      uint32_t buffer[2048];  /* 8k, but is int32 to guarantee nice alignment */
      PrivSepOp *op_buffer = (PrivSepOp *) buffer;
      size_t buffer_size = sizeof (PrivSepOp);
      uint32_t arg1_offset = 0, arg2_offset = 0;

      /* We're unprivileged, send this request to the privileged part */

      if (arg1 != NULL)
        {
          arg1_offset = buffer_size;
          buffer_size += strlen (arg1) + 1;
        }
      if (arg2 != NULL)
        {
          arg2_offset = buffer_size;
          buffer_size += strlen (arg2) + 1;
        }

      if (buffer_size >= sizeof (buffer))
        die ("privilege separation operation to large");

      op_buffer->op = op;
      op_buffer->flags = flags;
      op_buffer->arg1_offset = arg1_offset;
      op_buffer->arg2_offset = arg2_offset;
      if (arg1 != NULL)
        strcpy ((char *) buffer + arg1_offset, arg1);
      if (arg2 != NULL)
        strcpy ((char *) buffer + arg2_offset, arg2);

      if (write (privileged_op_socket, buffer, buffer_size) != buffer_size)
        die ("Can't write to privileged_op_socket");

      if (read (privileged_op_socket, buffer, 1) != 1)
        die ("Can't read from privileged_op_socket");

      return;
    }

  /*
   * This runs a privileged request for the unprivileged setup
   * code. Note that since the setup code is unprivileged it is not as
   * trusted, so we need to verify that all requests only affect the
   * child namespace as set up by the privileged parts of the setup,
   * and that all the code is very careful about handling input.
   *
   * This means:
   *  * Bind mounts are safe, since we always use filesystem namespace. They
   *     must be recursive though, as otherwise you can use a non-recursive bind
   *     mount to access an otherwise over-mounted mountpoint.
   *  * Mounting proc, tmpfs, mqueue, devpts in the child namespace is assumed to
   *    be safe.
   *  * Remounting RO (even non-recursive) is safe because it decreases privileges.
   *  * sethostname() is safe only if we set up a UTS namespace
   */
  switch (op)
    {
    case PRIV_SEP_OP_DONE:
      break;

    case PRIV_SEP_OP_REMOUNT_RO_NO_RECURSIVE:
      if (bind_mount (proc_fd, NULL, arg2, BIND_READONLY) != 0)
        die_with_error ("Can't remount readonly on %s", arg2);
      break;

    case PRIV_SEP_OP_BIND_MOUNT:
      /* We always bind directories recursively, otherwise this would let us
         access files that are otherwise covered on the host */
      if (bind_mount (proc_fd, arg1, arg2, BIND_RECURSIVE | flags) != 0)
        die_with_error ("Can't bind mount %s on %s", arg1, arg2);
      break;

    case PRIV_SEP_OP_PROC_MOUNT:
      if (mount ("proc", arg1, "proc", MS_MGC_VAL | MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0)
        die_with_error ("Can't mount proc on %s", arg1);
      break;

    case PRIV_SEP_OP_TMPFS_MOUNT:
      {
        cleanup_free char *opt = label_mount ("mode=0755", opt_file_label);
        if (mount ("tmpfs", arg1, "tmpfs", MS_MGC_VAL | MS_NOSUID | MS_NODEV, opt) != 0)
          die_with_error ("Can't mount tmpfs on %s", arg1);
        break;
      }

    case PRIV_SEP_OP_DEVPTS_MOUNT:
      if (mount ("devpts", arg1, "devpts", MS_MGC_VAL | MS_NOSUID | MS_NOEXEC,
                 "newinstance,ptmxmode=0666,mode=620") != 0)
        die_with_error ("Can't mount devpts on %s", arg1);
      break;

    case PRIV_SEP_OP_MQUEUE_MOUNT:
      if (mount ("mqueue", arg1, "mqueue", 0, NULL) != 0)
        die_with_error ("Can't mount mqueue on %s", arg1);
      break;

    case PRIV_SEP_OP_SET_HOSTNAME:
      /* This is checked at the start, but lets verify it here in case
         something manages to send hacked priv-sep operation requests. */
      if (!opt_unshare_uts)
        die ("Refusing to set hostname in original namespace");
      if (sethostname (arg1, strlen(arg1)) != 0)
        die_with_error ("Can't set hostname to %s", arg1);
      break;

    default:
      die ("Unexpected privileged op %d", op);
    }
}

/* This is run unprivileged in the child namespace but can request
 * some privileged operations (also in the child namespace) via the
 * privileged_op_socket.
 */
static void
setup_newroot (bool unshare_pid,
               int  privileged_op_socket)
{
  SetupOp *op;

  for (op = ops; op != NULL; op = op->next)
    {
      cleanup_free char *source = NULL;
      cleanup_free char *dest = NULL;
      int source_mode = 0;
      int i;

      if (op->source &&
          op->type != SETUP_MAKE_SYMLINK)
        {
          source = get_oldroot_path (op->source);
          source_mode = get_file_mode (source);
          if (source_mode < 0)
            die_with_error ("Can't get type of source %s", op->source);
        }

      if (op->dest &&
          (op->flags & NO_CREATE_DEST) == 0)
        {
          dest = get_newroot_path (op->dest);
          if (mkdir_with_parents (dest, 0755, FALSE) != 0)
            die_with_error ("Can't mkdir parents for %s", op->dest);
        }

      switch (op->type)
        {
        case SETUP_RO_BIND_MOUNT:
        case SETUP_DEV_BIND_MOUNT:
        case SETUP_BIND_MOUNT:
          if (source_mode == S_IFDIR)
            {
              if (mkdir (dest, 0755) != 0 && errno != EEXIST)
                die_with_error ("Can't mkdir %s", op->dest);
            }
          else if (ensure_file (dest, 0666) != 0)
            die_with_error ("Can't create file at %s", op->dest);

          privileged_op (privileged_op_socket,
                         PRIV_SEP_OP_BIND_MOUNT,
                         (op->type == SETUP_RO_BIND_MOUNT ? BIND_READONLY : 0) |
                         (op->type == SETUP_DEV_BIND_MOUNT ? BIND_DEVICES : 0),
                         source, dest);
          break;

        case SETUP_REMOUNT_RO_NO_RECURSIVE:
          privileged_op (privileged_op_socket,
                         PRIV_SEP_OP_REMOUNT_RO_NO_RECURSIVE, 0, NULL, dest);
          break;

        case SETUP_MOUNT_PROC:
          if (mkdir (dest, 0755) != 0 && errno != EEXIST)
            die_with_error ("Can't mkdir %s", op->dest);

          if (unshare_pid)
            {
              /* Our own procfs */
              privileged_op (privileged_op_socket,
                             PRIV_SEP_OP_PROC_MOUNT, 0,
                             dest, NULL);
            }
          else
            {
              /* Use system procfs, as we share pid namespace anyway */
              privileged_op (privileged_op_socket,
                             PRIV_SEP_OP_BIND_MOUNT, 0,
                             "oldroot/proc", dest);
            }

          /* There are a bunch of weird old subdirs of /proc that could potentially be
             problematic (for instance /proc/sysrq-trigger lets you shut down the machine
             if you have write access). We should not have access to these as a non-privileged
             user, but lets cover them anyway just to make sure */
          const char *cover_proc_dirs[] = { "sys", "sysrq-trigger", "irq", "bus" };
          for (i = 0; i < N_ELEMENTS (cover_proc_dirs); i++)
            {
              cleanup_free char *subdir = strconcat3 (dest, "/", cover_proc_dirs[i]);
              /* Some of these may not exist */
              if (get_file_mode (subdir) == -1)
                continue;
              privileged_op (privileged_op_socket,
                             PRIV_SEP_OP_BIND_MOUNT, BIND_READONLY,
                             subdir, subdir);
            }

          break;

        case SETUP_MOUNT_DEV:
          if (mkdir (dest, 0755) != 0 && errno != EEXIST)
            die_with_error ("Can't mkdir %s", op->dest);

          privileged_op (privileged_op_socket,
                         PRIV_SEP_OP_TMPFS_MOUNT, 0,
                         dest, NULL);

          static const char *const devnodes[] = { "null", "zero", "full", "random", "urandom", "tty" };
          for (i = 0; i < N_ELEMENTS (devnodes); i++)
            {
              cleanup_free char *node_dest = strconcat3 (dest, "/", devnodes[i]);
              cleanup_free char *node_src = strconcat ("/oldroot/dev/", devnodes[i]);
              if (create_file (node_dest, 0666, NULL) != 0)
                die_with_error ("Can't create file %s/%s", op->dest, devnodes[i]);
              privileged_op (privileged_op_socket,
                             PRIV_SEP_OP_BIND_MOUNT, BIND_DEVICES,
                             node_src, node_dest);
            }

          static const char *const stdionodes[] = { "stdin", "stdout", "stderr" };
          for (i = 0; i < N_ELEMENTS (stdionodes); i++)
            {
              cleanup_free char *target = xasprintf ("/proc/self/fd/%d", i);
              cleanup_free char *node_dest = strconcat3 (dest, "/", stdionodes[i]);
              if (symlink (target, node_dest) < 0)
                die_with_error ("Can't create symlink %s/%s", op->dest, stdionodes[i]);
            }

          {
            cleanup_free char *pts = strconcat (dest, "/pts");
            cleanup_free char *ptmx = strconcat (dest, "/ptmx");
            cleanup_free char *shm = strconcat (dest, "/shm");

            if (mkdir (shm, 0755) == -1)
              die_with_error ("Can't create %s/shm", op->dest);

            if (mkdir (pts, 0755) == -1)
              die_with_error ("Can't create %s/devpts", op->dest);
            privileged_op (privileged_op_socket,
                           PRIV_SEP_OP_DEVPTS_MOUNT, 0, pts, NULL);

            if (symlink ("pts/ptmx", ptmx) != 0)
              die_with_error ("Can't make symlink at %s/ptmx", op->dest);
          }

          /* If stdout is a tty, that means the sandbox can write to the
             outside-sandbox tty. In that case we also create a /dev/console
             that points to this tty device. This should not cause any more
             access than we already have, and it makes ttyname() work in the
             sandbox. */
          if (host_tty_dev != NULL && *host_tty_dev != 0)
            {
              cleanup_free char *src_tty_dev = strconcat ("/oldroot", host_tty_dev);
              cleanup_free char *dest_console = strconcat (dest, "/console");

              if (create_file (dest_console, 0666, NULL) != 0)
                die_with_error ("creating %s/console", op->dest);

              privileged_op (privileged_op_socket,
                             PRIV_SEP_OP_BIND_MOUNT, BIND_DEVICES,
                             src_tty_dev, dest_console);
            }

          break;

        case SETUP_MOUNT_TMPFS:
          if (mkdir (dest, 0755) != 0 && errno != EEXIST)
            die_with_error ("Can't mkdir %s", op->dest);

          privileged_op (privileged_op_socket,
                         PRIV_SEP_OP_TMPFS_MOUNT, 0,
                         dest, NULL);
          break;

        case SETUP_MOUNT_MQUEUE:
          if (mkdir (dest, 0755) != 0 && errno != EEXIST)
            die_with_error ("Can't mkdir %s", op->dest);

          privileged_op (privileged_op_socket,
                         PRIV_SEP_OP_MQUEUE_MOUNT, 0,
                         dest, NULL);
          break;

        case SETUP_MAKE_DIR:
          if (mkdir (dest, 0755) != 0 && errno != EEXIST)
            die_with_error ("Can't mkdir %s", op->dest);

          break;

        case SETUP_MAKE_FILE:
          {
            cleanup_fd int dest_fd = -1;

            dest_fd = creat (dest, 0666);
            if (dest_fd == -1)
              die_with_error ("Can't create file %s", op->dest);

            if (copy_file_data (op->fd, dest_fd) != 0)
              die_with_error ("Can't write data to file %s", op->dest);

            close (op->fd);
          }
          break;

        case SETUP_MAKE_BIND_FILE:
        case SETUP_MAKE_RO_BIND_FILE:
          {
            cleanup_fd int dest_fd = -1;
            char tempfile[] = "/bindfileXXXXXX";

            dest_fd = mkstemp (tempfile);
            if (dest_fd == -1)
              die_with_error ("Can't create tmpfile for %s", op->dest);

            if (copy_file_data (op->fd, dest_fd) != 0)
              die_with_error ("Can't write data to file %s", op->dest);

            close (op->fd);

            if (ensure_file (dest, 0666) != 0)
              die_with_error ("Can't create file at %s", op->dest);

            privileged_op (privileged_op_socket,
                           PRIV_SEP_OP_BIND_MOUNT,
                           (op->type == SETUP_MAKE_RO_BIND_FILE ? BIND_READONLY : 0),
                           tempfile, dest);

            /* Remove the file so we're sure the app can't get to it in any other way.
               Its outside the container chroot, so it shouldn't be possible, but lets
               make it really sure. */
            unlink (tempfile);
          }
          break;

        case SETUP_MAKE_SYMLINK:
          if (symlink (op->source, dest) != 0)
            die_with_error ("Can't make symlink at %s", op->dest);
          break;

        case SETUP_SET_HOSTNAME:
          privileged_op (privileged_op_socket,
                         PRIV_SEP_OP_SET_HOSTNAME, 0,
                         op->dest, NULL);
          break;

        default:
          die ("Unexpected type %d", op->type);
        }
    }
  privileged_op (privileged_op_socket,
                 PRIV_SEP_OP_DONE, 0, NULL, NULL);
}

/* We need to resolve relative symlinks in the sandbox before we
   chroot so that absolute symlinks are handled correctly. We also
   need to do this after we've switched to the real uid so that
   e.g. paths on fuse mounts work */
static void
resolve_symlinks_in_ops (void)
{
  SetupOp *op;

  for (op = ops; op != NULL; op = op->next)
    {
      const char *old_source;

      switch (op->type)
        {
        case SETUP_RO_BIND_MOUNT:
        case SETUP_DEV_BIND_MOUNT:
        case SETUP_BIND_MOUNT:
          old_source = op->source;
          op->source = realpath (old_source, NULL);
          if (op->source == NULL)
            die_with_error ("Can't find source path %s", old_source);
          break;
        default:
          break;
        }
    }
}


static const char *
resolve_string_offset (void    *buffer,
                       size_t   buffer_size,
                       uint32_t offset)
{
  if (offset == 0)
    return NULL;

  if (offset > buffer_size)
    die ("Invalid string offset %d (buffer size %zd)", offset, buffer_size);

  return (const char *) buffer + offset;
}

static uint32_t
read_priv_sec_op (int          read_socket,
                  void        *buffer,
                  size_t       buffer_size,
                  uint32_t    *flags,
                  const char **arg1,
                  const char **arg2)
{
  const PrivSepOp *op = (const PrivSepOp *) buffer;
  ssize_t rec_len;

  do
    rec_len = read (read_socket, buffer, buffer_size - 1);
  while (rec_len == -1 && errno == EINTR);

  if (rec_len < 0)
    die_with_error ("Can't read from unprivileged helper");

  if (rec_len == 0)
    exit (1); /* Privileged helper died and printed error, so exit silently */

  if (rec_len < sizeof (PrivSepOp))
    die ("Invalid size %zd from unprivileged helper", rec_len);

  /* Guarantee zero termination of any strings */
  ((char *) buffer)[rec_len] = 0;

  *flags = op->flags;
  *arg1 = resolve_string_offset (buffer, rec_len, op->arg1_offset);
  *arg2 = resolve_string_offset (buffer, rec_len, op->arg2_offset);

  return op->op;
}

static void __attribute__ ((noreturn))
print_version_and_exit (void)
{
  printf ("%s\n", PACKAGE_STRING);
  exit (0);
}

static void
parse_args_recurse (int    *argcp,
                    char ***argvp,
                    bool    in_file,
                    int    *total_parsed_argc_p)
{
  SetupOp *op;
  int argc = *argcp;
  char **argv = *argvp;
  /* I can't imagine a case where someone wants more than this.
   * If you do...you should be able to pass multiple files
   * via a single tmpfs and linking them there, etc.
   *
   * We're adding this hardening due to precedent from
   * http://googleprojectzero.blogspot.com/2014/08/the-poisoned-nul-byte-2014-edition.html
   *
   * I picked 9000 because the Internet told me to and it was hard to
   * resist.
   */
  static const uint32_t MAX_ARGS = 9000;

  if (*total_parsed_argc_p > MAX_ARGS)
    die ("Exceeded maximum number of arguments %u", MAX_ARGS);

  while (argc > 0)
    {
      const char *arg = argv[0];

      if (strcmp (arg, "--help") == 0)
        {
          usage (EXIT_SUCCESS, stdout);
        }
      else if (strcmp (arg, "--version") == 0)
        {
          print_version_and_exit ();
        }
      else if (strcmp (arg, "--args") == 0)
        {
          int the_fd;
          char *endptr;
          char *data, *p;
          char *data_end;
          size_t data_len;
          cleanup_free char **data_argv = NULL;
          char **data_argv_copy;
          int data_argc;
          int i;

          if (in_file)
            die ("--args not supported in arguments file");

          if (argc < 2)
            die ("--args takes an argument");

          the_fd = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || the_fd < 0)
            die ("Invalid fd: %s", argv[1]);

          data = load_file_data (the_fd, &data_len);
          if (data == NULL)
            die_with_error ("Can't read --args data");

          data_end = data + data_len;
          data_argc = 0;

          p = data;
          while (p != NULL && p < data_end)
            {
              data_argc++;
              (*total_parsed_argc_p)++;
              if (*total_parsed_argc_p > MAX_ARGS)
                die ("Exceeded maximum number of arguments %u", MAX_ARGS);
              p = memchr (p, 0, data_end - p);
              if (p != NULL)
                p++;
            }

          data_argv = xcalloc (sizeof (char *) * (data_argc + 1));

          i = 0;
          p = data;
          while (p != NULL && p < data_end)
            {
              /* Note: load_file_data always adds a nul terminator, so this is safe
               * even for the last string. */
              data_argv[i++] = p;
              p = memchr (p, 0, data_end - p);
              if (p != NULL)
                p++;
            }

          data_argv_copy = data_argv; /* Don't change data_argv, we need to free it */
          parse_args_recurse (&data_argc, &data_argv_copy, TRUE, total_parsed_argc_p);

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--unshare-all") == 0)
        {
          /* Keep this in order with the older (legacy) --unshare arguments,
           * we use the --try variants of user and cgroup, since we want
           * to support systems/kernels without support for those.
           */
          opt_unshare_user_try = opt_unshare_ipc = opt_unshare_pid =
            opt_unshare_uts = opt_unshare_cgroup_try =
            opt_unshare_net = TRUE;
        }
      /* Begin here the older individual --unshare variants */
      else if (strcmp (arg, "--unshare-user") == 0)
        {
          opt_unshare_user = TRUE;
        }
      else if (strcmp (arg, "--unshare-user-try") == 0)
        {
          opt_unshare_user_try = TRUE;
        }
      else if (strcmp (arg, "--unshare-ipc") == 0)
        {
          opt_unshare_ipc = TRUE;
        }
      else if (strcmp (arg, "--unshare-pid") == 0)
        {
          opt_unshare_pid = TRUE;
        }
      else if (strcmp (arg, "--unshare-net") == 0)
        {
          opt_unshare_net = TRUE;
        }
      else if (strcmp (arg, "--unshare-uts") == 0)
        {
          opt_unshare_uts = TRUE;
        }
      else if (strcmp (arg, "--unshare-cgroup") == 0)
        {
          opt_unshare_cgroup = TRUE;
        }
      else if (strcmp (arg, "--unshare-cgroup-try") == 0)
        {
          opt_unshare_cgroup_try = TRUE;
        }
      /* Begin here the newer --share variants */
      else if (strcmp (arg, "--share-net") == 0)
        {
          opt_unshare_net = FALSE;
        }
      /* End --share variants, other arguments begin */
      else if (strcmp (arg, "--chdir") == 0)
        {
          if (argc < 2)
            die ("--chdir takes one argument");

          opt_chdir_path = argv[1];
          argv++;
          argc--;
        }
      else if (strcmp (arg, "--remount-ro") == 0)
        {
          if (argc < 2)
            die ("--remount-ro takes one argument");

          SetupOp *op = setup_op_new (SETUP_REMOUNT_RO_NO_RECURSIVE);
          op->dest = argv[1];

          argv++;
          argc--;
        }
      else if (strcmp (arg, "--bind") == 0)
        {
          if (argc < 3)
            die ("--bind takes two arguments");

          op = setup_op_new (SETUP_BIND_MOUNT);
          op->source = argv[1];
          op->dest = argv[2];

          argv += 2;
          argc -= 2;
        }
      else if (strcmp (arg, "--ro-bind") == 0)
        {
          if (argc < 3)
            die ("--ro-bind takes two arguments");

          op = setup_op_new (SETUP_RO_BIND_MOUNT);
          op->source = argv[1];
          op->dest = argv[2];

          argv += 2;
          argc -= 2;
        }
      else if (strcmp (arg, "--dev-bind") == 0)
        {
          if (argc < 3)
            die ("--dev-bind takes two arguments");

          op = setup_op_new (SETUP_DEV_BIND_MOUNT);
          op->source = argv[1];
          op->dest = argv[2];

          argv += 2;
          argc -= 2;
        }
      else if (strcmp (arg, "--proc") == 0)
        {
          if (argc < 2)
            die ("--proc takes an argument");

          op = setup_op_new (SETUP_MOUNT_PROC);
          op->dest = argv[1];

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--exec-label") == 0)
        {
          if (argc < 2)
            die ("--exec-label takes an argument");
          opt_exec_label = argv[1];
          die_unless_label_valid (opt_exec_label);

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--file-label") == 0)
        {
          if (argc < 2)
            die ("--file-label takes an argument");
          opt_file_label = argv[1];
          die_unless_label_valid (opt_file_label);
          if (label_create_file (opt_file_label))
            die_with_error ("--file-label setup failed");

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--dev") == 0)
        {
          if (argc < 2)
            die ("--dev takes an argument");

          op = setup_op_new (SETUP_MOUNT_DEV);
          op->dest = argv[1];
          opt_needs_devpts = TRUE;

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--tmpfs") == 0)
        {
          if (argc < 2)
            die ("--tmpfs takes an argument");

          op = setup_op_new (SETUP_MOUNT_TMPFS);
          op->dest = argv[1];

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--mqueue") == 0)
        {
          if (argc < 2)
            die ("--mqueue takes an argument");

          op = setup_op_new (SETUP_MOUNT_MQUEUE);
          op->dest = argv[1];

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--dir") == 0)
        {
          if (argc < 2)
            die ("--dir takes an argument");

          op = setup_op_new (SETUP_MAKE_DIR);
          op->dest = argv[1];

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--file") == 0)
        {
          int file_fd;
          char *endptr;

          if (argc < 3)
            die ("--file takes two arguments");

          file_fd = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || file_fd < 0)
            die ("Invalid fd: %s", argv[1]);

          op = setup_op_new (SETUP_MAKE_FILE);
          op->fd = file_fd;
          op->dest = argv[2];

          argv += 2;
          argc -= 2;
        }
      else if (strcmp (arg, "--bind-data") == 0)
        {
          int file_fd;
          char *endptr;

          if (argc < 3)
            die ("--bind-data takes two arguments");

          file_fd = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || file_fd < 0)
            die ("Invalid fd: %s", argv[1]);

          op = setup_op_new (SETUP_MAKE_BIND_FILE);
          op->fd = file_fd;
          op->dest = argv[2];

          argv += 2;
          argc -= 2;
        }
      else if (strcmp (arg, "--ro-bind-data") == 0)
        {
          int file_fd;
          char *endptr;

          if (argc < 3)
            die ("--ro-bind-data takes two arguments");

          file_fd = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || file_fd < 0)
            die ("Invalid fd: %s", argv[1]);

          op = setup_op_new (SETUP_MAKE_RO_BIND_FILE);
          op->fd = file_fd;
          op->dest = argv[2];

          argv += 2;
          argc -= 2;
        }
      else if (strcmp (arg, "--symlink") == 0)
        {
          if (argc < 3)
            die ("--symlink takes two arguments");

          op = setup_op_new (SETUP_MAKE_SYMLINK);
          op->source = argv[1];
          op->dest = argv[2];

          argv += 2;
          argc -= 2;
        }
      else if (strcmp (arg, "--lock-file") == 0)
        {
          if (argc < 2)
            die ("--lock-file takes an argument");

          (void) lock_file_new (argv[1]);

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--sync-fd") == 0)
        {
          int the_fd;
          char *endptr;

          if (argc < 2)
            die ("--sync-fd takes an argument");

          the_fd = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || the_fd < 0)
            die ("Invalid fd: %s", argv[1]);

          opt_sync_fd = the_fd;

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--block-fd") == 0)
        {
          int the_fd;
          char *endptr;

          if (argc < 2)
            die ("--block-fd takes an argument");

          the_fd = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || the_fd < 0)
            die ("Invalid fd: %s", argv[1]);

          opt_block_fd = the_fd;

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--info-fd") == 0)
        {
          int the_fd;
          char *endptr;

          if (argc < 2)
            die ("--info-fd takes an argument");

          the_fd = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || the_fd < 0)
            die ("Invalid fd: %s", argv[1]);

          opt_info_fd = the_fd;

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--seccomp") == 0)
        {
          int the_fd;
          char *endptr;

          if (argc < 2)
            die ("--seccomp takes an argument");

          the_fd = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || the_fd < 0)
            die ("Invalid fd: %s", argv[1]);

          opt_seccomp_fd = the_fd;

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--setenv") == 0)
        {
          if (argc < 3)
            die ("--setenv takes two arguments");

          xsetenv (argv[1], argv[2], 1);

          argv += 2;
          argc -= 2;
        }
      else if (strcmp (arg, "--unsetenv") == 0)
        {
          if (argc < 2)
            die ("--unsetenv takes an argument");

          xunsetenv (argv[1]);

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--uid") == 0)
        {
          int the_uid;
          char *endptr;

          if (argc < 2)
            die ("--uid takes an argument");

          the_uid = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || the_uid < 0)
            die ("Invalid uid: %s", argv[1]);

          opt_sandbox_uid = the_uid;

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--gid") == 0)
        {
          int the_gid;
          char *endptr;

          if (argc < 2)
            die ("--gid takes an argument");

          the_gid = strtol (argv[1], &endptr, 10);
          if (argv[1][0] == 0 || endptr[0] != 0 || the_gid < 0)
            die ("Invalid gid: %s", argv[1]);

          opt_sandbox_gid = the_gid;

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--hostname") == 0)
        {
          if (argc < 2)
            die ("--hostname takes an argument");

          op = setup_op_new (SETUP_SET_HOSTNAME);
          op->dest = argv[1];
          op->flags = NO_CREATE_DEST;

          opt_sandbox_hostname = argv[1];

          argv += 1;
          argc -= 1;
        }
      else if (strcmp (arg, "--new-session") == 0)
        {
          opt_new_session = TRUE;
        }
      else if (strcmp (arg, "--die-with-parent") == 0)
        {
          opt_die_with_parent = TRUE;
        }
      else if (*arg == '-')
        {
          die ("Unknown option %s", arg);
        }
      else
        {
          break;
        }

      argv++;
      argc--;
    }

  *argcp = argc;
  *argvp = argv;
}

static void
parse_args (int    *argcp,
            char ***argvp)
{
  int total_parsed_argc = *argcp;

  parse_args_recurse (argcp, argvp, FALSE, &total_parsed_argc);
}

static void
read_overflowids (void)
{
  cleanup_free char *uid_data = NULL;
  cleanup_free char *gid_data = NULL;

  uid_data = load_file_at (AT_FDCWD, "/proc/sys/kernel/overflowuid");
  if (uid_data == NULL)
    die_with_error ("Can't read /proc/sys/kernel/overflowuid");

  overflow_uid = strtol (uid_data, NULL, 10);
  if (overflow_uid == 0)
    die ("Can't parse /proc/sys/kernel/overflowuid");

  gid_data = load_file_at (AT_FDCWD, "/proc/sys/kernel/overflowgid");
  if (gid_data == NULL)
    die_with_error ("Can't read /proc/sys/kernel/overflowgid");

  overflow_gid = strtol (gid_data, NULL, 10);
  if (overflow_gid == 0)
    die ("Can't parse /proc/sys/kernel/overflowgid");
}

int
main (int    argc,
      char **argv)
{
  mode_t old_umask;
  cleanup_free char *base_path = NULL;
  int clone_flags;
  char *old_cwd = NULL;
  pid_t pid;
  int event_fd = -1;
  int child_wait_fd = -1;
  const char *new_cwd;
  uid_t ns_uid;
  gid_t ns_gid;
  struct stat sbuf;
  uint64_t val;
  int res UNUSED;
  cleanup_free char *seccomp_data = NULL;
  size_t seccomp_len;
  struct sock_fprog seccomp_prog;

  /* Handle --version early on before we try to acquire/drop
   * any capabilities so it works in a build environment;
   * right now flatpak's build runs bubblewrap --version.
   * https://github.com/projectatomic/bubblewrap/issues/185
   */
  if (argc == 2 && (strcmp (argv[1], "--version") == 0))
    print_version_and_exit ();

  real_uid = getuid ();
  real_gid = getgid ();

  /* Get the (optional) privileges we need */
  acquire_privs ();

  /* Never gain any more privs during exec */
  if (prctl (PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0)
    die_with_error ("prctl(PR_SET_NO_NEW_CAPS) failed");

  /* The initial code is run with high permissions
     (i.e. CAP_SYS_ADMIN), so take lots of care. */

  read_overflowids ();

  argv0 = argv[0];

  if (isatty (1))
    host_tty_dev = ttyname (1);

  argv++;
  argc--;

  if (argc == 0)
    usage (EXIT_FAILURE, stderr);

  parse_args (&argc, &argv);

  /* We have to do this if we weren't installed setuid (and we're not
   * root), so let's just DWIM */
  if (!is_privileged && getuid () != 0)
    opt_unshare_user = TRUE;

#ifdef ENABLE_REQUIRE_USERNS
  /* In this build option, we require userns. */
  if (is_privileged && getuid () != 0)
    opt_unshare_user = TRUE;
#endif

  if (opt_unshare_user_try &&
      stat ("/proc/self/ns/user", &sbuf) == 0)
    {
      bool disabled = FALSE;

      /* RHEL7 has a kernel module parameter that lets you enable user namespaces */
      if (stat ("/sys/module/user_namespace/parameters/enable", &sbuf) == 0)
        {
          cleanup_free char *enable = NULL;
          enable = load_file_at (AT_FDCWD, "/sys/module/user_namespace/parameters/enable");
          if (enable != NULL && enable[0] == 'N')
            disabled = TRUE;
        }

      /* Debian lets you disable *unprivileged* user namespaces. However this is not
         a problem if we're privileged, and if we're not opt_unshare_user is TRUE
         already, and there is not much we can do, its just a non-working setup. */

      if (!disabled)
        opt_unshare_user = TRUE;
    }

  if (argc == 0)
    usage (EXIT_FAILURE, stderr);

  __debug__ (("Creating root mount point\n"));

  if (opt_sandbox_uid == -1)
    opt_sandbox_uid = real_uid;
  if (opt_sandbox_gid == -1)
    opt_sandbox_gid = real_gid;

  if (!opt_unshare_user && opt_sandbox_uid != real_uid)
    die ("Specifying --uid requires --unshare-user");

  if (!opt_unshare_user && opt_sandbox_gid != real_gid)
    die ("Specifying --gid requires --unshare-user");

  if (!opt_unshare_uts && opt_sandbox_hostname != NULL)
    die ("Specifying --hostname requires --unshare-uts");

  /* We need to read stuff from proc during the pivot_root dance, etc.
     Lets keep a fd to it open */
  proc_fd = open ("/proc", O_RDONLY | O_PATH);
  if (proc_fd == -1)
    die_with_error ("Can't open /proc");

  /* We need *some* mountpoint where we can mount the root tmpfs.
     We first try in /run, and if that fails, try in /tmp. */
  base_path = xasprintf ("/run/user/%d/.bubblewrap", real_uid);
  if (mkdir (base_path, 0755) && errno != EEXIST)
    {
      free (base_path);
      base_path = xasprintf ("/tmp/.bubblewrap-%d", real_uid);
      if (mkdir (base_path, 0755) && errno != EEXIST)
        die_with_error ("Creating root mountpoint failed");
    }

  __debug__ (("creating new namespace\n"));

  if (opt_unshare_pid)
    {
      event_fd = eventfd (0, EFD_CLOEXEC | EFD_NONBLOCK);
      if (event_fd == -1)
        die_with_error ("eventfd()");
    }

  /* We block sigchild here so that we can use signalfd in the monitor. */
  block_sigchild ();

  clone_flags = SIGCHLD | CLONE_NEWNS;
  if (opt_unshare_user)
    clone_flags |= CLONE_NEWUSER;
  if (opt_unshare_pid)
    clone_flags |= CLONE_NEWPID;
  if (opt_unshare_net)
    clone_flags |= CLONE_NEWNET;
  if (opt_unshare_ipc)
    clone_flags |= CLONE_NEWIPC;
  if (opt_unshare_uts)
    clone_flags |= CLONE_NEWUTS;
  if (opt_unshare_cgroup)
    {
      if (stat ("/proc/self/ns/cgroup", &sbuf))
        {
          if (errno == ENOENT)
            die ("Cannot create new cgroup namespace because the kernel does not support it");
          else
            die_with_error ("stat on /proc/self/ns/cgroup failed");
        }
      clone_flags |= CLONE_NEWCGROUP;
    }
  if (opt_unshare_cgroup_try)
    if (!stat ("/proc/self/ns/cgroup", &sbuf))
      clone_flags |= CLONE_NEWCGROUP;

  child_wait_fd = eventfd (0, EFD_CLOEXEC);
  if (child_wait_fd == -1)
    die_with_error ("eventfd()");

  pid = raw_clone (clone_flags, NULL);
  if (pid == -1)
    {
      if (opt_unshare_user)
        {
          if (errno == EINVAL)
            die ("Creating new namespace failed, likely because the kernel does not support user namespaces.  bwrap must be installed setuid on such systems.");
          else if (errno == EPERM && !is_privileged)
            die ("No permissions to creating new namespace, likely because the kernel does not allow non-privileged user namespaces. On e.g. debian this can be enabled with 'sysctl kernel.unprivileged_userns_clone=1'.");
        }

      die_with_error ("Creating new namespace failed");
    }

  ns_uid = opt_sandbox_uid;
  ns_gid = opt_sandbox_gid;

  if (pid != 0)
    {
      /* Parent, outside sandbox, privileged (initially) */

      if (is_privileged && opt_unshare_user)
        {
          /* We're running as euid 0, but the uid we want to map is
           * not 0. This means we're not allowed to write this from
           * the child user namespace, so we do it from the parent.
           *
           * Also, we map uid/gid 0 in the namespace (to overflowuid)
           * if opt_needs_devpts is true, because otherwise the mount
           * of devpts fails due to root not being mapped.
           */
          write_uid_gid_map (ns_uid, real_uid,
                             ns_gid, real_gid,
                             pid, TRUE, opt_needs_devpts);
        }

      /* Initial launched process, wait for exec:ed command to exit */

      /* We don't need any privileges in the launcher, drop them immediately. */
      drop_privs ();

      /* Optionally bind our lifecycle to that of the parent */
      handle_die_with_parent ();

      /* Let child run now that the uid maps are set up */
      val = 1;
      res = write (child_wait_fd, &val, 8);
      /* Ignore res, if e.g. the child died and closed child_wait_fd we don't want to error out here */
      close (child_wait_fd);

      if (opt_info_fd != -1)
        {
          cleanup_free char *output = xasprintf ("{\n    \"child-pid\": %i\n}\n", pid);
          size_t len = strlen (output);
          if (write (opt_info_fd, output, len) != len)
            die_with_error ("Write to info_fd");
          close (opt_info_fd);
        }

      monitor_child (event_fd, pid);
      exit (0); /* Should not be reached, but better safe... */
    }

  /* Child, in sandbox, privileged in the parent or in the user namespace (if --unshare-user).
   *
   * Note that for user namespaces we run as euid 0 during clone(), so
   * the child user namespace is owned by euid 0., This means that the
   * regular user namespace parent (with uid != 0) doesn't have any
   * capabilities in it, which is nice as we can't exploit those. In
   * particular the parent user namespace doesn't have CAP_PTRACE
   * which would otherwise allow the parent to hijack of the child
   * after this point.
   *
   * Unfortunately this also means you can't ptrace the final
   * sandboxed process from outside the sandbox either.
   */

  if (opt_info_fd != -1)
    close (opt_info_fd);

  /* Wait for the parent to init uid/gid maps and drop caps */
  res = read (child_wait_fd, &val, 8);
  close (child_wait_fd);

  /* At this point we can completely drop root uid, but retain the
   * required permitted caps. This allow us to do full setup as
   * the user uid, which makes e.g. fuse access work.
   */
  switch_to_user_with_privs ();

  if (opt_unshare_net)
    loopback_setup (); /* Will exit if unsuccessful */

  ns_uid = opt_sandbox_uid;
  ns_gid = opt_sandbox_gid;
  if (!is_privileged && opt_unshare_user)
    {
      /* In the unprivileged case we have to write the uid/gid maps in
       * the child, because we have no caps in the parent */

      if (opt_needs_devpts)
        {
          /* This is a bit hacky, but we need to first map the real uid/gid to
             0, otherwise we can't mount the devpts filesystem because root is
             not mapped. Later we will create another child user namespace and
             map back to the real uid */
          ns_uid = 0;
          ns_gid = 0;
        }

      write_uid_gid_map (ns_uid, real_uid,
                         ns_gid, real_gid,
                         -1, TRUE, FALSE);
    }

  old_umask = umask (0);

  /* Need to do this before the chroot, but after we're the real uid */
  resolve_symlinks_in_ops ();

  /* Mark everything as slave, so that we still
   * receive mounts from the real root, but don't
   * propagate mounts to the real root. */
  if (mount (NULL, "/", NULL, MS_SLAVE | MS_REC, NULL) < 0)
    die_with_error ("Failed to make / slave");

  /* Create a tmpfs which we will use as / in the namespace */
  if (mount ("", base_path, "tmpfs", MS_NODEV | MS_NOSUID, NULL) != 0)
    die_with_error ("Failed to mount tmpfs");

  old_cwd = get_current_dir_name ();

  /* Chdir to the new root tmpfs mount. This will be the CWD during
     the entire setup. Access old or new root via "oldroot" and "newroot". */
  if (chdir (base_path) != 0)
    die_with_error ("chdir base_path");

  /* We create a subdir "$base_path/newroot" for the new root, that
   * way we can pivot_root to base_path, and put the old root at
   * "$base_path/oldroot". This avoids problems accessing the oldroot
   * dir if the user requested to bind mount something over / */

  if (mkdir ("newroot", 0755))
    die_with_error ("Creating newroot failed");

  if (mkdir ("oldroot", 0755))
    die_with_error ("Creating oldroot failed");

  if (pivot_root (base_path, "oldroot"))
    die_with_error ("pivot_root");

  if (chdir ("/") != 0)
    die_with_error ("chdir / (base path)");

  if (is_privileged)
    {
      pid_t child;
      int privsep_sockets[2];

      if (socketpair (AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, privsep_sockets) != 0)
        die_with_error ("Can't create privsep socket");

      child = fork ();
      if (child == -1)
        die_with_error ("Can't fork unprivileged helper");

      if (child == 0)
        {
          /* Unprivileged setup process */
          drop_privs ();
          close (privsep_sockets[0]);
          setup_newroot (opt_unshare_pid, privsep_sockets[1]);
          exit (0);
        }
      else
        {
          int status;
          uint32_t buffer[2048];  /* 8k, but is int32 to guarantee nice alignment */
          uint32_t op, flags;
          const char *arg1, *arg2;
          cleanup_fd int unpriv_socket = -1;

          unpriv_socket = privsep_sockets[0];
          close (privsep_sockets[1]);

          do
            {
              op = read_priv_sec_op (unpriv_socket, buffer, sizeof (buffer),
                                     &flags, &arg1, &arg2);
              privileged_op (-1, op, flags, arg1, arg2);
              if (write (unpriv_socket, buffer, 1) != 1)
                die ("Can't write to op_socket");
            }
          while (op != PRIV_SEP_OP_DONE);

          waitpid (child, &status, 0);
          /* Continue post setup */
        }
    }
  else
    {
      setup_newroot (opt_unshare_pid, -1);
    }

  /* The old root better be rprivate or we will send unmount events to the parent namespace */
  if (mount ("oldroot", "oldroot", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
    die_with_error ("Failed to make old root rprivate");

  if (umount2 ("oldroot", MNT_DETACH))
    die_with_error ("unmount old root");

  if (opt_unshare_user &&
      (ns_uid != opt_sandbox_uid || ns_gid != opt_sandbox_gid))
    {
      /* Now that devpts is mounted and we've no need for mount
         permissions we can create a new userspace and map our uid
         1:1 */

      if (unshare (CLONE_NEWUSER))
        die_with_error ("unshare user ns");

      write_uid_gid_map (opt_sandbox_uid, ns_uid,
                         opt_sandbox_gid, ns_gid,
                         -1, FALSE, FALSE);
    }

  /* Now make /newroot the real root */
  if (chdir ("/newroot") != 0)
    die_with_error ("chdir newroot");
  if (chroot ("/newroot") != 0)
    die_with_error ("chroot /newroot");
  if (chdir ("/") != 0)
    die_with_error ("chdir /");

  /* All privileged ops are done now, so drop it */
  drop_privs ();

  if (opt_block_fd != -1)
    {
      char b[1];
      read (opt_block_fd, b, 1);
      close (opt_block_fd);
    }

  if (opt_seccomp_fd != -1)
    {
      seccomp_data = load_file_data (opt_seccomp_fd, &seccomp_len);
      if (seccomp_data == NULL)
        die_with_error ("Can't read seccomp data");

      if (seccomp_len % 8 != 0)
        die ("Invalid seccomp data, must be multiple of 8");

      seccomp_prog.len = seccomp_len / 8;
      seccomp_prog.filter = (struct sock_filter *) seccomp_data;

      close (opt_seccomp_fd);
    }

  umask (old_umask);

  new_cwd = "/";
  if (opt_chdir_path)
    {
      if (chdir (opt_chdir_path))
        die_with_error ("Can't chdir to %s", opt_chdir_path);
      new_cwd = opt_chdir_path;
    }
  else if (chdir (old_cwd) == 0)
    {
      /* If the old cwd is mapped in the sandbox, go there */
      new_cwd = old_cwd;
    }
  else
    {
      /* If the old cwd is not mapped, go to home */
      const char *home = getenv ("HOME");
      if (home != NULL &&
          chdir (home) == 0)
        new_cwd = home;
    }
  xsetenv ("PWD", new_cwd, 1);
  free (old_cwd);

  if (opt_new_session &&
      setsid () == (pid_t) -1)
    die_with_error ("setsid");

  if (label_exec (opt_exec_label) == -1)
    die_with_error ("label_exec %s", argv[0]);

  __debug__ (("forking for child\n"));

  if (opt_unshare_pid || lock_files != NULL || opt_sync_fd != -1)
    {
      /* We have to have a pid 1 in the pid namespace, because
       * otherwise we'll get a bunch of zombies as nothing reaps
       * them. Alternatively if we're using sync_fd or lock_files we
       * need some process to own these.
       */

      pid = fork ();
      if (pid == -1)
        die_with_error ("Can't fork for pid 1");

      if (pid != 0)
        {
          /* Close fds in pid 1, except stdio and optionally event_fd
             (for syncing pid 2 lifetime with monitor_child) and
             opt_sync_fd (for syncing sandbox lifetime with outside
             process).
             Any other fds will been passed on to the child though. */
          {
            int dont_close[3];
            int j = 0;
            if (event_fd != -1)
              dont_close[j++] = event_fd;
            if (opt_sync_fd != -1)
              dont_close[j++] = opt_sync_fd;
            dont_close[j++] = -1;
            fdwalk (proc_fd, close_extra_fds, dont_close);
          }

          return do_init (event_fd, pid, seccomp_data != NULL ? &seccomp_prog : NULL);
        }
    }

  __debug__ (("launch executable %s\n", argv[0]));

  if (proc_fd != -1)
    close (proc_fd);

  if (opt_sync_fd != -1)
    close (opt_sync_fd);

  /* We want sigchild in the child */
  unblock_sigchild ();

  /* Optionally bind our lifecycle */
  handle_die_with_parent ();

  /* Should be the last thing before execve() so that filters don't
   * need to handle anything above */
  if (seccomp_data != NULL &&
      prctl (PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &seccomp_prog) != 0)
    die_with_error ("prctl(PR_SET_SECCOMP)");

  if (execvp (argv[0], argv) == -1)
    die_with_error ("execvp %s", argv[0]);

  return 0;
}