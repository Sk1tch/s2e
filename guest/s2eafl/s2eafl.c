/*
 * S2E Selective Symbolic Execution Framework
 *
 * Copyright (c) 2015, Information Security Laboratory, NUDT
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Information Security Laboratory, NUDT nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE INFORMATION SECURITY LABORATORY, NUDT BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * All contributors are listed in S2E-AUTHORS file.
 *
 */

#include <s2e.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef WIN32
#include <windows.h>
#define SLEEP(x) Sleep((x) * 1000)
#else
#define SLEEP(x) sleep(x)
#endif

#define AFLS2EGUESTPIPE 284
#define AFLS2EGUESTPIPE_CHILD 384
#define MAXARGNUM 16 // we support at most 16 arguments
const char *g_s2etools_dir = NULL;
const char *g_target_file = NULL;
const char *g_target_code = NULL;
const char *g_target_args[MAXARGNUM] =
{ NULL };
int needSymwrite = 0;

int is1sttime = 1;

static void print_usage(const char *prog_name)
{
    fprintf(stderr, "Usage: %s --s2etools-dir s2etools_dir "
            "--target-file target_file --target-code target_code "
            "--target-args arg1 arg2 ...\n\n", prog_name);

    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --s2etools-dir : specify s2e tools directory.\n");
    fprintf(stderr,
            "  --target-file  : specify the filename generated by AFL.\n");
    fprintf(stderr,
                "  --need-symbolic  : if need to symbolic execution, just set this option.\n");
    fprintf(stderr,
            "  --target-code  : specify the full path of target binary code.\n");
    fprintf(stderr,
            "  --target-args  : specify the arguments of target binary code, replace file argument with '@'.\n");
}

static void symbfile(const char *filename)
{

    int fd = open(filename, O_RDWR);
    if (fd < 0) {
        s2e_kill_state_printf(-1, "symbfile: could not open %s\n", filename);
        return;
    }
    printf("we have successfully opened %s.\n", filename);
    /* Determine the size of the file */
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        s2e_kill_state_printf(-1, "symbfile: could not determine the size of %s\n", filename);
        return;
    }
    printf("we get file size through lseek.\n");
    char buffer[0x1000];

    unsigned current_chunk = 0;
    unsigned total_chunks = size / sizeof(buffer);
    if (size % sizeof(buffer)) {
        ++total_chunks;
    }

    /**
     * Replace slashes in the filename with underscores.
     * It should make it easier for plugins to generate
     * concrete files, while preserving info about the original path
     * and without having to deal with the slashes.
     */
    char cleaned_name[512];
    strncpy(cleaned_name, filename, sizeof(cleaned_name));
    for (unsigned i = 0; cleaned_name[i]; ++i) {
        if (cleaned_name[i] == '/') {
            cleaned_name[i] = '_';
        }
    }
    printf("we are ready to write symbolic data to file.\n");
    off_t offset = 0;
    do {
        /* Read the file in chunks of 4K and make them concolic */
        char symbvarname[512];

        if (lseek(fd, offset, SEEK_SET) < 0) {
            s2e_kill_state_printf(-1, "symbfile: could not seek to position %d", offset);
            return;
        }

        ssize_t totransfer = size > sizeof(buffer) ? sizeof(buffer) : size;

        /* Read the data */
        ssize_t read_count = read(fd, buffer, totransfer);
        if (read_count < 0) {
            s2e_kill_state_printf(-1, "symbfile: could not read from file %s", filename);
            return;
        }
        printf("we have read %d bytes of data from file.\n", read_count);
        /**
         * Make the buffer concolic.
         * The symbolic variable name encodes the original file name with its path
         * as well as the chunk id contained in the buffer.
         * A test case generator should therefore be able to reconstruct concrete
         * files easily.
         */
        snprintf(symbvarname, sizeof(symbvarname), "__symfile___%s___%d_%d_symfile__",
                 cleaned_name, current_chunk, total_chunks);
        s2e_make_concolic(buffer, read_count, symbvarname);
        printf("we successfully make some data symbolic.\n");
        /* Write it back */
        if (lseek(fd, offset, SEEK_SET) < 0) {
            s2e_kill_state_printf(-1, "symbfile: could not seek to position %d", offset);
            return;
        }

        ssize_t written_count = write(fd, buffer, read_count);
        if (written_count < 0) {
            s2e_kill_state_printf(-1, "symbfile: could not write to file %s", filename);
            return;
        }

        if (read_count != written_count) {
            /* XXX: should probably retry...*/
            s2e_kill_state_printf(-1, "symbfile: could not write the read amount");
            return;
        }

        offset += read_count;
        size -= read_count;
        ++current_chunk;

    } while (size > 0);

    close(fd);
}

