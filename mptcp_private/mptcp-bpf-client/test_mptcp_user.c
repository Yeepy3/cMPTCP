// SPDX-License-Identifier: GPL-2.0
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/bpf.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include "bpf_util.h"
#include "bpf_rlimit.h"
#include <linux/perf_event.h>
#include "test_tcpbpf.h"

// copied from samples/bpf/bpf_load.c
#define DEBUGFS "/sys/kernel/debug/tracing/"
void read_trace_pipe(void)
{
	int trace_fd;

	trace_fd = open(DEBUGFS "trace_pipe", O_RDONLY, 0);
	if (trace_fd < 0)
		return;

	while (1) {
		static char buf[4096];
		ssize_t sz;

		sz = read(trace_fd, buf, sizeof(buf));
		if (sz > 0) {
			buf[sz] = 0;
			puts(buf);
		}
	}
}

#define SYSTEM(CMD)						\
	do {							\
		if (system(CMD)) {				\
			printf("system(%s) FAILS!\n", CMD);	\
		}						\
		else printf("system(%s) PASS!\n", CMD);		\
	} while (0)

int main(int argc, char **argv)
{
	const char *file, *script;
	int cg_fd, prog_fd;
	bool debug_flag = true;
	int error = EXIT_FAILURE;
	struct bpf_object *obj;
	char cmd[100], *dir;
	struct stat buffer;
	int pid;
	int rv;

	if (argc > 2) {
		file = argv[1];
		script = argv[2];
	} else {
		printf("Please specify the bpf program object and script.\n"
			" e.g.: ./test_mptcp_user  bpf_mptcp_ndiffports.o ./curl-multipath-tcp.org \n");
		exit(1);
	}

	if (argc > 3  &&  strncmp(argv[3], "-q", 2) == 0) {
		printf("Quiet mode\n");
		debug_flag = false;
	}

	printf("loading bpf program: %s\n", file);

	dir = "/tmp/cgroupv2/foo";

	if (stat(dir, &buffer) != 0) {
		printf("stat not found, creating cgroup \n");
		SYSTEM("mkdir -p /tmp/cgroupv2");
		SYSTEM("mount -t cgroup2 none /tmp/cgroupv2");
		SYSTEM("mkdir -p /tmp/cgroupv2/foo");
	}
	pid = (int) getpid();
	sprintf(cmd, "echo %d >> /tmp/cgroupv2/foo/cgroup.procs", pid);
	SYSTEM(cmd);

	cg_fd = open(dir, O_DIRECTORY, O_RDONLY);
	if (bpf_prog_load(file, BPF_PROG_TYPE_SOCK_OPS, &obj, &prog_fd)) {
		printf("FAILED: load_bpf_file failed for: %s\n", file);
		goto err;
	}

	rv = bpf_prog_attach(prog_fd, cg_fd, BPF_CGROUP_SOCK_OPS, 0);
	if (rv) {
		printf("FAILED: bpf_prog_attach: %d (%s)\n",
		       error, strerror(errno));
		goto err;
	}

	//SYSTEM("curl multipath-tcp.org");
	//SYSTEM("./my_net.sh");
	SYSTEM("iperf3 -c 192.168.1.112 -p 8080 -R -t 60 ");
	//SYSTEM("python &");
	//SYSTEM("python /home/sandesh/workspace/journal/static/DASH/dash_stats.py rbased_1 http://192.168.1.112/DASH/dash.js/samples/advanced/monitoring.html 90");
	//SYSTEM("./dash_stats.py 1 http://192.168.1.112/DASH/dash.js/samples/advanced/monitoring.html 30 ");
	//SYSTEM("google-chrome --no-sandbox --incognito http://192.168.1.112/DASH/dash.js/samples/advanced/monitoring.html");
	if (0) {
		printf(" Hello World\n");
		read_trace_pipe();
	}

	error = 0;
err:
	bpf_prog_detach(cg_fd, BPF_CGROUP_SOCK_OPS);
	return error;

}
