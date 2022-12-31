/* Android initialization for GNU Emacs.

Copyright (C) 2023 Free Software Foundation, Inc.

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <limits.h>
#include <signal.h>
#include <semaphore.h>

#include <sys/stat.h>
#include <sys/mman.h>

#include <assert.h>

#include "android.h"
#include "androidgui.h"

#include "lisp.h"
#include "blockinput.h"
#include "coding.h"
#include "epaths.h"

/* Whether or not Emacs is running inside the application process and
   Android windowing should be enabled.  */
bool android_init_gui;

#ifndef ANDROID_STUBIFY

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <linux/ashmem.h>

#define ANDROID_THROW(env, class, msg)					\
  ((*(env))->ThrowNew ((env), (*(env))->FindClass ((env), class), msg))

#define ANDROID_MAX_ASSET_FD 65535

struct android_fd_table_entry
{
  /* Various flags associated with this table.  */
  short flags;

  /* The stat buffer associated with this entry.  */
  struct stat statb;
};

enum android_fd_table_entry_flags
  {
    ANDROID_FD_TABLE_ENTRY_IS_VALID = 1,
  };

struct android_emacs_service
{
  jclass class;
  jmethodID fill_rectangle;
  jmethodID fill_polygon;
  jmethodID draw_rectangle;
  jmethodID draw_line;
  jmethodID draw_point;
  jmethodID copy_area;
  jmethodID clear_window;
  jmethodID clear_area;
};

struct android_emacs_pixmap
{
  jclass class;
  jmethodID constructor;
};

struct android_graphics_point
{
  jclass class;
  jmethodID constructor;
};

/* The asset manager being used.  */
static AAssetManager *asset_manager;

/* Whether or not Emacs has been initialized.  */
static int emacs_initialized;

/* The path used to store site-lisp.  */
char *android_site_load_path;

/* The path used to store native libraries.  */
char *android_lib_dir;

/* The Android application data directory.  */
static char *android_files_dir;

/* Array of structures used to hold asset information corresponding to
   a file descriptor.  This assumes Emacs does not do funny things
   with dup.  It currently does not.  */
static struct android_fd_table_entry android_table[ANDROID_MAX_ASSET_FD];

/* The Java environment being used for the main thread.  */
JNIEnv *android_java_env;

/* The EmacsGC class.  */
static jclass emacs_gc_class;

/* Various fields.  */
static jfieldID emacs_gc_foreground, emacs_gc_background;
static jfieldID emacs_gc_function, emacs_gc_clip_rects;
static jfieldID emacs_gc_clip_x_origin, emacs_gc_clip_y_origin;
static jfieldID emacs_gc_stipple, emacs_gc_clip_mask;
static jfieldID emacs_gc_fill_style, emacs_gc_ts_origin_x;
static jfieldID emacs_gc_ts_origin_y;

/* The constructor and one function.  */
static jmethodID emacs_gc_constructor, emacs_gc_mark_dirty;

/* The Rect class.  */
static jclass android_rect_class;

/* Its constructor.  */
static jmethodID android_rect_constructor;

/* The EmacsService object.  */
static jobject emacs_service;

/* Various methods associated with the EmacsService.  */
static struct android_emacs_service service_class;

/* Various methods associated with the EmacsPixmap class.  */
static struct android_emacs_pixmap pixmap_class;

/* Various methods associated with the Point class.  */
static struct android_graphics_point point_class;



/* Event handling functions.  Events are stored on a (circular) queue
   that is read synchronously.  The Android port replaces pselect with
   a function android_select, which runs pselect in a separate thread,
   but more importantly also waits for events to be available on the
   android event queue.  */

struct android_event_container
{
  /* The next and last events in this queue.  */
  struct android_event_container *volatile next, *last;

  /* The event itself.  */
  union android_event event;
};

struct android_event_queue
{
  /* Mutex protecting the event queue.  */
  pthread_mutex_t mutex;

  /* Mutex protecting the select data.  */
  pthread_mutex_t select_mutex;

  /* The thread used to run select.  */
  pthread_t select_thread;

  /* Condition variable for the writing side.  */
  pthread_cond_t write_var;

  /* Condition variables for the reading side.  */
  pthread_cond_t read_var;

  /* The number of events in the queue.  If this is greater than 1024,
     writing will block.  */
  volatile int num_events;

  /* Circular queue of events.  */
  struct android_event_container events;
};

/* Arguments to pselect used by the select thread.  */
static volatile int android_pselect_nfds;
static fd_set *volatile android_pselect_readfds;
static fd_set *volatile android_pselect_writefds;
static fd_set *volatile android_pselect_exceptfds;
static struct timespec *volatile android_pselect_timeout;
static const sigset_t *volatile android_pselect_sigset;

/* Value of pselect.  */
static int android_pselect_rc;

/* Whether or not pselect finished.  */
static volatile bool android_pselect_completed;

/* The global event queue.  */
static struct android_event_queue event_queue;

/* Semaphore used to signal select completion.  */
static sem_t android_pselect_sem;

static void *
android_run_select_thread (void *data)
{
  sigset_t signals;
  int sig, rc;

  sigfillset (&signals);

  if (pthread_sigmask (SIG_BLOCK, &signals, NULL))
    __android_log_print (ANDROID_LOG_FATAL, __func__,
			 "pthread_sigmask: %s",
			 strerror (errno));

  sigemptyset (&signals);
  sigaddset (&signals, SIGUSR1);

  if (pthread_sigmask (SIG_UNBLOCK, &signals, NULL))
    __android_log_print (ANDROID_LOG_FATAL, __func__,
			 "pthread_sigmask: %s",
			 strerror (errno));

  sigemptyset (&signals);
  sigaddset (&signals, SIGUSR2);

  while (true)
    {
      /* Keep waiting for SIGUSR2, ignoring EINTR in the meantime.  */

      while (sigwait (&signals, &sig))
	/* Spin.  */;

      /* Get the select lock and call pselect.  */
      pthread_mutex_lock (&event_queue.select_mutex);
      rc = pselect (android_pselect_nfds,
		    android_pselect_readfds,
		    android_pselect_writefds,
		    android_pselect_exceptfds,
		    android_pselect_timeout,
		    android_pselect_sigset);
      android_pselect_rc = rc;
      pthread_mutex_unlock (&event_queue.select_mutex);

      /* Signal the Emacs thread that pselect is done.  If read_var
	 was signaled by android_write_event, event_queue.mutex could
	 still be locked, so this must come before.  */
      sem_post (&android_pselect_sem);

      pthread_mutex_lock (&event_queue.mutex);
      android_pselect_completed = true;
      pthread_cond_signal (&event_queue.read_var);
      pthread_mutex_unlock (&event_queue.mutex);
    }
}

static void
android_handle_sigusr1 (int sig, siginfo_t *siginfo, void *arg)
{
  /* Nothing to do here, this signal handler is only installed to make
     sure the disposition of SIGUSR1 is enough.  */
}

/* Set up the global event queue by initializing the mutex and two
   condition variables, and the linked list of events.  This must be
   called before starting the Emacs thread.  Also, initialize the
   thread used to run pselect.

   These functions must also use the C library malloc and free,
   because xmalloc is not thread safe.  */

