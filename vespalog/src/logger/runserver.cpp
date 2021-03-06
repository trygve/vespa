// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <csignal>

#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <fcntl.h>

#include <vespa/defaults.h>
#include <vespa/log/llparser.h>
#include "llreader.h"
#include <vespa/log/log.h>

LOG_SETUP("runserver");

// XXX should close all file descriptors,
// XXX but that makes logging go haywire

static volatile sig_atomic_t gotstopsig = 0;
static volatile sig_atomic_t lastsig = 0;
static volatile sig_atomic_t unhandledsig = 0;

extern "C" {
void termsig(int sig) {
    lastsig = sig;
    gotstopsig = 1;
    unhandledsig = 1;
}
}

class PidFile
{
private:
    char *_pidfile;
    int _fd;
    PidFile(const PidFile&);
    PidFile& operator= (const PidFile&);
public:
    PidFile(const char *pidfile) : _pidfile(strdup(pidfile)), _fd(-1) {}
    ~PidFile() { free(_pidfile); if (_fd >= 0) close(_fd); }
    int readPid();
    void writePid();
    bool writeOpen();
    bool isRunning();
    bool isMine();
    void cleanUp();
};

void
PidFile::cleanUp()
{
    if (isMine() || !isRunning()) remove(_pidfile);
    if (_fd >= 0) close(_fd);
    _fd = -1;
}

bool
PidFile::writeOpen()
{
    if (_fd >= 0) close(_fd);
    int flags = O_CREAT | O_WRONLY | O_NONBLOCK;
    _fd = open(_pidfile, flags, 0644);
    if (_fd < 0) {
        fprintf(stderr, "could not create pidfile %s: %s\n", _pidfile,
                strerror(errno));
        return false;
    }
    // XXX should we use locking or not?
    if (flock(_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "could not lock pidfile %s: %s\n", _pidfile,
                strerror(errno));
        close(_fd);
        _fd = -1;
        return false;
    }
    fcntl(_fd, F_SETFD, FD_CLOEXEC);
    return true;
}

void
PidFile::writePid()
{
    if (_fd < 0) abort();
    ftruncate(_fd, (off_t)0);
    char buf[100];
    sprintf(buf, "%d\n", getpid());
    int l = strlen(buf);
    ssize_t didw = write(_fd, buf, l);
    if (didw != l) {
        fprintf(stderr, "could not write pid to %s: %s\n",
                _pidfile, strerror(errno));
        exit(1);
    }
    LOG(debug, "wrote '%s' to %s (fd %d)", buf, _pidfile, _fd);
}

int
PidFile::readPid()
{
    FILE *pf = fopen(_pidfile, "r");
    if (pf == NULL) return 0;
    char buf[100];
    strcpy(buf, "0");
    fgets(buf, 100, pf);
    fclose(pf);
    return atoi(buf);
}

bool
PidFile::isRunning()
{
    int pid = readPid();
    if (pid < 1) return false;
    return (kill(pid, 0) == 0 || errno == EPERM);
}

bool
PidFile::isMine()
{
    int pid = readPid();
    return (pid == getpid());
}

using namespace ns_log;

