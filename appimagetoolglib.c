
#include <glib.h>
#include <stdlib.h>

#include <stdio.h>
#include <argp.h>

#include <stdlib.h>
#include <fcntl.h>
#include "squashfuse.h"

#include <sys/types.h>
#include <sys/stat.h>

#include "binreloc.h"
#ifndef NULL
#define NULL ((void *) 0)
#endif

#include <libgen.h>

#include <unistd.h>

#include <stdio.h>

extern int _binary_runtime_start;
extern int _binary_runtime_size;


static gint repeats = 2;
static gint max_size = 8;
static gboolean list = FALSE;
static gboolean verbose = FALSE;
gchar **remaining_args = NULL;

// #####################################################################

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

int is_directory(const char *path) {
    struct stat statbuf;
    if (stat(path, &statbuf) != 0)
        return 0;
    return S_ISDIR(statbuf.st_mode);
}

int is_regular_file(const char *path)
{
    struct stat path_stat;
    stat(path, &path_stat);
    return S_ISREG(path_stat.st_mode);
}

/* Function that prints the contents of a squashfs file
 * using libsquashfuse (#include "squashfuse.h") 
 * TODO: Implement offset */
int sfs_ls(char* image) {
    sqfs_err err = SQFS_OK;
    sqfs_traverse trv;
    sqfs fs;
    
    if ((err = sqfs_open_image(&fs, image, 0)))
        die("sqfs_open_image error, TODO: Implement offset");
    
    if ((err = sqfs_traverse_open(&trv, &fs, sqfs_inode_root(&fs))))
        die("sqfs_traverse_open error");
    while (sqfs_traverse_next(&trv, &err)) {
        if (!trv.dir_end) {
            printf("%s\n", trv.path);
        }
    }
    if (err)
        die("sqfs_traverse_next error");
    sqfs_traverse_close(&trv);
    
    sqfs_fd_close(fs.fd);
    return 0;
}

/* Generate a squashfs filesystem using mksquashfs on the $PATH  */
int sfs_mksquashfs(char *source, char *destination) {
    pid_t parent = getpid();
    pid_t pid = fork();
    
    if (pid == -1) {
        // error, failed to fork()
        return(-1);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    } else {
        // we are the child
        execlp("mksquashfs", "mksquashfs", source, destination, "-root-owned", "-noappend", (char *)0);
        perror("execlp");   // execvp() returns only on error
        return(-1); // exec never returns
    }
    return(0);
}

/* Generate a squashfs filesystem
 * The following would work if we link to mksquashfs.o after we renamed 
 * main() to mksquashfs_main() in mksquashfs.c but we don't want to actually do
 * this because squashfs-tools is not under a permissive license
 i *nt sfs_mksquashfs(char *source, char *destination) {
 char *child_argv[5];
 child_argv[0] = NULL;
 child_argv[1] = source;
 child_argv[2] = destination;
 child_argv[3] = "-root-owned";
 child_argv[4] = "-noappend";
 mksquashfs_main(5, child_argv);
 }
 */

// #####################################################################

static GOptionEntry entries[] =
{
    // { "repeats", 'r', 0, G_OPTION_ARG_INT, &repeats, "Average over N repetitions", "N" },
    { "list", 'l', 0, G_OPTION_ARG_NONE, &list, "List files in SOURCE AppImage", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose, "Produce verbose output", NULL },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &remaining_args, NULL },
    { NULL }
};

int
main (int argc, char *argv[])
{
    GError *error = NULL;
    GOptionContext *context;
    
    context = g_option_context_new ("SOURCE [DESTINATION] - Generate, extract, and inspect AppImages");
    g_option_context_add_main_entries (context, entries, NULL);
    // g_option_context_add_group (context, gtk_get_option_group (TRUE));
    if (!g_option_context_parse (context, &argc, &argv, &error))
    {
        g_print ("option parsing failed: %s\n", error->message);
        exit(1);
    }
    
    if(!&remaining_args[0])
        die("SOURCE is missing");
    
    /* If in list mode */
    if (list){
        sfs_ls(remaining_args[0]);
        exit(0);
    }
    
    /* If the first argument is a directory, then we assume that we should package it */
    if(is_directory(remaining_args[0])){
        char *destination;
        char source[PATH_MAX];
        realpath(remaining_args[0], source);
        if (remaining_args[1]) {
            destination = remaining_args[1];
        } else {
            /* No destination has been specified, to let's construct one
             * TODO: Find out the architecture and use a $VERSION that might be around in the env */
            destination = basename(br_strcat(source, ".AppImage"));
            fprintf (stdout, "DESTINATION not specified, so assuming %s\n", destination);
        }
        fprintf (stdout, "%s should be packaged as %s\n", remaining_args[0], destination);
        
        /* mksquashfs can currently not start writing at an offset,
         * so we need a tempfile. https://github.com/plougher/squashfs-tools/pull/13
         * should hopefully change that. */
        char *tempfile;
        fprintf (stderr, "Generating squashfs...\n");
        tempfile = br_strcat(destination, ".temp");
        int result = sfs_mksquashfs(source, tempfile);
        if(result != 0)
            die("sfs_mksquashfs error");
        
        fprintf (stderr, "Generating AppImage...\n");
        FILE *fpsrc = fopen(tempfile, "rb");
        if (fpsrc == NULL) {
            die("Not able to open the tempfile for reading, aborting");
        }
        FILE *fpdst = fopen(destination, "w");
        if (fpdst == NULL) {
            die("Not able to open the destination file for writing, aborting");
        }
        
        /* runtime is embedded into this executable
         * http://stupefydeveloper.blogspot.de/2008/08/cc-embed-binary-data-into-elf.html */
        int size = (int)&_binary_runtime_size;
        char *data = (char *)&_binary_runtime_start;
        if (verbose)
            printf("Size of the embedded runtime: %d bytes\n", size);
        /* Where to store updateinformation. Fixed offset preferred for easy manipulation 
         * after the fact. Proposal: 4 KB at the end of the 128 KB padding. 
         * Hence, offset 126976, max. 4096 bytes long. 
         * Possibly we might want to store additional information in the future.
         * Assuming 4 blocks of 4096 bytes each.
         */
        if(size > 128*1024-4*4096-2){
            die("Size of the embedded runtime is too large, aborting");
        }
        // printf("%s", data);
        fwrite(data, size, 1, fpdst);
        
        if(ftruncate(fileno(fpdst), 128*1024) != 0) {
            die("Not able to write padding to destination file, aborting");
        }
        
        fseek (fpdst, 0, SEEK_END);
        char byte;
        
        while (!feof(fpsrc))
        {
            fread(&byte, sizeof(char), 1, fpsrc);
            fwrite(&byte, sizeof(char), 1, fpdst);
        }
        
        fclose(fpsrc);
        fclose(fpdst);
        
        fprintf (stderr, "Marking the AppImage as executable...\n");
        if (chmod (destination, 0755) < 0) {
            printf("Could not set executable bit, aborting\n");
            exit(1);
        }
        if(unlink(tempfile) != 0) {
            die("Could not delete the tempfile, aborting");
        }
        fprintf (stderr, "Success\n");
    }
    
    /* If the first argument is a regular file, then we assume that we should unpack it */
    if(is_regular_file(remaining_args[0])){
        fprintf (stdout, "%s is a file, assuming it is an AppImage and should be unpacked\n", remaining_args[0]);
        die("To be implemented");
    }
    
    return 0;    
}