static void
android_init_events (void)
{
  struct sigaction sa;

  if (pthread_mutex_init (&event_queue.mutex, NULL))
    __android_log_print (ANDROID_LOG_FATAL, __func__,
			 "pthread_mutex_init: %s",
			 strerror (errno));

  if (pthread_mutex_init (&event_queue.select_mutex, NULL))
    __android_log_print (ANDROID_LOG_FATAL, __func__,
			 "pthread_mutex_init: %s",
			 strerror (errno));

  if (pthread_cond_init (&event_queue.write_var, NULL))
    __android_log_print (ANDROID_LOG_FATAL, __func__,
			 "pthread_cond_init: %s",
			 strerror (errno));

  if (pthread_cond_init (&event_queue.read_var, NULL))
    __android_log_print (ANDROID_LOG_FATAL, __func__,
			 "pthread_cond_init: %s",
			 strerror (errno));

  sem_init (&android_pselect_sem, 0, 0);

  event_queue.events.next = &event_queue.events;
  event_queue.events.last = &event_queue.events;

  /* Before starting the select thread, make sure the disposition for
     SIGUSR1 is correct.  */
  sigfillset (&sa.sa_mask);
  sa.sa_sigaction = android_handle_sigusr1;
  sa.sa_flags = SA_SIGINFO;

  if (sigaction (SIGUSR1, &sa, NULL))
    __android_log_print (ANDROID_LOG_FATAL, __func__,
			 "sigaction: %s",
			 strerror (errno));

  /* Start the select thread.  */
  if (pthread_create (&event_queue.select_thread, NULL,
		      android_run_select_thread, NULL))
    __android_log_print (ANDROID_LOG_FATAL, __func__,
			 "pthread_create: %s",
			 strerror (errno));
}

int
android_pending (void)
{
  int i;

  pthread_mutex_lock (&event_queue.mutex);
  i = event_queue.num_events;
  pthread_mutex_unlock (&event_queue.mutex);

  return i;
}

void
android_next_event (union android_event *event_return)
{
  struct android_event_container *container;

  pthread_mutex_lock (&event_queue.mutex);

  /* Wait for events to appear if there are none available to
     read.  */
  if (!event_queue.num_events)
    pthread_cond_wait (&event_queue.read_var,
		       &event_queue.mutex);

  /* Obtain the event from the end of the queue.  */
  container = event_queue.events.last;
  eassert (container != &event_queue.events);

  /* Remove the event from the queue and copy it to the caller
     supplied buffer.  */
  container->last->next = container->next;
  container->next->last = container->last;
  *event_return = container->event;
  event_queue.num_events--;

  /* Free the container.  */
  free (container);

  /* Signal that events can now be written.  */
  pthread_cond_signal (&event_queue.write_var);
  pthread_mutex_unlock (&event_queue.mutex);
}

static void
android_write_event (union android_event *event)
{
  struct android_event_container *container;

  container = malloc (sizeof *container);

  if (!container)
    return;

  pthread_mutex_lock (&event_queue.mutex);

  /* The event queue is full, wait for events to be read.  */
  if (event_queue.num_events >= 1024)
    pthread_cond_wait (&event_queue.write_var,
		       &event_queue.mutex);

  container->next = event_queue.events.next;
  container->last = &event_queue.events;
  container->next->last = container;
  container->last->next = container;
  container->event = *event;
  event_queue.num_events++;
  pthread_cond_signal (&event_queue.read_var);
  pthread_mutex_unlock (&event_queue.mutex);
}

int
android_select (int nfds, fd_set *readfds, fd_set *writefds,
		fd_set *exceptfds, struct timespec *timeout,
		const sigset_t *sigset)
{
  int nfds_return;

  pthread_mutex_lock (&event_queue.mutex);

  if (event_queue.num_events)
    {
      pthread_mutex_unlock (&event_queue.mutex);
      return 1;
    }

  nfds_return = 0;
  android_pselect_completed = false;

  pthread_mutex_lock (&event_queue.select_mutex);
  android_pselect_nfds = nfds;
  android_pselect_readfds = readfds;
  android_pselect_writefds = writefds;
  android_pselect_exceptfds = exceptfds;
  android_pselect_timeout = timeout;
  android_pselect_sigset = sigset;
  pthread_mutex_unlock (&event_queue.select_mutex);

  pthread_kill (event_queue.select_thread, SIGUSR2);
  pthread_cond_wait (&event_queue.read_var, &event_queue.mutex);

  /* Interrupt the select thread now, in case it's still in
     pselect.  */
  pthread_kill (event_queue.select_thread, SIGUSR1);

  /* Wait for pselect to return in any case.  */
  sem_wait (&android_pselect_sem);

  /* If there are now events in the queue, return 1.  */
  if (event_queue.num_events)
    nfds_return = 1;

  /* Add the return value of pselect.  */
  if (android_pselect_rc >= 0)
    nfds_return += android_pselect_rc;

  if (!nfds_return && android_pselect_rc < 0)
    nfds_return = android_pselect_rc;

  /* Unlock the event queue mutex.  */
  pthread_mutex_unlock (&event_queue.mutex);

  return nfds_return;
}



static void *
android_run_debug_thread (void *data)
{
  FILE *file;
  int fd;
  char *line;
  size_t n;

  fd = (int) data;
  file = fdopen (fd, "r");

  if (!file)
    return NULL;

  line = NULL;

  while (true)
    {
      if (getline (&line, &n, file) < 0)
	{
	  free (line);
	  break;
	}

      __android_log_print (ANDROID_LOG_INFO, __func__, "%s", line);
    }

  fclose (file);
  return NULL;
}



/* Intercept USER_FULL_NAME and return something that makes sense if
   pw->pw_gecos is NULL.  */

char *
android_user_full_name (struct passwd *pw)
{
  if (!pw->pw_gecos)
    return (char *) "Android user";

  return pw->pw_gecos;
}

/* Given a real file name, return the part that describes its asset
   path, or NULL if it is not an asset.  */

static const char *
android_get_asset_name (const char *filename)
{
  if (!strcmp (filename, "/assets") || !strcmp (filename, "/assets/"))
    return "/";

  if (!strncmp (filename, "/assets/", sizeof "/assets/" - 1))
    return filename + (sizeof "/assets/" - 1);

  return NULL;
}

/* Like fstat.  However, look up the asset corresponding to the file
   descriptor.  If it exists, return the right information.  */

int
android_fstat (int fd, struct stat *statb)
{
  if (fd < ANDROID_MAX_ASSET_FD
      && (android_table[fd].flags
	  & ANDROID_FD_TABLE_ENTRY_IS_VALID))
    {
      memcpy (statb, &android_table[fd].statb,
	      sizeof *statb);
      return 0;
    }

  return fstat (fd, statb);
}

/* Like fstatat.  However, if dirfd is AT_FDCWD and PATHNAME is an
   asset, find the information for the corresponding asset.  */

int
android_fstatat (int dirfd, const char *restrict pathname,
		 struct stat *restrict statbuf, int flags)
{
  AAsset *asset_desc;
  const char *asset;

  if (dirfd == AT_FDCWD
      && asset_manager
      && (asset = android_get_asset_name (pathname)))
    {
      /* AASSET_MODE_STREAMING is fastest here.  */
      asset_desc = AAssetManager_open (asset_manager, asset,
				       AASSET_MODE_STREAMING);

      if (!asset_desc)
	return ENOENT;

      memset (statbuf, 0, sizeof *statbuf);

      /* Fill in the stat buffer.  */
      statbuf->st_mode = S_IFREG;
      statbuf->st_size = AAsset_getLength (asset_desc);

      /* Close the asset.  */
      AAsset_close (asset_desc);
      return 0;
    }

  return fstatat (dirfd, pathname, statbuf, flags);
}

/* Return if NAME is a file that is actually an asset and is
   accessible, as long as !(amode & W_OK).  */

bool
android_file_access_p (const char *name, int amode)
{
  AAsset *asset;
  AAssetDir *directory;

  if (!asset_manager)
    return false;

  if (!(amode & W_OK) && (name = android_get_asset_name (name)))
    {
      /* Check if the asset exists by opening it.  Suboptimal! */
      asset = AAssetManager_open (asset_manager, name,
				  AASSET_MODE_UNKNOWN);

      if (!asset)
	{
	  /* See if it's a directory also.  */
	  directory = AAssetManager_openDir (asset_manager, name);

	  if (directory)
	    {
	      AAssetDir_close (directory);
	      return true;
	    }

	  return false;
	}

      AAsset_close (asset);
      return true;
    }

  return false;
}