int loop(const char *svc, char * const * run)
{
    int pstdout[2];
    int pstderr[2];

    if (pipe(pstdout) < 0 || pipe(pstderr) < 0) {
        LOG(error, "pipe: %s", strerror(errno));
        exit(1);
    }
    LOG(debug, "stdout pipe %d <- %d; stderr pipe %d <- %d",
        pstdout[0], pstdout[1],
        pstderr[0], pstderr[1]);

    int high = 1 + pstdout[0] + pstderr[0];

    pid_t child = fork();

    if (child == 0) {
        // I am the child process
        dup2(pstdout[1], 1);
        dup2(pstderr[1], 2);
        close(pstdout[0]);
        close(pstderr[0]);
        close(pstdout[1]);
        close(pstderr[1]);
        execvp(run[0], run);
        LOG(error, "exec %s: %s", run[0], strerror(errno));
        exit(1);
    }
    if (child < 0) {
        LOG(error, "fork(): %s", strerror(errno));
        exit(1);
    }
    // I am the parent process

    LOG(debug, "started %s (pid %d)", run[0], (int)child);
    std::string torun = run[0];
    for (char * const *arg = (run + 1); *arg != NULL; ++arg) {
        torun += " ";
        torun += *arg;
    }

    {
        torun += " (pid ";
        char buf[20];
        sprintf(buf, "%d", (int)child);
        torun += buf;
        torun += ")";
    }
    EV_STARTING(torun.c_str());

    close(pstdout[1]);
    close(pstderr[1]);

    LLParser outvia;
    LLParser errvia;
    outvia.setDefaultLevel(Logger::info);
    errvia.setDefaultLevel(Logger::warning);

    outvia.setService(svc);
    errvia.setService(svc);
    outvia.setComponent("stdout");
    errvia.setComponent("stderr");
    outvia.setPid(child);
    errvia.setPid(child);

    InputBuf outReader(pstdout[0]);
    InputBuf errReader(pstderr[0]);

    bool outeof = false;
    bool erreof = false;

    int wstat = 0;

    while (child || !outeof || !erreof) {
        struct timeval timeout;

        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // == 100 ms == 1/10 s

        fd_set pipes;

        FD_ZERO(&pipes);
        if (!outeof) FD_SET(pstdout[0], &pipes);
        if (!erreof) FD_SET(pstderr[0], &pipes);

        int n = select(high, &pipes, NULL, NULL, &timeout);
        if (n > 0) {
            if (FD_ISSET(pstdout[0], &pipes)) {
                LOG(debug, "out reader has input");
                if (outReader.blockRead()) {
                    while (outReader.hasInput()) {
                        LOG(debug, "process out reader input");
                        outReader.doInput(outvia);
                    }
                } else {
                    LOG(debug, "eof on stdout");
                    outeof = true; // EOF on stdout
                    close(pstdout[0]);
                }
            }
            if (FD_ISSET(pstderr[0], &pipes)) {
                LOG(debug, "err reader has input");
                if (errReader.blockRead()) {
                    while (errReader.hasInput()) {
                        LOG(debug, "process err reader input");
                        errReader.doInput(errvia);
                    }
                } else {
                    LOG(debug, "eof on stderr");
                    erreof = true; // EOF on stderr
                    close(pstderr[0]);
                }
            }
        }

        if (child != 0) {
            int cpid = waitpid(child, &wstat, WNOHANG);
            if (cpid == child) {
                if (WIFSTOPPED(wstat)) {
                    LOG(info, "child %d stopped, waiting for it to continue",
                        cpid);
                } else if (WIFEXITED(wstat)) {
                    // child terminated
                    LOG(debug, "child %d exit status: %d", cpid,
                        (int)WEXITSTATUS(wstat));
                    EV_STOPPED(torun.c_str(), (int)child, (int)WEXITSTATUS(wstat));
                    child = 0;
                } else if (WIFSIGNALED(wstat)) {
                    if (WTERMSIG(wstat) != lastsig) {
                        LOG(warning, "child died from signal: %d", WTERMSIG(wstat));
                        if (WCOREDUMP(wstat)) {
                            LOG(info, "child %d dumped core", cpid);
                        }
                    }
                    child = 0;
                } else {
                    LOG(error, "unexpected status %d from waidpit", wstat);
                    abort();
                }
            } else if (cpid < 0) {
                LOG(error, "waitpid: %s", strerror(errno));
                abort();
            } else if (cpid != 0) {
                LOG(warning, "unexpected status %d for pid %d",
                    wstat, cpid);
                abort();
            }
        }
        if (unhandledsig && child != 0) {
            LOG(debug, "got signal %d, sending to pid %d",
                (int)lastsig, (int)child);
            char why[256];
            sprintf(why, "got signal %d", (int)lastsig);
            EV_STOPPING(torun.c_str(), why);
            kill(child, lastsig);
            unhandledsig = 0;
        }
    }
    if (WIFSIGNALED(wstat))
        return WTERMSIG(wstat);
    return WEXITSTATUS(wstat);
}

void usage(char *prog, int es)
{
    fprintf(stderr, "Usage: %s\n"
            "       [-s service] [-r restartinterval] [-p pidfile]"
            " program [args ...]\n"
            "or:    [-p pidfile] [-k killcmd] -S\n", prog);
    exit(es);
}

