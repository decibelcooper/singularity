/*
 *
 * Copyright (c) 2015, Gregory M. Kurtzer
 * All rights reserved.
 *
 *
 * Copyright (c) 2015, The Regents of the University of California,
 * through Lawrence Berkeley National Laboratory (subject to receipt of
 * any required approvals from the U.S. Dept. of Energy).
 * All rights reserved.
 *
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <errno.h> 
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <fcntl.h>  
#include <grp.h>
#include "config.h"
#include "util.h"

#ifndef LIBEXECDIR
#define LIBEXECDIR "undefined"
#endif

// Yes, I know... Global variables suck but necessary to pass sig to child
pid_t child_pid = 0;


void sighandler(int sig) {
    signal(sig, sighandler);

    printf("Caught signal: %d\n", sig);
    fflush(stdout);

    if ( child_pid > 0 ) {
        printf("Sending SIGKILL to child pid: %d\n", child_pid);
        fflush(stdout);

        kill(child_pid, SIGKILL);
    }
}


int main(int argc, char **argv) {
    char *homepath;
    char *scratchpath;
    char *containerhomepath = '\0';
    char *containerscratchpath = '\0';
    char *containerpath;
    char *singularitypath;
    char *containerdevpath;
    char *containertmppath;
    char *containerprocpath;
    char cwd[PATH_MAX];
    int cwd_fd;
    int opt_contain = 0;
    int retval = 0;
    uid_t uid = getuid();
    gid_t gid = getgid();
    mode_t initial_umask = umask(0);


    //****************************************************************************//
    // Sanity
    //****************************************************************************//

    // We don't run as root!
    if ( uid == 0 || gid == 0 ) {
        fprintf(stderr, "ERROR: Do not run singularities as root!\n");
        return(255);
    }

    // Lets start off and confirm non-root
    if ( seteuid(uid) < 0 ) {
        fprintf(stderr, "ERROR: Could not set effective user privledges to %d!\n", uid);
        return(255);
    }

    // Check for SINGULARITY_CONTAIN environment variable
    if ( getenv("SINGULARITY_CONTAIN") != NULL ) {
        opt_contain = 1;
    }

    // Figure out where we start
    if ( (cwd_fd = open(".", O_RDONLY)) < 0 ) {
        fprintf(stderr, "ERROR: Could not open cwd fd (%s)!\n", strerror(errno));
        return(1);
    }
    if ( getcwd(cwd, PATH_MAX) == NULL ) {
        fprintf(stderr, "Could not obtain current directory path\n");
        return(1);
    }


    //****************************************************************************//
    // Sanity
    //****************************************************************************//

    // Get containerpath from the environment (we check on this shortly)
    containerpath = getenv("CONTAINERPATH");

    // Check CONTAINERPATH
    if ( containerpath == NULL ) {
        fprintf(stderr, "ERROR: CONTAINERPATH undefined!\n");
        return(1);
    }
    if ( s_is_dir(containerpath) < 0 ) {
        fprintf(stderr, "ERROR: Container path is not a directory: %s!\n", containerpath);
        return(1);
    }
    if ( s_is_owner(containerpath, uid) < 0 ) {
        fprintf(stderr, "ERROR: Will not execute in a CONTAINERPATH you don't own: %s\n", containerpath);
        return(255);
    }

    
    // Check the singularity within the CONTAINERPATH
    singularitypath = (char *) malloc(strlen(containerpath) + 13);
    snprintf(singularitypath, strlen(containerpath) + 13, "%s/singularity", containerpath);
    if ( s_is_file(singularitypath) < 0 ) {
        fprintf(stderr, "ERROR: The singularity is not found in CONTAINERPATH!\n");
        return(1);
    }
    if ( s_is_owner(singularitypath, uid) < 0 ) {
        fprintf(stderr, "ERROR: Will not execute a singularity you don't own: %s!\n", singularitypath);
        return(255);
    }
    if ( s_is_exec(singularitypath) < 0 ) {
        fprintf(stderr, "ERROR: The singularity can not be executed!\n");
        return(1);
    }


    //****************************************************************************//
    // Setup For Bind Mounts
    //****************************************************************************//

    // Populate paths for system bind mounts
    containerdevpath = (char *) malloc(strlen(containerpath) + 5);
    snprintf(containerdevpath, strlen(containerpath) + 5, "%s/dev", containerpath);
    containerprocpath = (char *) malloc(strlen(containerpath) + 6);
    snprintf(containerprocpath, strlen(containerpath) + 6, "%s/proc", containerpath);
    containertmppath = (char *) malloc(strlen(containerpath) + 5);
    snprintf(containertmppath, strlen(containerpath) + 5, "%s/tmp", containerpath);

    // Create system directories as neccessary
    if ( s_is_dir(containerprocpath) < 0 ) {
        if ( s_mkpath(containerprocpath, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IXOTH) > 0 ) {
            fprintf(stderr, "ERROR: Could not create directory %s\n", containerprocpath);
            return(255);
        }
    }
    if ( s_is_dir(containerdevpath) < 0 ) {
        if ( s_mkpath(containerdevpath, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IXOTH) > 0 ) {
            fprintf(stderr, "ERROR: Could not create directory %s\n", containerdevpath);
            return(255);
        }
    }
    if ( s_is_dir(containertmppath) < 0 ) {
        if ( s_mkpath(containertmppath, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IXOTH) > 0 ) {
            fprintf(stderr, "ERROR: Could not create directory %s\n", containertmppath);
            return(255);
        }
    }


    // Get the home path from the environment and setup
    homepath = getenv("HOME");
    if ( homepath != NULL ) {
        containerhomepath = (char *) malloc(strlen(containerpath) + strlen(homepath) + 1);
        snprintf(containerhomepath, strlen(containerpath) + strlen(homepath) + 1, "%s%s", containerpath, homepath);
        if ( s_is_dir(homepath) == 0 ) {
            if ( s_is_dir(containerhomepath) < 0 ) {
                if ( s_mkpath(containerhomepath, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IXOTH) > 0 ) {
                    fprintf(stderr, "ERROR: Could not create directory %s\n", homepath);
                    return(255);
                }
            }
        } else {
            fprintf(stderr, "WARNING: Could not locate your home directory (%s), not linking to container.\n", homepath);
            homepath = NULL;
        }
    } else {
        fprintf(stderr, "WARNING: Could not obtain your home directory path, not linking to container.\n");
    }


    // Get the scratch path from the environment and setup
    scratchpath = getenv("SINGULARITY_SCRATCH");
    if ( scratchpath != NULL ) {
        if ( ( strncmp(homepath, scratchpath, strlen(homepath)) == 0 ) || ( strncmp(homepath, scratchpath, strlen(scratchpath)) == 0 ) ) {
            fprintf(stderr, "ERROR: Overlapping paths (scratch and home)!\n");
            return(255);
        }
        if ( strncmp(scratchpath, "/lib", 4) == 0 ) {
            fprintf(stderr, "ERROR: Can not link scratch directory over /lib\n");
            return(255);
        }
        if ( strncmp(scratchpath, "/bin", 4) == 0 ) {
            fprintf(stderr, "ERROR: Can not link scratch directory over /bin\n");
            return(255);
        }
        if ( strncmp(scratchpath, "/sbin", 4) == 0 ) {
            fprintf(stderr, "ERROR: Can not link scratch directory over /sbin\n");
            return(255);
        }
        if ( strncmp(scratchpath, "/etc", 4) == 0 ) {
            fprintf(stderr, "ERROR: Can not link scratch directory over /etc\n");
            return(255);
        }

        containerscratchpath = (char *) malloc(strlen(containerpath) + strlen(scratchpath) + 1);
        snprintf(containerscratchpath, strlen(containerpath) + strlen(scratchpath) + 1, "%s%s", containerpath, scratchpath);
        if ( s_is_dir(scratchpath) == 0 ) {
            if ( s_is_dir(containerscratchpath) < 0 ) {
                if ( s_mkpath(containerscratchpath, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IXOTH) > 0 ) {
                    fprintf(stderr, "ERROR: Could not create directory %s\n", scratchpath);
                    return(255);
                }
            }
        } else {
            fprintf(stderr, "WARNING: Could not locate your scratch directory (%s), not linking to container.\n", scratchpath);
            scratchpath = NULL;
        }
    }


    // Reset umask back to where we have started
    umask(initial_umask);


    //****************************************************************************//
    // Do root bits
    //****************************************************************************//

    // Entering danger zone
    if ( seteuid(0) < 0 ) {
        fprintf(stderr, "ERROR: Could not escalate effective user privledges!\n");
        return(255);
    }

    // Separate out the appropriate namespaces
    if ( unshare(CLONE_NEWPID | CLONE_NEWNS | CLONE_FS | CLONE_FILES) < 0 ) {
        fprintf(stderr, "ERROR: Could not create virtulized namespaces\n");
        return(255);
    }

    // Privitize the mount namespaces (thank you for the pointer Doug Jacobsen!)
    if ( mount(NULL, "/", NULL, MS_PRIVATE|MS_REC, NULL) < 0 ) {
        // I am not sure if this error needs to be caught, maybe it will fail
        // on older kernels? If so, we can fix then.
        fprintf(stderr, "ERROR: Could not make mountspaces private\n");
        return(255);
    }

    // Mount /dev
    if ( mount("/dev", containerdevpath, NULL, MS_BIND, NULL) < 0 ) {
        fprintf(stderr, "ERROR: Could not bind mount /dev\n");
        return(255);
    }

    // Mount any other file systems
    if ( opt_contain == 0 ) {
        if ( scratchpath != NULL ) {
            if ( mount(scratchpath, containerscratchpath, NULL, MS_BIND, NULL) < 0 ) {
                fprintf(stderr, "ERROR: Could not bind mount %s\n", scratchpath);
                return(255);
            }
        }
        if ( homepath != NULL ) {
            if ( mount(homepath, containerhomepath, NULL, MS_BIND, NULL) < 0 ) {
                fprintf(stderr, "ERROR: Could not bind mount %s\n", homepath);
                return(255);
            }
        }
        if ( mount("/tmp", containertmppath, NULL, MS_BIND, NULL) < 0 ) {
            fprintf(stderr, "ERROR: Could not bind mount %s\n", containertmppath);
            return(255);
        }
    }


    // Recheck to see if we can stat the singularitypath as root
    // This fails when home is exported with root_squash enabled
    if ( s_is_exec(singularitypath) < 0 ) {
        fprintf(stderr, "ERROR: Could not stat %s as root!\n", singularitypath);
        fprintf(stderr, "NOTE:  This maybe caused by root_squash on NFS, set environment\n");
        fprintf(stderr, "NOTE:  variable 'SINGULARITY_CACHEDIR' and point to a different\n");
        fprintf(stderr, "NOTE:  file system. For excample:\n\n");
        fprintf(stderr, "NOTE:  SINGULARITY_CACHEDIR\"=/var/tmp/singularity.`uid -u`\"\n");
        fprintf(stderr, "NOTE:  export SINGULARITY_CACHEDIR\n\n");
        return(1);
    }

    // Drop privledges for fork and parent
    if ( seteuid(uid) < 0 ) {
        fprintf(stderr, "ERROR: Could not drop effective user privledges!\n");
        return(255);
    }

    child_pid = fork();

    if ( child_pid == 0 ) {

        // Root needed for chroot and /proc mount
        if ( seteuid(0) < 0 ) {
            fprintf(stderr, "ERROR: Could not re-escalate effective user privledges!\n");
            return(255);
        }

        // Do the chroot
        if ( chroot(containerpath) < 0 ) {
            fprintf(stderr, "ERROR: failed enter CONTAINERPATH: %s\n", containerpath);
            return(255);
        }

        // Mount up /proc
        if ( mount("proc", "/proc", "proc", 0, NULL) < 0 ) {
            fprintf(stderr, "ERROR: Could not bind mount /proc\n");
            return(255);
        }

        // Dump all privs permanently for this process
        if ( setregid(gid, gid) < 0 ) {
            fprintf(stderr, "ERROR: Could not dump real and effective group privledges!\n");
            return(255);
        }
        if ( setreuid(uid, uid) < 0 ) {
            fprintf(stderr, "ERROR: Could not dump real and effective user privledges!\n");
            return(255);
        }

        // Confirm we no longer have any escalated privledges whatsoever
        if ( setuid(0) == 0 ) {
            fprintf(stderr, "ERROR: Root not allowed here!\n");
            return(1);
        }

        // change directory back to starting point if needed
        if ( opt_contain > 0 ) {
            if ( chdir("/") < 0 ) {
                fprintf(stderr, "ERROR: Could not changedir to /\n");
                return(1);
            }
        } else {
            if (strncmp(homepath, cwd, strlen(homepath)) == 0 ) {
                if ( chdir(cwd) < 0 ) {
                    fprintf(stderr, "ERROR: Could not fchdir!\n");
                    return(1);
                }
            } else {
                if ( fchdir(cwd_fd) < 0 ) {
                    fprintf(stderr, "ERROR: Could not fchdir!\n");
                    return(1);
                }
            }
        }

        // Exec the singularity
        if ( execv("/singularity", argv) < 0 ) {
            fprintf(stderr, "ERROR: Failed to exec SAPP envrionment\n");
            return(2);
        }

    } else if ( child_pid > 0 ) {
        int tmpstatus;
        signal(SIGINT, sighandler);
        signal(SIGKILL, sighandler);
        signal(SIGQUIT, sighandler);

        waitpid(child_pid, &tmpstatus, 0);
        retval = WEXITSTATUS(tmpstatus);
    } else {
        fprintf(stderr, "ERROR: Could not fork child process\n");
        retval++;
    }

    if ( close(cwd_fd) < 0 ) {
        fprintf(stderr, "ERROR: Could not close cwd_fd!\n");
        retval++;
    }

    return(retval);
}