/* Get a file descriptor backed by a temporary in-memory file for the
   given asset.  */

static int
android_hack_asset_fd (AAsset *asset)
{
  int fd, rc;
  unsigned char *mem;
  size_t size;

  fd = open ("/dev/ashmem", O_RDWR);

  if (fd < 0)
    return -1;

  /* Assets must be small enough to fit in size_t, if off_t is
     larger.  */
  size = AAsset_getLength (asset);

  /* An empty name means the memory area will exist until the file
     descriptor is closed, because no other process can attach.  */
  rc = ioctl (fd, ASHMEM_SET_NAME, "");

  if (rc < 0)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "ioctl ASHMEM_SET_NAME: %s",
			   strerror (errno));
      close (fd);
      return -1;
    }

  rc = ioctl (fd, ASHMEM_SET_SIZE, size);

  if (rc < 0)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "ioctl ASHMEM_SET_SIZE: %s",
			   strerror (errno));
      close (fd);
      return -1;
    }

  if (!size)
    return fd;

  /* Now map the resource.  */
  mem = mmap (NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "mmap: %s", strerror (errno));
      close (fd);
      return -1;
    }

  if (AAsset_read (asset, mem, size) != size)
    {
      /* Too little was read.  Close the file descriptor and report an
	 error.  */
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "AAsset_read: %s", strerror (errno));
      close (fd);
      return -1;
    }

  /* Return anyway even if munmap fails.  */
  munmap (mem, size);
  return fd;
}

/* `open' and such are modified even though they exist on Android,
   because Emacs treats "/assets/" as a special directory that must
   contain all assets in the application package.  */

int
android_open (const char *filename, int oflag, int mode)
{
  const char *name;
  AAsset *asset;
  int fd;
  off_t out_start, out_length;
  bool fd_hacked;

  /* This flag means whether or not fd should not be duplicated.  */
  fd_hacked = false;

  if (asset_manager && (name = android_get_asset_name (filename)))
    {
      /* If Emacs is trying to write to the file, return NULL.  */

      if (oflag & O_WRONLY || oflag & O_RDWR)
	{
	  errno = EROFS;
	  return -1;
	}

      if (oflag & O_DIRECTORY)
	{
	  errno = EINVAL;
	  return -1;
	}

      asset = AAssetManager_open (asset_manager, name,
				  AASSET_MODE_BUFFER);

      if (!asset)
	{
	  errno = ENOENT;
	  return -1;
	}

      /* Try to obtain the file descriptor corresponding to this
	 asset.  */
      fd = AAsset_openFileDescriptor (asset, &out_start,
				      &out_length);

      if (fd == -1)
	{
	  /* The asset can't be accessed for some reason.  Try to
	     create a shared memory file descriptor.  */
	  fd = android_hack_asset_fd (asset);

	  if (fd == -1)
	    {
	      AAsset_close (asset);
	      errno = ENXIO;
	      return -1;
	    }

	  fd_hacked = true;
	}

      /* Duplicate the file descriptor and then close the asset,
	 which will close the original file descriptor.  */

      if (!fd_hacked)
	fd = fcntl (fd, F_DUPFD_CLOEXEC);

      if (fd >= ANDROID_MAX_ASSET_FD || fd < 0)
	{
	  /* Too bad.  You lose.  */
	  errno = ENOMEM;

	  if (fd >= 0)
	    close (fd);

	  fd = -1;
	}
      else
	{
	  assert (!(android_table[fd].flags
		    & ANDROID_FD_TABLE_ENTRY_IS_VALID));
	  android_table[fd].flags = ANDROID_FD_TABLE_ENTRY_IS_VALID;
	  memset (&android_table[fd].statb, 0,
		  sizeof android_table[fd].statb);

	  /* Fill in some information that will be reported to
	     callers of android_fstat, among others.  */
	  android_table[fd].statb.st_mode = S_IFREG;

	  /* Owned by root.  */
	  android_table[fd].statb.st_uid = 0;
	  android_table[fd].statb.st_gid = 0;

	  /* Size of the file.  */
	  android_table[fd].statb.st_size
	    = AAsset_getLength (asset);
	}

      AAsset_close (asset);
      return fd;
    }

  return open (filename, oflag, mode);
}

/* Like close.  However, remove the file descriptor from the asset
   table as well.  */

int
android_close (int fd)
{
  if (fd < ANDROID_MAX_ASSET_FD
      && (android_table[fd].flags & ANDROID_FD_TABLE_ENTRY_IS_VALID))
    {
      __android_log_print (ANDROID_LOG_INFO, __func__,
			   "closing android file descriptor %d",
			   fd);
      android_table[fd].flags = 0;
    }

  return close (fd);
}



/* JNI functions called by Java.  */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-prototypes"

JNIEXPORT void JNICALL
NATIVE_NAME (setEmacsParams) (JNIEnv *env, jobject object,
			      jobject local_asset_manager,
			      jobject files_dir, jobject libs_dir,
			      jobject emacs_service_object)
{
  int pipefd[2];
  pthread_t thread;
  const char *java_string;

  /* This may be called from multiple threads.  setEmacsParams should
     only ever be called once.  */
  if (__atomic_fetch_add (&emacs_initialized, -1, __ATOMIC_RELAXED))
    {
      ANDROID_THROW (env, "java/lang/IllegalArgumentException",
		     "Emacs was already initialized!");
      return;
    }

  __android_log_print (ANDROID_LOG_INFO, __func__,
		       "Initializing "PACKAGE_STRING"...\nPlease report bugs to "
		       PACKAGE_BUGREPORT".  Thanks.\n");

  /* Set the asset manager.  */
  asset_manager = AAssetManager_fromJava (env, local_asset_manager);

  /* Hold a VM reference to the asset manager to prevent the native
     object from being deleted.  */
  (*env)->NewGlobalRef (env, local_asset_manager);

  /* Create a pipe and duplicate it to stdout and stderr.  Next, make
     a thread that prints stderr to the system log.  */

  if (pipe2 (pipefd, O_CLOEXEC) < 0)
    emacs_abort ();

  if (dup2 (pipefd[1], 2) < 0)
    emacs_abort ();
  close (pipefd[1]);

  if (pthread_create (&thread, NULL, android_run_debug_thread,
		      (void *) pipefd[0]))
    emacs_abort ();

  /* Now set the path to the site load directory.  */

  java_string = (*env)->GetStringUTFChars (env, (jstring) files_dir,
					   NULL);

  if (!java_string)
    emacs_abort ();

  android_files_dir = strdup ((const char *) java_string);

  if (!android_files_dir)
    emacs_abort ();

  (*env)->ReleaseStringUTFChars (env, (jstring) files_dir,
				 java_string);

  java_string = (*env)->GetStringUTFChars (env, (jstring) libs_dir,
					   NULL);

  if (!java_string)
    emacs_abort ();

  android_lib_dir = strdup ((const char *) java_string);

  if (!android_files_dir)
    emacs_abort ();

  (*env)->ReleaseStringUTFChars (env, (jstring) libs_dir,
				 java_string);

  /* Calculate the site-lisp path.  */

  android_site_load_path = malloc (PATH_MAX + 1);

  if (!android_site_load_path)
    emacs_abort ();

  snprintf (android_site_load_path, PATH_MAX, "%s/site-lisp",
	    android_files_dir);
  __android_log_print (ANDROID_LOG_INFO, __func__,
		       "Site-lisp directory: %s\n"
		       "Files directory: %s\n"
		       "Native code directory: %s",
		       android_site_load_path,
		       android_files_dir,
		       android_lib_dir);

  /* Make a reference to the Emacs service.  */
  emacs_service = (*env)->NewGlobalRef (env, emacs_service_object);

  if (!emacs_service)
    emacs_abort ();

  /* Set up events.  */
  android_init_events ();

  /* OK, setup is now complete.  The caller may start the Emacs thread
     now.  */
}

