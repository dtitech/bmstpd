/* SPDX-License-Identifier: GPL-2.0-or-later */
/*****************************************************************************
  Copyright (c) 2026 DTI Technologies s.r.o.

  Authors: Tomas Kyzlink <tkyzlink@dtitech.cz>

******************************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <poll.h>
#include <sys/wait.h>

#include "exec_cmd.h"

struct exec_pipe;
struct exec_str;

typedef int (*exec_pipefn)(struct exec_str *hnd, struct pollfd *fd, struct exec_pipe *ep);

struct exec_pipe
{
    struct iobuf *buf;
    exec_pipefn fn;
    int local;
    int cmd;
    short ev;
};

struct exec_str
{
    bool eto;
    pid_t pid;
    char *buf;
    size_t buf_sz;
    struct exec_pipe in;
    struct exec_pipe out;
    struct exec_pipe err;
};

static char ** exec_gen_params(const char *cmd, const char **params)
{
    int cnt = 0;
    while (params[cnt] != NULL)
        cnt++;
    cnt++;

    char ** pout = (char **)malloc(sizeof(void *) * (cnt + 1));
    if (pout != NULL)
      {
        pout[0] = (char *)cmd;
        for (int p = 0; p < cnt; p++)
            pout[p + 1] = (char *)params[p];

        return pout;
      }

    return NULL;
}

static int exec_pipe_init(struct exec_pipe *ph, bool out)
{
    int fds[2];

    const int ret = pipe(fds);
    if (ret)
        return -1;

    if (out)
    {
        ph->local = fds[1];
        ph->cmd = fds[0];
    }
    else
    {
        ph->local = fds[0];
        ph->cmd = fds[1];
    }

    return 0;
}

static void exec_pipe_free(struct exec_pipe *ph)
{
    if (ph->local >= 0)
    {
        close(ph->local);
        ph->local = -1;
    }
    if (ph->cmd >= 0)
    {
        close(ph->cmd);
        ph->cmd = -1;
    }
}

static int exec_pipein(struct exec_str *hnd, struct pollfd *fd, struct exec_pipe *ep)
{
    if (fd->revents & POLLOUT)
    {
        const int wtn = iobuf_write_to_fd(ep->local, ep->buf);
        return (wtn > 0) ? 0 : 1;
    }
    else if (fd->revents & POLLHUP)
        return 1;

    return -1;
}

static int exec_pipeout(struct exec_str *hnd, struct pollfd *fd, struct exec_pipe *ep)
{
    if (fd->revents & POLLIN)
    {
        const int rdd = iobuf_read_from_fd(ep->buf, ep->local);
        return (rdd > 0) ? 0 : 1;
    }
    else if (fd->revents & POLLHUP)
        return 1;

    return -1;
}

static int exec_pipenull(struct exec_str *hnd, struct pollfd *fd, struct exec_pipe *ep)
{
    if (fd->revents & POLLIN)
    {
        if (!hnd->buf)
        {
            hnd->buf_sz = 4096;
            hnd->buf = malloc(hnd->buf_sz);
            if (!hnd->buf)
                return -1;
        }

        const int rdd = read(ep->local, hnd->buf, hnd->buf_sz);
        return (rdd > 0) ? 0 : 1;
    }
    else if (fd->revents & POLLHUP)
        return 1;

    return -1;
}

static int exec_init(struct exec_str *hnd, struct iobuf *in, struct iobuf *out, struct iobuf *err)
{
    hnd->eto = (out == err);
    hnd->in.buf = in;
    hnd->in.local = -1;
    hnd->in.cmd = -1;
    hnd->out.buf = out;
    hnd->out.local = -1;
    hnd->out.cmd = -1;
    hnd->err.buf = (hnd->eto) ? NULL : err;
    hnd->err.local = -1;
    hnd->err.cmd = -1;

    if (exec_pipe_init(&(hnd->in), true))
        return -1;

    if (exec_pipe_init(&(hnd->out), false))
        return -1;

    if (!hnd->eto)
    {
        if (exec_pipe_init(&(hnd->err), false))
            return -1;
    }

    return 0;
}

static int exec_clean(struct exec_str *hnd)
{
    exec_pipe_free(&(hnd->in));
    exec_pipe_free(&(hnd->out));
    exec_pipe_free(&(hnd->err));

    return 0;
}

static void exec_child(struct exec_str *hnd, struct exec_sec *sec, const char *cmd, const char **params)
{
    if (hnd->eto)
    {
        const int pipe_in = hnd->in.cmd;
        const int pipe_out = hnd->out.cmd;

        const int cnt = getdtablesize();
        for (int p = 0; p < cnt; p++)
        {
            if ((p != pipe_in) && (p != pipe_out))
                close(p);
        }

        dup2(pipe_in, STDIN_FILENO);
        dup2(pipe_out, STDOUT_FILENO);
        dup2(pipe_out, STDERR_FILENO);
        close(pipe_in);
        close(pipe_out);
    }
    else
    {
        const int pipe_in = hnd->in.cmd;
        const int pipe_out = hnd->out.cmd;
        const int pipe_err = hnd->err.cmd;

        const int cnt = getdtablesize();
        for (int p = 0; p < cnt; p++)
        {
            if ((p != pipe_in) && (p != pipe_out) && (p != pipe_err))
                close(p);
        }

        dup2(pipe_in, STDIN_FILENO);
        dup2(pipe_out, STDOUT_FILENO);
        dup2(pipe_err, STDERR_FILENO);
        close(pipe_in);
        close(pipe_out);
        close(pipe_err);
    }

    if (sec)
    {
        setgid(sec->gid);
        setuid(sec->uid);
    }

    execvp(cmd, (char * const*)params);

    exit(-1);
}

static int exec_loop(struct exec_str *hnd, int *exitcode)
{
    int eret = 0;
    struct timespec tm;
    sigset_t sset;
    siginfo_t sinfo;
    struct exec_pipe *pipes[3];
    struct pollfd fds[3];
    int fds_cnt = 0;

    tm.tv_sec = 0;
    tm.tv_nsec = 100 * 1000 * 1000; // 100ms

    sigemptyset(&sset);
    sigaddset(&sset, SIGCHLD);

    if (hnd->in.buf)
    {
        hnd->in.fn = exec_pipein;
        hnd->in.ev = POLLOUT;
        pipes[fds_cnt] = &(hnd->in);
        fds_cnt++;
    }

    hnd->out.fn = (hnd->out.buf) ? exec_pipeout : exec_pipenull;
    hnd->out.ev = POLLIN;
    pipes[fds_cnt] = &(hnd->out);
    fds_cnt++;

    if (!hnd->eto)
    {
        hnd->err.fn = (hnd->err.buf) ? exec_pipeout : exec_pipenull;
        hnd->err.ev = POLLIN;
        pipes[fds_cnt] = &(hnd->err);
        fds_cnt++;
    }

    for (int p = 0; p < fds_cnt; p++)
    {
        fds[p].fd = pipes[p]->local;
        fds[p].events = pipes[p]->ev;
    }

    for (;;)
    {
        int ret = ppoll(fds, fds_cnt, &tm, &sset);
        if (ret >= 0)
        {
            if (ret > 0)
            {
                for (int p = 0; p < fds_cnt; p++)
                {
                    if (fds[p].revents)
                    {
                        int pret = (pipes[p]->fn)(hnd, &fds[p], pipes[p]);
                        if (pret)
                        {
                            if (pret == 1)
                            {
                                close(pipes[p]->local);
                                pipes[p]->local = -1;
                                eret = 1;
                            }
                            else
                            {
                                eret = -1;
                                break;
                            }
                        }
                    }
                }

                if (eret > 0)
                {
                    int new_cnt = 0;
                    for (int p = 0; p < fds_cnt; p++)
                    {
                        struct exec_pipe *cph = pipes[p];
                        if (cph->local >= 0)
                        {
                            pipes[new_cnt] = cph;
                            fds[new_cnt].fd = cph->local;
                            fds[new_cnt].events = cph->ev;
                            new_cnt++;
                        }
                    }

                    fds_cnt = new_cnt;
                    eret = 0;
                }
            }

            if (!waitid(P_PID, hnd->pid, &sinfo, WEXITED | WNOHANG))
            {
                if (sinfo.si_code)
                {
                    fprintf(stderr, "EXIT %d\n", sinfo.si_code);
                    if (exitcode)
                        *exitcode = sinfo.si_value.sival_int;
                    break;
                }
            }
            else
            {
                eret = -1;
                break;
            }
        }
        else
        {
            eret = -1;
            break;
        }
    }

    free(hnd->buf);
    return eret;
}

int exec_cmd(struct iobuf *in, struct iobuf *out, struct iobuf *err, int *exitcode, struct exec_sec *sec, const char *cmd, const char **params)
{
    struct exec_str hnd;

    if ((!cmd))
        return -1;

    if (exitcode)
        *exitcode = -1;

    memset(&hnd, 0, sizeof(struct exec_str));
    int ret = exec_init(&hnd, in, out, err);
    if (ret)
        return -1;

    ret = -1;
    char **eparms = exec_gen_params(cmd, params);
    if (eparms)
    {
        pid_t pid = fork();
        if (pid == 0)
            exec_child(&hnd, sec, cmd, (const char **)eparms);
        else if (pid > 0)
        {
            free(eparms);
            hnd.pid = pid;
            close(hnd.in.cmd);
            hnd.in.cmd = -1;
            close(hnd.out.cmd);
            hnd.out.cmd = -1;
            if (hnd.err.cmd >= 0)
            {
                close(hnd.err.cmd);
                hnd.err.cmd = 0;
            }

            ret = exec_loop(&hnd, exitcode);
        }
    }

    exec_clean(&hnd);
    return ret;
}