int main(int argc, char *argv[])
{
    bool doStop = false;
    int restart = 0;
    const char *service = "runserver";
    const char *pidfile = "vespa-runserver.pid"; // XXX bad default?
    const char *killcmd = NULL;

    signal(SIGQUIT, SIG_IGN);

    int ch;
    while ((ch = getopt(argc, argv, "k:s:r:p:Sh")) != -1) {
        switch (ch) {
        case 's':
            service = optarg;
            break;
        case 'r':
            restart = atoi(optarg);
            break;
        case 'p':
            pidfile = optarg;
            break;
        case 'S':
            doStop = true;
            break;
        case 'k':
            killcmd = optarg;
            break;
        default:
            usage(argv[0], ch != 'h');
        }
    }

    const char *envROOT = getenv("ROOT");
    if (envROOT == NULL || envROOT[0] == '\0') {
        envROOT = vespa::Defaults::vespaHome();
        setenv("ROOT", envROOT, 1);
    }
    if (chdir(envROOT) != 0) {
        fprintf(stderr, "Cannot chdir to %s: %s\n", envROOT, strerror(errno));
        exit(1);
    }

    PidFile mypf(pidfile);
    if (doStop) {
        if (mypf.isRunning()) {
            int pid = mypf.readPid();
            if (killcmd != NULL) {
                fprintf(stdout, "%s was running with pid %d, running '%s' to stop it\n",
                        service, pid, killcmd);
                if (system(killcmd) != 0) {
                    fprintf(stderr, "WARNING: stop command '%s' had some problem\n", killcmd);
                }
            } else {
                fprintf(stdout, "%s was running with pid %d, sending SIGTERM\n",
                    service, pid);
                if (killpg(pid, SIGTERM) != 0) {
                    fprintf(stderr, "could not signal %d: %s\n", pid,
                            strerror(errno));
                    exit(1);
                }
            }
            fprintf(stdout, "Waiting for exit (up to 60 seconds)\n");
            for (int cnt(0); cnt < 1800; cnt++) {
                usleep(100000); // wait 0.1 seconds
                if ((cnt > 300) && (cnt % 100 == 0)) {
                    killpg(pid, SIGTERM);
                }
                if (killpg(pid, 0) == 0) {
                    if (cnt%10 == 0) {
                        fprintf(stdout, ".");
                        fflush(stdout);
                    }
                } else {
                    fprintf(stdout, "DONE\n");
                    break;
                }
                if (cnt == 900) {
                    printf("\ngiving up, sending KILL signal\n");
                    killpg(pid, SIGKILL);
                }
            }
        } else {
            fprintf(stdout, "%s not running according to %s\n",
                    service, pidfile);
        }
        mypf.cleanUp();
        exit(0);
    }
    if (optind >= argc || killcmd != NULL) {
        usage(argv[0], 1);
    }

    if (mypf.isRunning()) {
        fprintf(stderr, "runserver already running with pid %d\n",
                mypf.readPid());
        exit(0);
    }

    if (!mypf.writeOpen()) {
        perror(pidfile);
        return 1;
    }

    pid_t rsp = fork();
    if (rsp == 0) {
        close(0);
        if (open("/dev/null", O_RDONLY) != 0) {
            perror("open /dev/null for reading failed");
            exit(1);
        }
        close(1);
        if (open("/dev/null", O_WRONLY) != 1) {
            perror("open /dev/null for writing failed");
            exit(1);
        }
        dup2(1, 2);
        if (setsid() < 0) {
            perror("setsid");
            exit(1);
        }
        struct sigaction act;
        struct sigaction oact;

        memset(&act, 0, sizeof(act));

        act.sa_handler = termsig;

        sigaction(SIGINT, &act, &oact);
        sigaction(SIGTERM, &act, &oact);

        int stat = 0;
        try {
            mypf.writePid();
            do {
                time_t laststart = time(NULL);
                stat = loop(service, argv+optind);
                if (restart > 0 && !gotstopsig) {
                    int wt = restart + laststart - time(NULL);
                    if (wt < 0) wt = 0;
                    LOG(info, "will restart in %d seconds", wt);
                }
                while (!gotstopsig && time(NULL) - laststart < restart) {
                    sleep(1);
                }
            } while (!gotstopsig && restart > 0);
        } catch (MsgException& ex) {
            LOG(error, "exception: '%s'", ex.what());
            exit(1);
        }
        if (restart > 0) {
            LOG(debug, "final exit status: %d", stat);
        }
        mypf.cleanUp();
        exit(stat);
    }

    if (rsp < 0) {
        perror("fork");
        return 1;
    }
    printf("runserver(%s) running with pid: %d\n", service, rsp);
    return 0;
}