/* Initialize service_class, aborting if something goes wrong.  */

static void
android_init_emacs_service (void)
{
  jclass old;

  service_class.class
    = (*android_java_env)->FindClass (android_java_env,
				      "org/gnu/emacs/EmacsService");
  eassert (service_class.class);

  old = service_class.class;
  service_class.class
    = (jclass) (*android_java_env)->NewGlobalRef (android_java_env,
						  (jobject) old);
  ANDROID_DELETE_LOCAL_REF (old);

  if (!service_class.class)
    emacs_abort ();

#define FIND_METHOD(c_name, name, signature)			\
  service_class.c_name						\
    = (*android_java_env)->GetMethodID (android_java_env,	\
					service_class.class,	\
					name, signature);	\
  assert (service_class.c_name);

  FIND_METHOD (fill_rectangle, "fillRectangle",
	       "(Lorg/gnu/emacs/EmacsDrawable;"
	       "Lorg/gnu/emacs/EmacsGC;IIII)V");
  FIND_METHOD (fill_polygon, "fillPolygon",
	       "(Lorg/gnu/emacs/EmacsDrawable;"
	       "Lorg/gnu/emacs/EmacsGC;"
	       "[Landroid/graphics/Point;)V");
  FIND_METHOD (draw_rectangle, "drawRectangle",
	       "(Lorg/gnu/emacs/EmacsDrawable;"
	       "Lorg/gnu/emacs/EmacsGC;IIII)V");
  FIND_METHOD (draw_line, "drawLine",
	       "(Lorg/gnu/emacs/EmacsDrawable;"
	       "Lorg/gnu/emacs/EmacsGC;IIII)V");
  FIND_METHOD (draw_point, "drawPoint",
	       "(Lorg/gnu/emacs/EmacsDrawable;"
	       "Lorg/gnu/emacs/EmacsGC;II)V");
  FIND_METHOD (copy_area, "copyArea",
	       "(Lorg/gnu/emacs/EmacsDrawable;"
	       "Lorg/gnu/emacs/EmacsDrawable;"
	       "Lorg/gnu/emacs/EmacsGC;IIIIII)V");
  FIND_METHOD (clear_window, "clearWindow",
	       "(Lorg/gnu/emacs/EmacsWindow;)V");
  FIND_METHOD (clear_area, "clearArea",
	       "(Lorg/gnu/emacs/EmacsWindow;IIII)V");

#undef FIND_METHOD
}

static void
android_init_emacs_pixmap (void)
{
  jclass old;

  pixmap_class.class
    = (*android_java_env)->FindClass (android_java_env,
				      "org/gnu/emacs/EmacsPixmap");
  eassert (pixmap_class.class);

  old = pixmap_class.class;
  pixmap_class.class
    = (jclass) (*android_java_env)->NewGlobalRef (android_java_env,
						  (jobject) old);
  ANDROID_DELETE_LOCAL_REF (old);

  if (!pixmap_class.class)
    emacs_abort ();

#define FIND_METHOD(c_name, name, signature)			\
  pixmap_class.c_name						\
    = (*android_java_env)->GetMethodID (android_java_env,	\
					pixmap_class.class,	\
					name, signature);	\
  assert (pixmap_class.c_name);

  FIND_METHOD (constructor, "<init>", "(S[IIII)V");

#undef FIND_METHOD
}

static void
android_init_graphics_point (void)
{
  jclass old;

  point_class.class
    = (*android_java_env)->FindClass (android_java_env,
				      "android/graphics/Point");
  eassert (point_class.class);

  old = point_class.class;
  point_class.class
    = (jclass) (*android_java_env)->NewGlobalRef (android_java_env,
						  (jobject) old);
  ANDROID_DELETE_LOCAL_REF (old);

  if (!point_class.class)
    emacs_abort ();

#define FIND_METHOD(c_name, name, signature)			\
  point_class.c_name						\
    = (*android_java_env)->GetMethodID (android_java_env,	\
					point_class.class,	\
					name, signature);	\
  assert (point_class.c_name);

  FIND_METHOD (constructor, "<init>", "(II)V");
#undef FIND_METHOD
}

extern JNIEXPORT void JNICALL
NATIVE_NAME (initEmacs) (JNIEnv *env, jobject object, jarray argv)
{
  char **c_argv;
  jsize nelements, i;
  jobject argument;
  const char *c_argument;

  android_java_env = env;

  nelements = (*env)->GetArrayLength (env, argv);
  c_argv = alloca (sizeof *c_argv * nelements);

  for (i = 0; i < nelements; ++i)
    {
      argument = (*env)->GetObjectArrayElement (env, argv, i);
      c_argument = (*env)->GetStringUTFChars (env, (jstring) argument,
					      NULL);

      if (!c_argument)
	emacs_abort ();

      /* Note that c_argument is in ``modified UTF-8 encoding'', but
	 we don't care as NUL bytes are not being specified inside.  */
      c_argv[i] = alloca (strlen (c_argument) + 1);
      strcpy (c_argv[i], c_argument);
      (*env)->ReleaseStringUTFChars (env, (jstring) argument, c_argument);
    }

  android_init_emacs_service ();
  android_init_emacs_pixmap ();
  android_init_graphics_point ();

  /* Initialize the Android GUI.  */
  android_init_gui = true;
  android_emacs_init (nelements, c_argv);
  /* android_emacs_init should never return.  */
  emacs_abort ();
}

extern JNIEXPORT void JNICALL
NATIVE_NAME (emacsAbort) (JNIEnv *env, jobject object)
{
  emacs_abort ();
}

extern JNIEXPORT void JNICALL
NATIVE_NAME (sendConfigureNotify) (JNIEnv *env, jobject object,
				   jshort window, jlong time,
				   jint x, jint y, jint width,
				   jint height)
{
  union android_event event;

  event.xconfigure.type = ANDROID_CONFIGURE_NOTIFY;
  event.xconfigure.window = window;
  event.xconfigure.time = time;
  event.xconfigure.x = x;
  event.xconfigure.y = y;
  event.xconfigure.width = width;
  event.xconfigure.height = height;

  android_write_event (&event);
}

extern JNIEXPORT void JNICALL
NATIVE_NAME (sendKeyPress) (JNIEnv *env, jobject object,
			    jshort window, jlong time,
			    jint state, jint keycode)
{
  union android_event event;

  event.xkey.type = ANDROID_KEY_PRESS;
  event.xkey.window = window;
  event.xkey.time = time;
  event.xkey.state = state;
  event.xkey.keycode = keycode;

  android_write_event (&event);
}

extern JNIEXPORT void JNICALL
NATIVE_NAME (sendKeyRelease) (JNIEnv *env, jobject object,
			      jshort window, jlong time,
			      jint state, jint keycode)
{
  union android_event event;

  event.xkey.type = ANDROID_KEY_RELEASE;
  event.xkey.window = window;
  event.xkey.time = time;
  event.xkey.state = state;
  event.xkey.keycode = keycode;

  android_write_event (&event);
}

#pragma clang diagnostic pop



/* Java functions called by C.

   Because all C code runs in the native function initEmacs, ALL LOCAL
   REFERENCES WILL PERSIST!

   This means that every local reference must be explicitly destroyed
   with DeleteLocalRef.  A helper macro is provided to do this.  */

#define MAX_HANDLE 65535

enum android_handle_type
  {
    ANDROID_HANDLE_WINDOW,
    ANDROID_HANDLE_GCONTEXT,
    ANDROID_HANDLE_PIXMAP,
  };

struct android_handle_entry
{
  /* The type.  */
  enum android_handle_type type;

  /* The handle.  */
  jobject handle;
};