/* file is a path relative to the HostFile's base directory */
static int copy_file(const char *directory, const char *guest_file)
{

    char *path = malloc(strlen(directory) + strlen(guest_file) + 1 + 1);
    if (!path) {
        fprintf(stderr, "Could not allocate memory for file path\n");
        exit(1);
    }

    sprintf(path, "%s/%s", directory, "sample");

    unlink(path);

    if (mkdir(directory, S_IRWXU) < 0 && (errno != EEXIST)) {
        fprintf(stderr, "Could not create directory %s (%s)\n", directory,
                strerror(errno));
        exit(-1);
    }

#ifdef _WIN32
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC|O_BINARY, S_IRWXU);
#else
    int fd = creat(path, S_IRWXU);
#endif

    if (fd == -1) {
        fprintf(stderr, "cannot create file %s\n", path);
        exit(1);
    }

    if(is1sttime){
        while (!s2e_find(guest_file))
            SLEEP(0.0001); //FIXME: HACK here
        is1sttime = 0;
    }else
        s2e_wait_afl_testcase();

    int s2e_fd = s2e_open(guest_file);
    if (s2e_fd == -1) {
        fprintf(stderr, "s2e_open of %s failed\n", guest_file);
        exit(1);
    }
    int fsize = 0;
    char buf[1024 * 64] =
    { 0 };

    while (1) {
        int ret = s2e_read(s2e_fd, buf, sizeof(buf));
        if (ret == -1) {
            fprintf(stderr, "s2e_read failed\n");
            exit(1);
        } else if (ret == 0) {
            break;
        }

        int ret1 = write(fd, buf, ret);
        if (ret1 != ret) {
            fprintf(stderr, "can not write to file\n");
            exit(1);
        }

        fsize += ret;
    }

    s2e_close(s2e_fd);
    close(fd);

    free(path);

    return 0;
}

static int parse_arguments(int argc, const char **argv)
{
    unsigned i = 1;
    while (i < argc) {
        if (!strcmp(argv[i], "--s2etools-dir")) {
            if (++i >= argc) {
                return -1;
            }
            g_s2etools_dir = argv[i++];
            continue;
        } else if (!strcmp(argv[i], "--target-file")) {
            if (++i >= argc) {
                return -1;
            }
            g_target_file = argv[i++];
            continue;
        }else if (!strcmp(argv[i], "--need-symbolic")) {
            needSymwrite = 1;
            i++;
            continue;
        } else if (!strcmp(argv[i], "--target-code")) {
            if (++i >= argc) {
                return -1;
            }
            g_target_code = argv[i++];
            continue;
        } else if (!strcmp(argv[i], "--target-args")) {
            if (++i >= argc) {
                return -1;
            }
            int j = 0;
            while (i < argc) { // just copy the others
                if (!strcmp(argv[i], "@")) {
                    g_target_args[j++] = "/tmp/testcase/sample";
                    i++;
                } else
                    g_target_args[j++] = argv[i++];
                if (j > MAXARGNUM)
                    break;
            }
        } else
            i++;
    }

    return 0;
}
// s2eafl --target-code target_code --target-args target_arguments
int main(int argc, const char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        exit(1);
    }
    if (parse_arguments(argc, argv))
        exit(-1);
    // create Pipe and duplicate it
    int fd[2];
    if (pipe(fd) != 0)
        return -1;
    if (dup2(fd[1], AFLS2EGUESTPIPE + 1) < 0 || dup2(fd[0], AFLS2EGUESTPIPE) < 0)
        return -1;

    int child_fd[2];
    if (pipe(child_fd) != 0)
        return -1;
    if (dup2(child_fd[1], AFLS2EGUESTPIPE_CHILD + 1) < 0 || dup2(child_fd[0], AFLS2EGUESTPIPE_CHILD) < 0)
        return -1;

    // start up our target program as child, and let it wait for our order
    pid_t pid;
    if ((pid = fork()) == 0) {
        char cmd[1024];
        sprintf(cmd, "LD_PRELOAD=%s/init_env.so %s  --wait-afl ",
                g_s2etools_dir, g_target_code);
        int i = 0;
        while (i < MAXARGNUM) {
            if (!g_target_args[i])
                break;
            strcat(cmd, g_target_args[i++]);
            strcat(cmd, " ");
        }
        //strcat(cmd, "&"); // block mask
        system(cmd);
        s2e_tell_afl();
        s2e_kill_state(0, "done");

    } else if (pid > 0) {
        printf("Waiting for S2E mode...\n");
        while (s2e_version() == 0)
            /* nothing */;
        printf("... S2E mode detected\n");
        // Now let get into the main loop
        while (1) {
            copy_file("/tmp/testcase", g_target_file);
            s2e_fork_state();
            if (s2e_get_path_id())
                break;
            s2e_yield();
        }
        if (needSymwrite){
            symbfile("/tmp/testcase/sample");
        }
        printf("... let us execute our target.\n");
        char tmp[4] = "nudt";
        write(AFLS2EGUESTPIPE + 1, tmp, sizeof(tmp)); // wake our child up and go to work
        printf("... we have told our target.\n");
        return 0;
    }
}

