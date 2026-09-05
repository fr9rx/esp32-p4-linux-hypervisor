/* Tiny binary /init for ESP32-P4 Linux port.
 *
 * Replaces buildroot's shell-based fs/cpio/init because busybox sh
 * fails to exec via BINFMT_SCRIPT on this port (M-mode NOMMU RV32).
 * Mounts devtmpfs on /dev, opens /dev/console as stdin/out/err,
 * execs /sbin/init.
 */
#include <sys/mount.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>

static const char *const init_argv[] = { "/sbin/init", 0 };
static const char *const init_envp[] = {
	"HOME=/",
	"TERM=linux",
	"PATH=/sbin:/usr/sbin:/bin:/usr/bin",
	0,
};

int main(int argc, char **argv)
{
	int fd;

	/* Best-effort; ignore errors (devtmpfs may already be mounted). */
	mount("devtmpfs", "/dev", "devtmpfs", 0, 0);

	fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}

	execve("/sbin/init", (char *const *)init_argv, (char *const *)init_envp);
	/* If execve fails, fall back to /bin/sh so the user sees *something*. */
	execve("/bin/sh", (char *const []){ "/bin/sh", 0 }, (char *const *)init_envp);
	return 1;
}