/* Table of handles MAX_HANDLE long.  */
struct android_handle_entry android_handles[USHRT_MAX];

/* The largest handle ID currently known, but subject to
   wraparound.  */
static android_handle max_handle;

/* Allocate a new, unused, handle identifier.  If Emacs is out of
   identifiers, return 0.  */

static android_handle
android_alloc_id (void)
{
  android_handle handle;

  /* 0 is never a valid handle ID.  */
  if (!max_handle)
    max_handle++;

  if (android_handles[max_handle].handle)
    {
      handle = max_handle + 1;

      while (max_handle < handle)
	{
	  ++max_handle;

	  if (!max_handle)
	    ++max_handle;

	  if (!android_handles[max_handle].handle)
	    return 0;
	}

      return 0;
    }

  return max_handle++;
}

/* Destroy the specified handle and mark it as free on the Java side
   as well.  */

static void
android_destroy_handle (android_handle handle)
{
  static jclass old, class;
  static jmethodID method;

  if (!android_handles[handle].handle)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "Trying to destroy free handle!");
      emacs_abort ();
    }

  if (!class)
    {
      class
	= (*android_java_env)->FindClass (android_java_env,
					  "org/gnu/emacs/EmacsHandleObject");
      assert (class != NULL);

      method
	= (*android_java_env)->GetMethodID (android_java_env, class,
					    "destroyHandle", "()V");
      assert (method != NULL);

      old = class;
      class
	= (jclass) (*android_java_env)->NewGlobalRef (android_java_env,
						      (jobject) class);
      (*android_java_env)->ExceptionClear (android_java_env);
      ANDROID_DELETE_LOCAL_REF (old);

      if (!class)
	memory_full (0);
    }

  (*android_java_env)->CallVoidMethod (android_java_env,
				       android_handles[handle].handle,
				       method);
  (*android_java_env)->DeleteGlobalRef (android_java_env,
					android_handles[handle].handle);
  android_handles[handle].handle = NULL;
}

static jobject
android_resolve_handle (android_handle handle,
			enum android_handle_type type)
{
  if (!handle)
    /* ANDROID_NONE.  */
    return NULL;

  if (!android_handles[handle].handle)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "Trying to resolve free handle!");
      emacs_abort ();
    }

  if (android_handles[handle].type != type)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "Handle has wrong type!");
      emacs_abort ();
    }

  return android_handles[handle].handle;
}

static jobject
android_resolve_handle2 (android_handle handle,
			 enum android_handle_type type,
			 enum android_handle_type type2)
{
  if (!handle)
    return NULL;

  if (!android_handles[handle].handle)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "Trying to resolve free handle!");
      emacs_abort ();
    }

  if (android_handles[handle].type != type
      && android_handles[handle].type != type2)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "Handle has wrong type!");
      emacs_abort ();
    }

  return android_handles[handle].handle;
}

static jmethodID android_lookup_method (const char *, const char *,
					const char *);

void
android_change_window_attributes (android_window handle,
				  enum android_window_value_mask value_mask,
				  struct android_set_window_attributes *attrs)
{
  jmethodID method;
  jobject window;

  window = android_resolve_handle (handle, ANDROID_HANDLE_WINDOW);

  if (value_mask & ANDROID_CW_BACK_PIXEL)
    {
      method = android_lookup_method ("org/gnu/emacs/EmacsWindow",
				      "changeWindowBackground", "(I)V");
      (*android_java_env)->CallVoidMethod (android_java_env,
					   window, method,
					   (jint) attrs->background_pixel);
    }
}

/* Create a new window with the given width, height and
   attributes.  */

android_window
android_create_window (android_window parent, int x, int y,
		       int width, int height,
		       enum android_window_value_mask value_mask,
		       struct android_set_window_attributes *attrs)
{
  static jclass class;
  static jmethodID constructor;
  jobject object, parent_object, old;
  android_window window;
  android_handle prev_max_handle;

  parent_object = android_resolve_handle (parent, ANDROID_HANDLE_WINDOW);

  prev_max_handle = max_handle;
  window = android_alloc_id ();

  if (!window)
    error ("Out of window handles!");

  if (!class)
    {
      class = (*android_java_env)->FindClass (android_java_env,
					      "org/gnu/emacs/EmacsWindow");
      assert (class != NULL);

      constructor
	= (*android_java_env)->GetMethodID (android_java_env, class, "<init>",
					    "(SLorg/gnu/emacs/EmacsWindow;IIII)V");
      assert (constructor != NULL);

      old = class;
      class = (*android_java_env)->NewGlobalRef (android_java_env, class);
      (*android_java_env)->ExceptionClear (android_java_env);
      ANDROID_DELETE_LOCAL_REF (old);

      if (!class)
	memory_full (0);
    }

  object = (*android_java_env)->NewObject (android_java_env, class,
					   constructor, (jshort) window,
					   parent_object, (jint) x, (jint) y,
					   (jint) width, (jint) height);
  if (!object)
    {
      (*android_java_env)->ExceptionClear (android_java_env);

      max_handle = prev_max_handle;
      memory_full (0);
    }

  android_handles[window].type = ANDROID_HANDLE_WINDOW;
  android_handles[window].handle
    = (*android_java_env)->NewGlobalRef (android_java_env,
					 object);
  (*android_java_env)->ExceptionClear (android_java_env);
  ANDROID_DELETE_LOCAL_REF (object);

  if (!android_handles[window].handle)
    memory_full (0);

  android_change_window_attributes (window, value_mask, attrs);
  return window;
}

void
android_set_window_background (android_window window, unsigned long pixel)
{
  struct android_set_window_attributes attrs;

  attrs.background_pixel = pixel;
  android_change_window_attributes (window, ANDROID_CW_BACK_PIXEL,
				    &attrs);
}

void
android_destroy_window (android_window window)
{
  if (android_handles[window].type != ANDROID_HANDLE_WINDOW)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "Trying to destroy something not a window!");
      emacs_abort ();
    }

  android_destroy_handle (window);
}

static void
android_init_android_rect_class (void)
{
  jclass old;

  if (android_rect_class)
    /* Already initialized.  */
    return;

  android_rect_class
    = (*android_java_env)->FindClass (android_java_env,
				      "android/graphics/Rect");
  assert (android_rect_class);

  android_rect_constructor
    = (*android_java_env)->GetMethodID (android_java_env, android_rect_class,
					"<init>", "(IIII)V");
  assert (emacs_gc_constructor);

  old = android_rect_class;
  android_rect_class
    = (jclass) (*android_java_env)->NewGlobalRef (android_java_env,
						  (jobject) android_rect_class);
  (*android_java_env)->ExceptionClear (android_java_env);
  ANDROID_DELETE_LOCAL_REF (old);

  if (!android_rect_class)
    memory_full (0);
}

static void
android_init_emacs_gc_class (void)
{
  jclass old;

  if (emacs_gc_class)
    /* Already initialized.  */
    return;

  emacs_gc_class
    = (*android_java_env)->FindClass (android_java_env,
				      "org/gnu/emacs/EmacsGC");
  assert (emacs_gc_class);

  emacs_gc_constructor
    = (*android_java_env)->GetMethodID (android_java_env,
					emacs_gc_class,
					"<init>", "(S)V");
  assert (emacs_gc_constructor);

  emacs_gc_mark_dirty
    = (*android_java_env)->GetMethodID (android_java_env,
					emacs_gc_class,
					"markDirty", "()V");
  assert (emacs_gc_mark_dirty);

  old = emacs_gc_class;
  emacs_gc_class
    = (jclass) (*android_java_env)->NewGlobalRef (android_java_env,
						  (jobject) emacs_gc_class);
  (*android_java_env)->ExceptionClear (android_java_env);
  ANDROID_DELETE_LOCAL_REF (old);
  if (!emacs_gc_class)
    memory_full (0);

  emacs_gc_foreground
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "foreground", "I");
  emacs_gc_background
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "background", "I");
  emacs_gc_function
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "function", "I");
  emacs_gc_clip_rects
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "clip_rects",
				       "[Landroid/graphics/Rect;");
  emacs_gc_clip_x_origin
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "clip_x_origin", "I");
  emacs_gc_clip_y_origin
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "clip_y_origin", "I");
  emacs_gc_stipple
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "stipple",
				       "Lorg/gnu/emacs/EmacsPixmap;");
  emacs_gc_clip_mask
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "clip_mask",
				       "Lorg/gnu/emacs/EmacsPixmap;");
  emacs_gc_fill_style
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "fill_style", "I");
  emacs_gc_ts_origin_x
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "ts_origin_x", "I");
  emacs_gc_ts_origin_y
    = (*android_java_env)->GetFieldID (android_java_env,
				       emacs_gc_class,
				       "ts_origin_y", "I");
}

struct android_gc *
android_create_gc (enum android_gc_value_mask mask,
		   struct android_gc_values *values)
{
  struct android_gc *gc;
  android_handle prev_max_handle;
  jobject object;

  android_init_emacs_gc_class ();

  gc = xmalloc (sizeof *gc);
  prev_max_handle = max_handle;
  gc->gcontext = android_alloc_id ();

  if (!gc->gcontext)
    {
      xfree (gc);
      error ("Out of GContext handles!");
    }

  object = (*android_java_env)->NewObject (android_java_env,
					   emacs_gc_class,
					   emacs_gc_constructor,
					   (jshort) gc->gcontext);

  if (!object)
    {
      (*android_java_env)->ExceptionClear (android_java_env);

      max_handle = prev_max_handle;
      memory_full (0);
    }

  android_handles[gc->gcontext].type = ANDROID_HANDLE_GCONTEXT;
  android_handles[gc->gcontext].handle
    = (*android_java_env)->NewGlobalRef (android_java_env, object);
  (*android_java_env)->ExceptionClear (android_java_env);
  ANDROID_DELETE_LOCAL_REF (object);

  if (!android_handles[gc->gcontext].handle)
    memory_full (0);

  android_change_gc (gc, mask, values);
  return gc;
}

void
android_free_gc (struct android_gc *gc)
{
  android_destroy_handle (gc->gcontext);
  xfree (gc);
}

void
android_change_gc (struct android_gc *gc,
		   enum android_gc_value_mask mask,
		   struct android_gc_values *values)
{
  jobject what, gcontext;

  android_init_emacs_gc_class ();
  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  if (mask & ANDROID_GC_FOREGROUND)
    (*android_java_env)->SetIntField (android_java_env,
				      gcontext,
				      emacs_gc_foreground,
				      values->foreground);

  if (mask & ANDROID_GC_BACKGROUND)
    (*android_java_env)->SetIntField (android_java_env,
				      gcontext,
				      emacs_gc_background,
				      values->background);

  if (mask & ANDROID_GC_FUNCTION)
    (*android_java_env)->SetIntField (android_java_env,
				      gcontext,
				      emacs_gc_function,
				      values->function);

  if (mask & ANDROID_GC_CLIP_X_ORIGIN)
    (*android_java_env)->SetIntField (android_java_env,
				      gcontext,
				      emacs_gc_clip_x_origin,
				      values->clip_x_origin);

  if (mask & ANDROID_GC_CLIP_Y_ORIGIN)
    (*android_java_env)->SetIntField (android_java_env,
				      gcontext,
				      emacs_gc_clip_y_origin,
				      values->clip_y_origin);

  if (mask & ANDROID_GC_CLIP_MASK)
    {
      what = android_resolve_handle (values->clip_mask,
				     ANDROID_HANDLE_PIXMAP);
      (*android_java_env)->SetObjectField (android_java_env,
					   gcontext,
					   emacs_gc_stipple,
					   what);

      /* Clearing GCClipMask also clears the clip rectangles.  */
      (*android_java_env)->SetObjectField (android_java_env,
					   gcontext,
					   emacs_gc_clip_rects,
					   NULL);
    }

  if (mask & ANDROID_GC_STIPPLE)
    {
      what = android_resolve_handle (values->stipple,
				     ANDROID_HANDLE_PIXMAP);
      (*android_java_env)->SetObjectField (android_java_env,
					   gcontext,
					   emacs_gc_stipple,
					   what);
    }

  if (mask & ANDROID_GC_FILL_STYLE)
    (*android_java_env)->SetIntField (android_java_env,
				      gcontext,
				      emacs_gc_fill_style,
				      values->fill_style);

  if (mask & ANDROID_GC_TILE_STIP_X_ORIGIN)
    (*android_java_env)->SetIntField (android_java_env,
				      gcontext,
				      emacs_gc_ts_origin_x,
				      values->ts_x_origin);

  if (mask & ANDROID_GC_TILE_STIP_Y_ORIGIN)
    (*android_java_env)->SetIntField (android_java_env,
				      gcontext,
				      emacs_gc_ts_origin_y,
				      values->ts_y_origin);

  if (mask)
    (*android_java_env)->CallVoidMethod (android_java_env,
					 gcontext,
					 emacs_gc_mark_dirty);
}

void
android_set_clip_rectangles (struct android_gc *gc, int clip_x_origin,
			     int clip_y_origin,
			     struct android_rectangle *clip_rects,
			     int n_clip_rects)
{
  jobjectArray array;
  jobject rect, gcontext;
  int i;

  android_init_android_rect_class ();
  android_init_emacs_gc_class ();

  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  array = (*android_java_env)->NewObjectArray (android_java_env,
					       n_clip_rects,
					       android_rect_class,
					       NULL);

  if (!array)
    {
      (*android_java_env)->ExceptionClear (android_java_env);
      memory_full (0);
    }

  for (i = 0; i < n_clip_rects; ++i)
    {
      rect = (*android_java_env)->NewObject (android_java_env,
					     android_rect_class,
					     android_rect_constructor,
					     (jint) clip_rects[i].x,
					     (jint) clip_rects[i].y,
					     (jint) (clip_rects[i].x
						     + clip_rects[i].width),
					     (jint) (clip_rects[i].y
						     + clip_rects[i].height));

      if (!rect)
	{
	  (*android_java_env)->ExceptionClear (android_java_env);
	  ANDROID_DELETE_LOCAL_REF (array);
	  memory_full (0);
	}

      (*android_java_env)->SetObjectArrayElement (android_java_env,
						  array, i, rect);
      ANDROID_DELETE_LOCAL_REF (rect);
    }

  (*android_java_env)->SetObjectField (android_java_env,
				       gcontext,
				       emacs_gc_clip_rects,
				       (jobject) array);
  ANDROID_DELETE_LOCAL_REF (array);

  (*android_java_env)->SetIntField (android_java_env,
				    gcontext,
				    emacs_gc_clip_x_origin,
				    clip_x_origin);
  (*android_java_env)->SetIntField (android_java_env,
				    gcontext,
				    emacs_gc_clip_y_origin,
				    clip_y_origin);

  (*android_java_env)->CallVoidMethod (android_java_env,
				       gcontext,
				       emacs_gc_mark_dirty);
}

void
android_reparent_window (android_window w, android_window parent,
			 int x, int y)
{
  /* TODO */
}

/* Look up the method with SIGNATURE by NAME in CLASS.  Abort if it
   could not be found.  This should be used for functions which are
   not called very often.

   CLASS must never be unloaded, or the behavior is undefined.  */

static jmethodID
android_lookup_method (const char *class, const char *name,
		       const char *signature)
{
  jclass java_class;
  jmethodID method;

  java_class
    = (*android_java_env)->FindClass (android_java_env, class);

  if (!java_class)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "Failed to find class %s", class);
      emacs_abort ();
    }

  method
    = (*android_java_env)->GetMethodID (android_java_env,
					java_class, name,
					signature);

  if (!method)
    {
      __android_log_print (ANDROID_LOG_ERROR, __func__,
			   "Failed to find method %s in class %s"
			   " with signature %s",
			   name, class, signature);
      emacs_abort ();
    }

  ANDROID_DELETE_LOCAL_REF (java_class);
  return method;
}

void
android_clear_window (android_window handle)
{
  jobject window;

  window = android_resolve_handle (handle, ANDROID_HANDLE_WINDOW);

  (*android_java_env)->CallVoidMethod (android_java_env,
				       emacs_service,
				       service_class.clear_window,
				       window);
}

void
android_map_window (android_window handle)
{
  jobject window;
  jmethodID map_window;

  window = android_resolve_handle (handle, ANDROID_HANDLE_WINDOW);
  map_window = android_lookup_method ("org/gnu/emacs/EmacsWindow",
				      "mapWindow", "()V");

  (*android_java_env)->CallVoidMethod (android_java_env,
				       window, map_window);
}

void
android_unmap_window (android_window handle)
{
  jobject window;
  jmethodID unmap_window;

  window = android_resolve_handle (handle, ANDROID_HANDLE_WINDOW);
  unmap_window = android_lookup_method ("org/gnu/emacs/EmacsWindow",
					"unmapWindow", "()V");

  (*android_java_env)->CallVoidMethod (android_java_env,
				       window, unmap_window);
}

void
android_resize_window (android_window handle, unsigned int width,
		       unsigned int height)
{
  jobject window;
  jmethodID resize_window;

  window = android_resolve_handle (handle, ANDROID_HANDLE_WINDOW);
  resize_window = android_lookup_method ("org/gnu/emacs/EmacsWindow",
					 "resizeWindow", "(II)V");

  (*android_java_env)->CallVoidMethod (android_java_env,
				       window, resize_window,
				       (jint) width, (jint) height);
}

void
android_move_window (android_window handle, int x, int y)
{
  jobject window;
  jmethodID move_window;

  window = android_resolve_handle (handle, ANDROID_HANDLE_WINDOW);
  move_window = android_lookup_method ("org/gnu/emacs/EmacsWindow",
				       "moveWindow", "(II)V");

  (*android_java_env)->CallVoidMethod (android_java_env,
				       window, move_window,
				       (jint) x, (jint) y);
}

void
android_swap_buffers (struct android_swap_info *swap_info,
		      int num_windows)
{
  jobject window;
  jmethodID swap_buffers;
  int i;

  swap_buffers = android_lookup_method ("org/gnu/emacs/EmacsWindow",
					"swapBuffers", "()V");

  for (i = 0; i < num_windows; ++i)
    {
      window = android_resolve_handle (swap_info[i].swap_window,
				       ANDROID_HANDLE_WINDOW);
      (*android_java_env)->CallVoidMethod (android_java_env,
					   window, swap_buffers);
    }
}

void
android_get_gc_values (struct android_gc *gc,
		       enum android_gc_value_mask mask,
		       struct android_gc_values *values)
{
  jobject gcontext;

  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  if (mask & ANDROID_GC_FOREGROUND)
    /* GCs never have 32 bit colors, so we don't have to worry about
       sign extension here.  */
    values->foreground
      = (*android_java_env)->GetIntField (android_java_env,
					  gcontext,
					  emacs_gc_foreground);

  if (mask & ANDROID_GC_BACKGROUND)
    values->background
      = (*android_java_env)->GetIntField (android_java_env,
					  gcontext,
					  emacs_gc_background);

  if (mask & ANDROID_GC_FUNCTION)
    values->function
      = (*android_java_env)->GetIntField (android_java_env,
					  gcontext,
					  emacs_gc_function);

  if (mask & ANDROID_GC_CLIP_X_ORIGIN)
    values->clip_x_origin
      = (*android_java_env)->GetIntField (android_java_env,
					  gcontext,
					  emacs_gc_clip_x_origin);

  if (mask & ANDROID_GC_CLIP_Y_ORIGIN)
    values->clip_y_origin
      = (*android_java_env)->GetIntField (android_java_env,
					  gcontext,
					  emacs_gc_clip_y_origin);

  if (mask & ANDROID_GC_FILL_STYLE)
    values->fill_style
      = (*android_java_env)->GetIntField (android_java_env,
					  gcontext,
					  emacs_gc_fill_style);

  if (mask & ANDROID_GC_TILE_STIP_X_ORIGIN)
    values->ts_x_origin
      = (*android_java_env)->GetIntField (android_java_env,
					  gcontext,
					  emacs_gc_ts_origin_x);

  if (mask & ANDROID_GC_TILE_STIP_Y_ORIGIN)
    values->ts_y_origin
      = (*android_java_env)->GetIntField (android_java_env,
					  gcontext,
					  emacs_gc_ts_origin_y);

  /* Fields involving handles are not used by Emacs, and thus not
     implemented */
}

void
android_set_foreground (struct android_gc *gc, unsigned long foreground)
{
  struct android_gc_values gcv;

  gcv.foreground = foreground;
  android_change_gc (gc, ANDROID_GC_FOREGROUND, &gcv);
}

void
android_fill_rectangle (android_drawable handle, struct android_gc *gc,
			int x, int y, unsigned int width,
			unsigned int height)
{
  jobject drawable, gcontext;

  drawable = android_resolve_handle2 (handle,
				      ANDROID_HANDLE_WINDOW,
				      ANDROID_HANDLE_PIXMAP);
  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  (*android_java_env)->CallVoidMethod (android_java_env,
				       emacs_service,
				       service_class.fill_rectangle,
				       drawable,
				       gcontext,
				       (jint) x, (jint) y,
				       (jint) width,
				       (jint) height);
}

android_pixmap
android_create_pixmap_from_bitmap_data (char *data, unsigned int width,
					unsigned int height,
					unsigned long foreground,
					unsigned long background,
					unsigned int depth)
{
  android_handle prev_max_handle;
  jobject object;
  jintArray colors;
  android_pixmap pixmap;
  unsigned int x, y;
  jint *region;

  USE_SAFE_ALLOCA;

  /* Create the color array holding the data.  */
  colors = (*android_java_env)->NewIntArray (android_java_env,
					     width * height);

  if (!colors)
    {
      (*android_java_env)->ExceptionClear (android_java_env);
      memory_full (0);
    }

  SAFE_NALLOCA (region, sizeof *region, width);

  for (y = 0; y < height; ++y)
    {
      for (x = 0; x < width; ++x)
	{
	  if (data[y / 8] & (1 << (x % 8)))
	    region[x] = foreground;
	  else
	    region[x] = background;
	}

      (*android_java_env)->SetIntArrayRegion (android_java_env,
					      colors,
					      width * y, width,
					      region);
    }

  /* First, allocate the pixmap handle.  */
  prev_max_handle = max_handle;
  pixmap = android_alloc_id ();

  if (!pixmap)
    {
      ANDROID_DELETE_LOCAL_REF ((jobject) colors);
      error ("Out of pixmap handles!");
    }

  object = (*android_java_env)->NewObject (android_java_env,
					   pixmap_class.class,
					   pixmap_class.constructor,
					   (jshort) pixmap, colors,
					   (jint) width, (jint) height,
					   (jint) depth);
  (*android_java_env)->ExceptionClear (android_java_env);
  ANDROID_DELETE_LOCAL_REF ((jobject) colors);

  if (!object)
    {
      max_handle = prev_max_handle;
      memory_full (0);
    }

  android_handles[pixmap].type = ANDROID_HANDLE_PIXMAP;
  android_handles[pixmap].handle
    = (*android_java_env)->NewGlobalRef (android_java_env, object);
  ANDROID_DELETE_LOCAL_REF (object);

  if (!android_handles[pixmap].handle)
    memory_full (0);

  SAFE_FREE ();
  return pixmap;
}

void
android_set_clip_mask (struct android_gc *gc, android_pixmap pixmap)
{
  struct android_gc_values gcv;

  gcv.clip_mask = pixmap;
  android_change_gc (gc, ANDROID_GC_CLIP_MASK, &gcv);
}

void
android_set_fill_style (struct android_gc *gc,
			enum android_fill_style fill_style)
{
  struct android_gc_values gcv;

  gcv.fill_style = fill_style;
  android_change_gc (gc, ANDROID_GC_FILL_STYLE, &gcv);
}

void
android_copy_area (android_drawable src, android_drawable dest,
		   struct android_gc *gc, int src_x, int src_y,
		   unsigned int width, unsigned int height,
		   int dest_x, int dest_y)
{
  jobject src_object, dest_object, gcontext;

  src_object = android_resolve_handle2 (src, ANDROID_HANDLE_WINDOW,
					ANDROID_HANDLE_PIXMAP);
  dest_object = android_resolve_handle2 (dest, ANDROID_HANDLE_WINDOW,
					 ANDROID_HANDLE_PIXMAP);
  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  (*android_java_env)->CallVoidMethod (android_java_env,
				       emacs_service,
				       service_class.copy_area,
				       src_object,
				       dest_object,
				       gcontext,
				       (jint) src_x, (jint) src_y,
				       (jint) width, (jint) height,
				       (jint) dest_x, (jint) dest_y);
}

void
android_free_pixmap (android_pixmap pixmap)
{
  android_destroy_handle (pixmap);
}

void
android_set_background (struct android_gc *gc, unsigned long background)
{
  struct android_gc_values gcv;

  gcv.background = background;
  android_change_gc (gc, ANDROID_GC_BACKGROUND, &gcv);
}

void
android_fill_polygon (android_drawable drawable, struct android_gc *gc,
		      struct android_point *points, int npoints,
		      enum android_shape shape, enum android_coord_mode mode)
{
  jobjectArray array;
  jobject point, drawable_object, gcontext;
  int i;

  drawable_object = android_resolve_handle2 (drawable,
					     ANDROID_HANDLE_WINDOW,
					     ANDROID_HANDLE_PIXMAP);
  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  array = (*android_java_env)->NewObjectArray (android_java_env,
					       npoints,
					       point_class.class,
					       NULL);

  if (!array)
    {
      (*android_java_env)->ExceptionClear (android_java_env);
      memory_full (0);
    }

  for (i = 0; i < npoints; ++i)
    {
      point = (*android_java_env)->NewObject (android_java_env,
					      point_class.class,
					      point_class.constructor,
					      (jint) points[i].x,
					      (jint) points[i].y);

      if (!point)
	{
	  (*android_java_env)->ExceptionClear (android_java_env);
	  ANDROID_DELETE_LOCAL_REF (array);
	  memory_full (0);
	}

      (*android_java_env)->SetObjectArrayElement (android_java_env,
						  array, i, point);
      ANDROID_DELETE_LOCAL_REF (point);
    }

  (*android_java_env)->CallVoidMethod (android_java_env,
				       emacs_service,
				       service_class.fill_polygon,
				       drawable_object,
				       gcontext, array);
  ANDROID_DELETE_LOCAL_REF (array);
}

void
android_draw_rectangle (android_drawable handle, struct android_gc *gc,
			int x, int y, unsigned int width, unsigned int height)
{
  jobject drawable, gcontext;

  drawable = android_resolve_handle2 (handle,
				      ANDROID_HANDLE_WINDOW,
				      ANDROID_HANDLE_PIXMAP);
  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  (*android_java_env)->CallVoidMethod (android_java_env,
				       emacs_service,
				       service_class.draw_rectangle,
				       drawable, gcontext,
				       (jint) x, (jint) y,
				       (jint) width, (jint) height);
}

void
android_draw_point (android_drawable handle, struct android_gc *gc,
		    int x, int y)
{
  jobject drawable, gcontext;

  drawable = android_resolve_handle2 (handle,
				      ANDROID_HANDLE_WINDOW,
				      ANDROID_HANDLE_PIXMAP);
  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  (*android_java_env)->CallVoidMethod (android_java_env,
				       emacs_service,
				       service_class.draw_point,
				       drawable, gcontext,
				       (jint) x, (jint) y);
}

void
android_draw_line (android_drawable handle, struct android_gc *gc,
		   int x, int y, int x2, int y2)
{
  jobject drawable, gcontext;

  drawable = android_resolve_handle2 (handle,
				      ANDROID_HANDLE_WINDOW,
				      ANDROID_HANDLE_PIXMAP);
  gcontext = android_resolve_handle (gc->gcontext,
				     ANDROID_HANDLE_GCONTEXT);

  (*android_java_env)->CallVoidMethod (android_java_env,
				       emacs_service,
				       service_class.draw_line,
				       drawable, gcontext,
				       (jint) x, (jint) y,
				       (jint) x2, (jint) y2);
}

android_pixmap
android_create_pixmap (unsigned int width, unsigned int height,
		       int depth)
{
  android_handle prev_max_handle;
  jobject object;
  jintArray colors;
  android_pixmap pixmap;

  /* Create the color array holding the data.  */
  colors = (*android_java_env)->NewIntArray (android_java_env,
					     width * height);

  if (!colors)
    {
      (*android_java_env)->ExceptionClear (android_java_env);
      memory_full (0);
    }

  /* First, allocate the pixmap handle.  */
  prev_max_handle = max_handle;
  pixmap = android_alloc_id ();

  if (!pixmap)
    {
      ANDROID_DELETE_LOCAL_REF ((jobject) colors);
      error ("Out of pixmap handles!");
    }

  object = (*android_java_env)->NewObject (android_java_env,
					   pixmap_class.class,
					   pixmap_class.constructor,
					   (jshort) pixmap, colors,
					   (jint) width, (jint) height,
					   (jint) depth);
  ANDROID_DELETE_LOCAL_REF ((jobject) colors);

  if (!object)
    {
      (*android_java_env)->ExceptionClear (android_java_env);
      max_handle = prev_max_handle;
      memory_full (0);
    }

  android_handles[pixmap].type = ANDROID_HANDLE_PIXMAP;
  android_handles[pixmap].handle
    = (*android_java_env)->NewGlobalRef (android_java_env, object);
  (*android_java_env)->ExceptionClear (android_java_env);
  ANDROID_DELETE_LOCAL_REF (object);

  if (!android_handles[pixmap].handle)
    memory_full (0);

  return pixmap;
}

void
android_set_ts_origin (struct android_gc *gc, int x, int y)
{
  struct android_gc_values gcv;

  gcv.ts_x_origin = x;
  gcv.ts_y_origin = y;
  android_change_gc (gc, (ANDROID_GC_TILE_STIP_X_ORIGIN
			  | ANDROID_GC_TILE_STIP_Y_ORIGIN),
		     &gcv);
}

void
android_clear_area (android_window handle, int x, int y,
		    unsigned int width, unsigned int height)
{
  jobject window;

  window = android_resolve_handle (handle, ANDROID_HANDLE_WINDOW);

  (*android_java_env)->CallVoidMethod (android_java_env,
				       emacs_service,
				       service_class.clear_area,
				       window, (jint) x, (jint) y,
				       (jint) width, (jint) height);
}

#else /* ANDROID_STUBIFY */

/* X emulation functions for Android.  */

struct android_gc *
android_create_gc (enum android_gc_value_mask mask,
		   struct android_gc_values *values)
{
  /* This function should never be called when building stubs.  */
  emacs_abort ();
}

void
android_free_gc (struct android_gc *gc)
{
  /* This function should never be called when building stubs.  */
  emacs_abort ();
}

#endif
