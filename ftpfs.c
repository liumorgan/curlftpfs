/*
    FTP file system
    Copyright (C) 2006 Robson Braga Araujo <robsonbraga@gmail.com>

    This program can be distributed under the terms of the GNU GPL.
    See the file COPYING.
*/

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <fuse.h>
#include <fuse_opt.h>
#include <glib.h>
#include <semaphore.h>
#include <assert.h>

#include "error.h"
#include "buffer.h"
#include "charset_utils.h"
#include "path_utils.h"
#include "ftpfs-ls.h"
#include "cache.h"
#include "passwd.h"
#include "ftpfs.h"

#define MAX_BUFFER_LEN (300*1024)

struct ftpfs ftpfs;
static char error_buf[CURL_ERROR_SIZE];

struct ftpfs_file {
  struct buffer buf;
  int dirty;
  int copied;
  off_t last_offset;
  int can_shrink;
  pthread_t thread_id;
  mode_t mode;
  char * open_path;
  char * full_path;
  struct buffer stream_buf;
  CURL *write_conn;
  sem_t data_avail;
  sem_t data_need;
  sem_t data_written;
  sem_t ready;
  int isready;
  int eof;
  int written_flag;
  int write_fail_cause;
  int write_may_start;
  char curl_error_buffer[CURL_ERROR_SIZE];
  off_t pos;
};

void ftpfs_curl_easy_setopt_abort(void) {
  fprintf(stderr, "Error setting curl: %s\n", error_buf);
  exit(1);
}

void ftpfs_curl_easy_perform_abort(void) {
  fprintf(stderr, "Error connecting to ftp: %s\n", error_buf);
  exit(1);
}

void cancel_previous_multi(void) {
  CURLMcode curlMCode;
  /* curl_multi_cleanup(ftpfs.multi); */

  if (!ftpfs.attached_to_multi)
    return;

  DEBUG(1, "cancel previous multi\n");

  curlMCode = curl_multi_remove_handle(ftpfs.multi, ftpfs.connection);
  if (curlMCode != CURLM_OK) {
    fprintf(stderr, "curl_multi_remove_handle problem: %d\n", curlMCode);
    exit(1);
  }

  ftpfs.attached_to_multi = 0;

  return;
}

static int op_return(int err, const char * operation)
{
  if(!err)
  {
    DEBUG(2, "%s successful\n", operation);
    return 0;
  }
  fprintf(stderr, "ftpfs: operation %s failed because %s\n", operation, strerror(-err));
  return err;
}


static size_t write_data(void *ptr, size_t size, size_t nmemb, void *data) {
  struct ftpfs_file* fh = (struct ftpfs_file*)data;
  size_t to_copy;
  if (fh == NULL) return 0;
  to_copy = size * nmemb;
  if (to_copy > fh->buf.len - fh->copied) {
    to_copy = fh->buf.len - fh->copied;
  }
  DEBUG(2, "write_data: %zu\n", to_copy);
  DEBUG(3, "%*s\n", (int)to_copy, (char*)ptr);
  memcpy(ptr, fh->buf.p + fh->copied, to_copy);
  fh->copied += to_copy;
  return to_copy;
}

static size_t read_data(void *ptr, size_t size, size_t nmemb, void *data) {
  struct buffer* buf = (struct buffer*)data;
  if (buf == NULL) return size * nmemb;
  if (buf_add_mem(buf, ptr, size * nmemb) == -1)
    return 0;

  DEBUG(2, "read_data: %zu\n", size * nmemb);
  DEBUG(3, "%*s\n", (int)(size * nmemb), (char*)ptr);
  return size * nmemb;
}

static int ftpfs_getdir(const char* path, fuse_cache_dirh_t h,
                        fuse_cache_dirfil_t filler) {
  int err = 0;
  CURLcode curl_res;
  struct buffer buf;
  char* dir_path = get_fulldir_path(path);

  DEBUG(1, "ftpfs_getdir: %s\n", dir_path);
  buf_init(&buf);

  pthread_mutex_lock(&ftpfs.lock);
  cancel_previous_multi();
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_URL, dir_path);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_WRITEDATA, &buf);
  curl_res = curl_easy_perform(ftpfs.connection);
  pthread_mutex_unlock(&ftpfs.lock);

  if (curl_res != 0) {
    DEBUG(1, "%s\n", error_buf);
    err = -EIO;
  } else {
    buf_null_terminate(&buf);
    parse_dir((char*)buf.p, dir_path + strlen(ftpfs.host) - 1,
              NULL, NULL, NULL, 0, h, filler);
  }

  free(dir_path);
  buf_free(&buf);
  return op_return(err, "ftpfs_getdir");
}

static int ftpfs_getattr(const char* path, struct stat* sbuf) {
  int err;
  CURLcode curl_res;
  struct buffer buf;
  char* name;
  char* dir_path = get_dir_path(path);

  DEBUG(2, "ftpfs_getattr: %s dir_path=%s\n", path, dir_path);
  buf_init(&buf);

  pthread_mutex_lock(&ftpfs.lock);
  cancel_previous_multi();
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_URL, dir_path);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_WRITEDATA, &buf);
  curl_res = curl_easy_perform(ftpfs.connection);
  pthread_mutex_unlock(&ftpfs.lock);

  if (curl_res != 0) {
    DEBUG(1, "%s\n", error_buf);
  }
  buf_null_terminate(&buf);

  name = strrchr(path, '/');
  ++name;
  err = parse_dir((char*)buf.p, dir_path + strlen(ftpfs.host) - 1,
                  name, sbuf, NULL, 0, NULL, NULL);

  free(dir_path);
  buf_free(&buf);
  if (err) return op_return(-ENOENT, "ftpfs_getattr");
  return 0;
}


static int check_running(void) {
  int running_handles = 0;
  curl_multi_perform(ftpfs.multi, &running_handles);
  return running_handles;
}

static struct ftpfs_file *get_ftpfs_file(struct fuse_file_info *fi) {
  return (struct ftpfs_file *) (uintptr_t) fi->fh;
}

static size_t ftpfs_read_chunk(const char* full_path, char* rbuf,
                               size_t size, off_t offset,
                               struct fuse_file_info* fi,
                               int update_offset) {
  int running_handles = 0;
  int err = 0;
  size_t to_copy;
  struct ftpfs_file* fh = get_ftpfs_file(fi);

  DEBUG(2, "ftpfs_read_chunk: %s %p %zu %lld %p %p\n",
        full_path, rbuf, size, (long long) offset, (void *) fi, (void *) fh);

  pthread_mutex_lock(&ftpfs.lock);

  DEBUG(2, "buffer size: %zu %lld\n", fh->buf.len, (long long) fh->buf.begin_offset);

  if ((fh->buf.len < size + offset - fh->buf.begin_offset) ||
      offset < fh->buf.begin_offset ||
      offset > fh->buf.begin_offset + fh->buf.len) {
    /* We can't answer this from cache */
    if (ftpfs.current_fh != fh ||
        offset < fh->buf.begin_offset ||
        offset > fh->buf.begin_offset + fh->buf.len ||
        !check_running()) {
      CURLMcode curlMCode;

      DEBUG(1, "We need to restart the connection %p\n", ftpfs.connection);
      DEBUG(2, "current_fh=%p fh=%p\n", (void *) ftpfs.current_fh, (void *) fh);
      DEBUG(2, "buf.begin_offset=%lld offset=%lld\n", (long long) fh->buf.begin_offset, (long long) offset);

      buf_clear(&fh->buf);
      fh->buf.begin_offset = offset;
      ftpfs.current_fh = fh;

      cancel_previous_multi();

      curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_URL, full_path);
      curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_WRITEDATA, &fh->buf);
      if (offset) {
        char range[15];
        snprintf(range, 15, "%lld-", (long long) offset);
        curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_RANGE, range);
      }

      curlMCode = curl_multi_add_handle(ftpfs.multi, ftpfs.connection);
      if (curlMCode != CURLM_OK)
      {
          fprintf(stderr, "curl_multi_add_handle problem: %d\n", curlMCode);
          exit(1);
      }
      ftpfs.attached_to_multi = 1;
    }

    while(CURLM_CALL_MULTI_PERFORM ==
        curl_multi_perform(ftpfs.multi, &running_handles));

    curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_RANGE, NULL);

    while ((fh->buf.len < size + offset - fh->buf.begin_offset) &&
        running_handles) {
      struct timeval timeout;
      int rc; /* select() return code */

      fd_set fdread;
      fd_set fdwrite;
      fd_set fdexcep;
      int maxfd;

      FD_ZERO(&fdread);
      FD_ZERO(&fdwrite);
      FD_ZERO(&fdexcep);

      /* set a suitable timeout to play around with */
      timeout.tv_sec = 1;
      timeout.tv_usec = 0;

      /* get file descriptors from the transfers */
      curl_multi_fdset(ftpfs.multi, &fdread, &fdwrite, &fdexcep, &maxfd);

      rc = select(maxfd+1, &fdread, &fdwrite, &fdexcep, &timeout);
      if (rc == -1) {
          err = 1;
          break;
      }
      while(CURLM_CALL_MULTI_PERFORM ==
            curl_multi_perform(ftpfs.multi, &running_handles));
    }

    if (running_handles == 0) {
      int msgs_left = 1;
      while (msgs_left) {
        CURLMsg* msg = curl_multi_info_read(ftpfs.multi, &msgs_left);
        if (msg == NULL ||
            msg->msg != CURLMSG_DONE ||
            msg->data.result != CURLE_OK) {
          DEBUG(1, "error: curl_multi_info %d\n", msg->msg);
          err = 1;
        }
      }
    }
  }

  to_copy = fh->buf.len + fh->buf.begin_offset - offset;
  size = size > to_copy ? to_copy : size;
  if (rbuf) {
    memcpy(rbuf, fh->buf.p + offset - fh->buf.begin_offset, size);
  }

  if (update_offset) {
    fh->last_offset = offset + size;
  }

  /* Check if the buffer is growing and we can delete a part of it */
  if (fh->can_shrink && fh->buf.len > MAX_BUFFER_LEN) {
    DEBUG(2, "Shrinking buffer from %zu to %zu bytes\n",
          fh->buf.len, to_copy - size);
    memmove(fh->buf.p,
            fh->buf.p + offset - fh->buf.begin_offset + size,
            to_copy - size);
    fh->buf.len = to_copy - size;
    fh->buf.begin_offset = offset + size;
  }

  pthread_mutex_unlock(&ftpfs.lock);

  if (err) return CURLFTPFS_BAD_READ;
  return size;
}

static size_t write_data_bg(void *ptr, size_t size, size_t nmemb, void *data) {
  struct ftpfs_file *fh = data;
  size_t to_copy = size * nmemb;

  if (!fh->isready) {
    sem_post(&fh->ready);
    fh->isready = 1;
  }

  if (fh->stream_buf.len == 0 && fh->written_flag) {
    sem_post(&fh->data_written); /* ftpfs_write can return */
  }

  sem_wait(&fh->data_avail);

  DEBUG(2, "write_data_bg: data_avail eof=%d\n", fh->eof);

  if (fh->eof)
    return 0;

  DEBUG(2, "write_data_bg: %zu %zu\n", to_copy, fh->stream_buf.len);
  if (to_copy > fh->stream_buf.len)
    to_copy = fh->stream_buf.len;

  memcpy(ptr, fh->stream_buf.p, to_copy);
  if (fh->stream_buf.len > to_copy) {
    size_t newlen = fh->stream_buf.len - to_copy;
    memmove(fh->stream_buf.p, fh->stream_buf.p + to_copy, newlen);
    fh->stream_buf.len = newlen;
    sem_post(&fh->data_avail);
    DEBUG(2, "write_data_bg: data_avail\n");

  } else {
    fh->stream_buf.len = 0;
    fh->written_flag = 1;
    sem_post(&fh->data_need);
    DEBUG(2, "write_data_bg: data_need\n");
  }

  return to_copy;
}

int write_thread_ctr = 0;

static void *ftpfs_write_thread(void *data) {
  struct ftpfs_file *fh = data;
  CURLcode curl_res;

  DEBUG(2, "enter streaming write thread #%d path=%s pos=%lld\n", ++write_thread_ctr, fh->full_path, (long long) fh->pos);


  curl_easy_setopt_or_die(fh->write_conn, CURLOPT_URL, fh->full_path);
  curl_easy_setopt_or_die(fh->write_conn, CURLOPT_UPLOAD, 1);
  curl_easy_setopt_or_die(fh->write_conn, CURLOPT_READFUNCTION, write_data_bg);
  curl_easy_setopt_or_die(fh->write_conn, CURLOPT_READDATA, fh);
  curl_easy_setopt_or_die(fh->write_conn, CURLOPT_LOW_SPEED_LIMIT, 1);
  curl_easy_setopt_or_die(fh->write_conn, CURLOPT_LOW_SPEED_TIME, 60);

  fh->curl_error_buffer[0] = '\0';
  curl_easy_setopt_or_die(fh->write_conn, CURLOPT_ERRORBUFFER, fh->curl_error_buffer);

  if (fh->pos > 0) {
    /* resuming a streaming write */
    /*
    char range[15];
    snprintf(range, 15, "%lld-", (long long) fh->pos);
    curl_easy_setopt_or_die(fh->write_conn, CURLOPT_RANGE, range);
    */

    curl_easy_setopt_or_die(fh->write_conn, CURLOPT_APPEND, 1);

    /*curl_easy_setopt_or_die(fh->write_conn, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)fh->pos); */
  }

  curl_res = curl_easy_perform(fh->write_conn);

  curl_easy_setopt_or_die(fh->write_conn, CURLOPT_UPLOAD, 0);

  if (!fh->isready)
    sem_post(&fh->ready);

  if (curl_res != CURLE_OK)
  {
    DEBUG(1, "write problem: %d(%s) text=%s\n", curl_res, curl_easy_strerror(curl_res), fh->curl_error_buffer);
    fh->write_fail_cause = curl_res;
    /* problem - let ftpfs_write continue to avoid hang */
    sem_post(&fh->data_need);
  }

  DEBUG(2, "leaving streaming write thread #%d curl_res=%d\n", write_thread_ctr--, curl_res);

  sem_post(&fh->data_written); /* ftpfs_write may return */

  return NULL;
}

/* returns 1 on success, 0 on failure */
static int start_write_thread(struct ftpfs_file *fh)
{
  if (fh->write_conn != NULL)
  {
    fprintf(stderr, "assert fh->write_conn == NULL failed!\n");
    exit(1);
  }

  fh->written_flag=0;
  fh->isready=0;
  fh->eof=0;
  sem_init(&fh->data_avail, 0, 0);
  sem_init(&fh->data_need, 0, 0);
  sem_init(&fh->data_written, 0, 0);
  sem_init(&fh->ready, 0, 0);

    fh->write_conn = curl_easy_init();
    if (fh->write_conn == NULL) {
      fprintf(stderr, "Error initializing libcurl\n");
      return 0;
    } else {
      int err;
      set_common_curl_stuff(fh->write_conn);
      err = pthread_create(&fh->thread_id, NULL, ftpfs_write_thread, fh);
      if (err) {
        fprintf(stderr, "failed to create thread: %s\n", strerror(err));
        /* FIXME: destroy curl_easy */
        return 0;
      }
    }
  return 1;
}

static int finish_write_thread(struct ftpfs_file *fh)
{
    if (fh->write_fail_cause == CURLE_OK)
    {
      sem_wait(&fh->data_need);  /* only wait when there has been no error */
    }
    sem_post(&fh->data_avail);
    fh->eof = 1;

    pthread_join(fh->thread_id, NULL);
    DEBUG(2, "finish_write_thread after pthread_join. write_fail_cause=%d\n", fh->write_fail_cause);

    curl_easy_cleanup(fh->write_conn);
    fh->write_conn = NULL;

    sem_destroy(&fh->data_avail);
    sem_destroy(&fh->data_need);
    sem_destroy(&fh->data_written);
    sem_destroy(&fh->ready);

    if (fh->write_fail_cause != CURLE_OK)
    {
      return -EIO;
    }
    return 0;
}


static void free_ftpfs_file(struct ftpfs_file *fh) {
  if (fh->write_conn)
    curl_easy_cleanup(fh->write_conn);
  g_free(fh->full_path);
  g_free(fh->open_path);
  sem_destroy(&fh->data_avail);
  sem_destroy(&fh->data_need);
  sem_destroy(&fh->data_written);
  sem_destroy(&fh->ready);
  if (fh->buf.size) { buf_free(&fh->buf); }
  if (fh->stream_buf.size) { buf_free(&fh->stream_buf); }
  free(fh);
}

#if 0

static int buffer_file(struct ftpfs_file *fh) {
  CURLcode curl_res;
  /* If we want to write to the file, we have to load it all at once,
     modify it in memory and then upload it as a whole as most FTP servers
     don't support resume for uploads. */
  pthread_mutex_lock(&ftpfs.lock);
  cancel_previous_multi();
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_URL, fh->full_path);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_WRITEDATA, &fh->buf);
  curl_res = curl_easy_perform(ftpfs.connection);
  pthread_mutex_unlock(&ftpfs.lock);

  if (curl_res != 0) {
    return -EACCES;
  }

  return 0;
}

#endif

static int create_empty_file(const char * path)
{
  int err = 0;
  CURLcode curl_res;
  char *full_path = get_full_path(path);

  pthread_mutex_lock(&ftpfs.lock);
  cancel_previous_multi();
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_URL, full_path);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_INFILESIZE, 0);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_UPLOAD, 1);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_READDATA, NULL);
  curl_res = curl_easy_perform(ftpfs.connection);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_UPLOAD, 0);
  pthread_mutex_unlock(&ftpfs.lock);

  if (curl_res != 0) {
    err = -EPERM;
  }
  free(full_path);
  return err;
}

static int ftpfs_mknod(const char* path, mode_t mode, dev_t rdev);
static int ftpfs_chmod(const char* path, mode_t mode);

static char * flags_to_string(int flags)
{
  const char * access_mode_str = NULL;
  if ((flags & O_ACCMODE) == O_WRONLY)
    access_mode_str = "O_WRONLY";
  else if ((flags & O_ACCMODE) == O_RDWR)
    access_mode_str = "O_RDWR";
  else if ((flags & O_ACCMODE) == O_RDONLY)
    access_mode_str = "O_RDONLY";

  return g_strdup_printf("access_mode=%s, flags=%s%s%s%s",
      access_mode_str,
      (flags & O_CREAT) ? "O_CREAT " : "",
      (flags & O_TRUNC) ? "O_TRUNC " : "",
      (flags & O_EXCL) ? "O_EXCL " : "",
      (flags & O_APPEND) ? "O_APPEND " : "");

}

static int test_exists(const char* path)
{
  struct stat sbuf;
  return ftpfs_getattr(path, &sbuf);
}

static off_t test_size(const char* path)
{
  struct stat sbuf;
  int err = ftpfs_getattr(path, &sbuf);
  if (err)
    return err;
  return sbuf.st_size;
}

static int ftpfs_open_common(const char* path, mode_t mode,
                             struct fuse_file_info* fi) {

  int err = 0;
  struct ftpfs_file* fh;
  char * flagsAsStr = flags_to_string(fi->flags);
  DEBUG(2, "ftpfs_open_common: %s\n", flagsAsStr);

  fh = malloc(sizeof *fh);

  memset(fh, 0, sizeof(*fh));
  buf_init(&fh->buf);
  fh->mode = mode;
  fh->dirty = 0;
  fh->copied = 0;
  fh->last_offset = 0;
  fh->can_shrink = 0;
  buf_init(&fh->stream_buf);
  /* sem_init(&fh->data_avail, 0, 0);
  sem_init(&fh->data_need, 0, 0);
  sem_init(&fh->data_written, 0, 0);
  sem_init(&fh->ready, 0, 0); */
  fh->open_path = strdup(path);
  fh->full_path = get_full_path(path);
  fh->written_flag = 0;
  fh->write_fail_cause = CURLE_OK;
  fh->curl_error_buffer[0] = '\0';
  fh->write_may_start = 0;
  fi->fh = (unsigned long) fh;

  if ((fi->flags & O_ACCMODE) == O_RDONLY) {
    if (fi->flags & O_CREAT) {
      err = ftpfs_mknod(path, (mode & 07777) | S_IFREG, 0);
    } else {
      size_t size;
      /* If it's read-only, we can load the file a bit at a time, as necessary*/
      DEBUG(1, "opening %s O_RDONLY\n", path);
      fh->can_shrink = 1;
      size = ftpfs_read_chunk(fh->full_path, NULL, 1, 0, fi, 0);

      if (size == CURLFTPFS_BAD_READ) {
        DEBUG(1, "initial read failed size=%zu\n", size);
        err = -EACCES;
      }
    }
  }

  else if ((fi->flags & O_ACCMODE) == O_RDWR || (fi->flags & O_ACCMODE) == O_WRONLY)
  {
#ifndef CURLFTPFS_O_RW_WORKAROUND
    if ((fi->flags & O_ACCMODE) == O_RDWR)
    {
      err = -ENOTSUP;
      goto fin;
    }
#endif


    if ((fi->flags & O_APPEND))
    {
      DEBUG(1, "opening %s with O_APPEND - not supported!\n", path);
      err = -ENOTSUP;
    }

    if ((fi->flags & O_EXCL))
    {
      int exists_r;
      DEBUG(1, "opening %s with O_EXCL - testing existence\n", path);
      exists_r = test_exists(path);
      if (exists_r != -ENOENT)
        err = -EACCES;
    }

    if (!err)
    {
      if ((fi->flags & O_CREAT) || (fi->flags & O_TRUNC))
        {
          DEBUG(1, "opening %s for writing with O_CREAT or O_TRUNC. write thread will start now\n", path);


        fh->write_may_start=1;

          if (start_write_thread(fh))
          {
            sem_wait(&fh->ready);
            /* chmod makes only sense on O_CREAT */
            if (fi->flags & O_CREAT) ftpfs_chmod(path, mode);
            sem_post(&fh->data_need);
          }
          else
          {
            err = -EIO;
          }
        }
        else
        {
        /* in this case we have to start writing later */
          DEBUG(1, "opening %s for writing without O_CREAT or O_TRUNC. write thread will start after ftruncate\n", path);
          /* expecting ftruncate */
          fh->write_may_start=0;
        }
    }

  } else {
      err = -EIO;
  }

  fin:
  if (err)
    free_ftpfs_file(fh);

  g_free(flagsAsStr);
  return op_return(err, "ftpfs_open");
}

static int ftpfs_open(const char* path, struct fuse_file_info* fi) {
  return ftpfs_open_common(path, 0, fi);
}

#if FUSE_VERSION >= 25
static int ftpfs_create(const char* path, mode_t mode,
                        struct fuse_file_info* fi) {
  return ftpfs_open_common(path, mode, fi);
}
#endif

static int ftpfs_read(const char* path, char* rbuf, size_t size, off_t offset,
                      struct fuse_file_info* fi) {
  int ret;
  char *full_path;
  size_t size_read;
  struct ftpfs_file *fh = get_ftpfs_file(fi);

  DEBUG(1, "ftpfs_read: %s size=%zu offset=%lld has_write_conn=%d pos=%lld\n", path, size, (long long) offset, fh->write_conn!=0, (long long) fh->pos);

  if (fh->pos>0 || fh->write_conn!=NULL)
  {
    fprintf(stderr, "in read/write mode we cannot read from a file that has already been written to\n");
    return op_return(-EIO, "ftpfs_read");
  }

  full_path = get_full_path(path);
  size_read = ftpfs_read_chunk(full_path, rbuf, size, offset, fi, 1);
  free(full_path);
  if (size_read == CURLFTPFS_BAD_READ) {
    ret = -EIO;
  } else {
    ret = size_read;
  }

  if (ret<0) op_return(ret, "ftpfs_read");
  return ret;
}

static int ftpfs_mknod(const char* path, mode_t mode, dev_t rdev) {
  int err = 0;

  (void) rdev;

  DEBUG(1, "ftpfs_mknode: mode=%d\n", (int)mode);

  if ((mode & S_IFMT) != S_IFREG)
    return -EPERM;

  err = create_empty_file(path);

  if (!err)
      ftpfs_chmod(path, mode);

  return op_return(err, "ftpfs_mknod");
}


static int ftpfs_do_cmd(struct curl_slist *header, const char *path) {
  struct buffer buf;
  const char   *url = NULL;
  CURLcode      curl_res;
  int           err = 0;

  buf_init(&buf);
  if (path)
    url = get_dir_path(path);
  else
    url = ftpfs.host;

  pthread_mutex_lock(&ftpfs.lock);

  cancel_previous_multi();

  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_POSTQUOTE, header);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_URL,       url);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_NOBODY,  ftpfs.safe_nobody);

  curl_res = curl_easy_perform(ftpfs.connection);

  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_POSTQUOTE, NULL);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_NOBODY,    0);

  pthread_mutex_unlock(&ftpfs.lock);

  if (curl_res != 0)
    err = -EPERM;

  free((char *) url);
  buf_free(&buf);

  return err;
}

static int ftpfs_chmod(const char *path, mode_t mode) {
  struct curl_slist *header = NULL;
  const char        *filename, *cmd;
  int                mode_c;
  int                err;

  DEBUG(1, "ftpfs_chmod: %o\n", (int) mode);

  filename = get_file_name(path);
  /* We can only process a subset of the mode - so strip to supported subset */
  mode_c   = mode - (mode / 0x1000 * 0x1000);
  cmd      = g_strdup_printf("SITE CHMOD %.3o %s", mode_c, filename);
  header   = curl_slist_append(header, cmd);

  err = ftpfs_do_cmd(header, path);

  curl_slist_free_all(header);
  free((void *) cmd);
  free((void *) filename);

  return op_return(err, "ftpfs_chmod");
}

static int ftpfs_chown(const char *path, uid_t uid, gid_t gid) {
  struct curl_slist *header = NULL;
  const char        *filename, *cmd, *cmd2;
  int                err;

  DEBUG(1, "ftpfs_chown: %d %d\n", (int) uid, (int) gid);

  filename = get_file_name(path);
  cmd      = g_strdup_printf("SITE CHUID %i %s", uid, filename);
  cmd2     = g_strdup_printf("SITE CHGID %i %s", gid, filename);
  header   = curl_slist_append(header, cmd);
  header   = curl_slist_append(header, cmd2);

  err = ftpfs_do_cmd(header, path);

  curl_slist_free_all(header);
  free((void *) cmd2);
  free((void *) cmd);
  free((void *) filename);

  return op_return(err, "ftpfs_chown");
}

static int ftpfs_truncate(const char* path, off_t offset) {
  off_t size;

  DEBUG(1, "ftpfs_truncate: %s len=%lld\n", path, (long long) offset);
  /* we can't use ftpfs_mknod here, because we don't know the right permissions */
  if (offset == 0) return op_return(create_empty_file(path), "ftpfs_truncate");

  /* fix openoffice problem, truncating exactly to file length */

  size = (long long int)test_size(path);
  DEBUG(1, "ftpfs_truncate: %s check filesize=%lld\n", path, (long long int)size);

  if (offset == size)
    return op_return(0, "ftpfs_ftruncate");

  DEBUG(1, "ftpfs_truncate problem: %s offset != 0 or filesize=%lld != offset\n", path, (long long int)size);


  return op_return(-EPERM, "ftpfs_truncate");
}

static int ftpfs_ftruncate(const char * path , off_t offset, struct fuse_file_info * fi)
{
  off_t size;
  struct ftpfs_file *fh = get_ftpfs_file(fi);

  DEBUG(1, "ftpfs_ftruncate: %s len=%lld\n", path, (long long) offset);
  if (offset == 0)
  {
   if (fh->pos == 0)
   {
     fh->write_may_start=1;
     return op_return(create_empty_file(fh->open_path), "ftpfs_ftruncate");
   }
   return op_return(-EPERM, "ftpfs_ftruncate");
  }
  /* fix openoffice problem, truncating exactly to file length */

  size = test_size(path);
  DEBUG(1, "ftpfs_ftruncate: %s check filesize=%lld\n", path, (long long int)size);

  if (offset == size)
    return op_return(0, "ftpfs_ftruncate");

  DEBUG(1, "ftpfs_ftruncate problem: %s offset != 0 or filesize(=%lld) != offset(=%lld)\n", path, (long long int)size, (long long int) offset);

  return op_return(-EPERM, "ftpfs_ftruncate");
}

static int ftpfs_utime(const char* path, struct utimbuf* time) {
  (void) path;
  (void) time;
  return op_return(0, "ftpfs_utime");
}

static int ftpfs_rmdir(const char *path) {
  struct curl_slist *header = NULL;
  const char        *filename, *cmd;
  int                err;

  DEBUG(1, "ftpfs_rmdir: %s\n", path);

  filename = get_file_name(path);
  cmd      = g_strdup_printf("RMD %s", filename);
  header   = curl_slist_append(header, cmd);

  err = ftpfs_do_cmd(header, path);

  curl_slist_free_all(header);
  free((void *) cmd);
  free((void *) filename);

  return op_return(err, "ftpfs_rmdir");
}

static int ftpfs_mkdir(const char *path, mode_t mode) {
  struct curl_slist *header = NULL;
  const char        *filename, *cmd;
  int                err;

  DEBUG(1, "ftpfs_mkdir: %s %o\n", path, (int) mode);

  filename = get_file_name(path);
  cmd      = g_strdup_printf("MKD %s", filename);
  header   = curl_slist_append(header, cmd);

  err = ftpfs_do_cmd(header, path);

  curl_slist_free_all(header);
  free((void *) cmd);
  free((void *) filename);

  /* XXX Should we propagate the error here? This seems natural, but there are
   * cases where mkdir(2) may succeed even if the mode is not applied (like on
   * VFAT, but note that on VFAT chmod(2) also succeeds even though the mode is
   * not applied). */
  if (!err)
    ftpfs_chmod(path, mode);

  return op_return(err, "ftpfs_mkdir");
}

static int ftpfs_unlink(const char *path) {
  struct curl_slist *header = NULL;
  const char        *filename, *cmd;
  int                err;

  DEBUG(1, "ftpfs_unlink: %s\n", path);

  filename = get_file_name(path);
  cmd      = g_strdup_printf("DELE %s", filename);
  header   = curl_slist_append(header, cmd);

  err = ftpfs_do_cmd(header, path);

  curl_slist_free_all(header);
  free((void *) cmd);
  free((void *) filename);

  return op_return(err, "ftpfs_unlink");
}

static int ftpfs_write(const char *path, const char *wbuf, size_t size,
                       off_t offset, struct fuse_file_info *fi) {
  struct ftpfs_file *fh = get_ftpfs_file(fi);

  (void) path;

  DEBUG(1, "ftpfs_write: %s size=%zu offset=%lld has_write_conn=%d pos=%lld\n", path, size, (long long) offset, fh->write_conn!=0, (long long) fh->pos);

  if (fh->write_fail_cause != CURLE_OK)
  {
    DEBUG(1, "previous write failed. cause=%d\n", fh->write_fail_cause);
    return -EIO;
  }

  if (!fh->write_conn && fh->pos == 0 && offset == 0)
  {
    int success;
    DEBUG(1, "ftpfs_write: starting a streaming write at pos=%lld\n", (long long) fh->pos);

    /* check if the file has been truncated to zero or has been newly created */
    if (!fh->write_may_start)
    {
      long long path_size = (long long int)test_size(path);
      if (path_size != 0)
      {
        fprintf(stderr, "ftpfs_write: start writing with no previous truncate not allowed! size check rval=%lld\n", path_size);
        return op_return(-EIO, "ftpfs_write");
      }
    }

    success = start_write_thread(fh);
    if (!success)
    {
      return op_return(-EIO, "ftpfs_write");
    }
    sem_wait(&fh->ready);
    sem_post(&fh->data_need);
  }

  if (!fh->write_conn && fh->pos >0 && offset == fh->pos)
  {
    int success;
    /* resume a streaming write */
    DEBUG(1, "ftpfs_write: resuming a streaming write at pos=%lld\n", (long long) fh->pos);

    success = start_write_thread(fh);
    if (!success)
    {
      return op_return(-EIO, "ftpfs_write");
    }
    sem_wait(&fh->ready);
    sem_post(&fh->data_need);
  }

  if (fh->write_conn) {
    sem_wait(&fh->data_need);

    if (offset != fh->pos) {
      DEBUG(1, "non-sequential write detected -> fail\n");

      sem_post(&fh->data_avail);
      finish_write_thread(fh);
      return op_return(-EIO, "ftpfs_write");


    } else {
      if (buf_add_mem(&fh->stream_buf, wbuf, size) == -1) {
        sem_post(&fh->data_need);
        return op_return(-ENOMEM, "ftpfs_write");
      }
      fh->pos += size;
      /* wake up write_data_bg */
      sem_post(&fh->data_avail);
      /* wait until libcurl has completely written the current chunk or finished/failed */
      sem_wait(&fh->data_written);
      fh->written_flag = 0;

      if (fh->write_fail_cause != CURLE_OK)
      {
      /* TODO: on error we should problably unlink the target file  */
        DEBUG(1, "writing failed. cause=%d\n", fh->write_fail_cause);
        return op_return(-EIO, "ftpfs_write");
      }
    }

  }

  return size;

}

static int ftpfs_flush(const char *path, struct fuse_file_info *fi) {
  int err = 0;
  struct ftpfs_file* fh = get_ftpfs_file(fi);

  DEBUG(1, "ftpfs_flush: buf.len=%zu buf.pos=%lld write_conn=%d\n", fh->buf.len, (long long) fh->pos, fh->write_conn!=0);

  if (fh->write_conn) {
    struct stat sbuf;

    err = finish_write_thread(fh);
    if (err) return op_return(err, "ftpfs_flush");

    /* check if the resulting file has the correct size
     this is important, because we use APPE for continuing
     writing after a premature flush */
    err = ftpfs_getattr(path, &sbuf);
    if (err) return op_return(err, "ftpfs_flush");

    if (sbuf.st_size != fh->pos)
    {
      fh->write_fail_cause = -999;
      fprintf(stderr, "ftpfs_flush: check filesize problem: size=%lld expected=%lld\n", (long long) sbuf.st_size, (long long) fh->pos);
      return op_return(-EIO, "ftpfs_flush");
    }

    return 0;
  }


  if (!fh->dirty) return 0;

  return op_return(-EIO, "ftpfs_flush");

}

static int ftpfs_fsync(const char *path, int isdatasync,
                      struct fuse_file_info *fi) {
  DEBUG(1, "ftpfs_fsync %s\n", path);
  (void) isdatasync;
  return ftpfs_flush(path, fi);
}

static int ftpfs_release(const char* path, struct fuse_file_info* fi) {
  struct ftpfs_file* fh = get_ftpfs_file(fi);
  DEBUG(1, "ftpfs_release %s\n", path);
  ftpfs_flush(path, fi);
  pthread_mutex_lock(&ftpfs.lock);
  if (ftpfs.current_fh == fh) {
    ftpfs.current_fh = NULL;
  }
  pthread_mutex_unlock(&ftpfs.lock);

  /*
  if (fh->write_conn) {
    finish_write_thread(fh);
  }
  */
  free_ftpfs_file(fh);
  return op_return(0, "ftpfs_release");
}


static int ftpfs_rename(const char *from, const char *to) {
  struct curl_slist *header = NULL;
  char              *rnfr, *rnto;
  int                err;

  DEBUG(1, "ftpfs_rename from %s to %s\n", from, to);

  rnfr   = g_strdup_printf("RNFR %s", from + 1);
  rnto   = g_strdup_printf("RNTO %s", to + 1);
  if (ftpfs.codepage) {
    convert_charsets(ftpfs.iocharset, ftpfs.codepage, &rnfr);
    convert_charsets(ftpfs.iocharset, ftpfs.codepage, &rnto);
  }
  header = curl_slist_append(header, rnfr);
  header = curl_slist_append(header, rnto);

  err = ftpfs_do_cmd(header, NULL);

  curl_slist_free_all(header);
  free(rnto);
  free(rnfr);

  return op_return(err, "ftpfs_rename");
}

static int ftpfs_readlink(const char *path, char *linkbuf, size_t size) {
  int err;
  CURLcode curl_res;
  char *name;
  char* dir_path = get_dir_path(path);
  struct buffer buf;

  DEBUG(2, "dir_path: %s %s\n", path, dir_path);
  buf_init(&buf);

  pthread_mutex_lock(&ftpfs.lock);
  cancel_previous_multi();
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_URL, dir_path);
  curl_easy_setopt_or_die(ftpfs.connection, CURLOPT_WRITEDATA, &buf);
  curl_res = curl_easy_perform(ftpfs.connection);
  pthread_mutex_unlock(&ftpfs.lock);

  if (curl_res != 0) {
    DEBUG(1, "%s\n", error_buf);
  }
  buf_null_terminate(&buf);

  name = strrchr(path, '/');
  ++name;
  err = parse_dir((char*)buf.p, dir_path + strlen(ftpfs.host) - 1,
                  name, NULL, linkbuf, size, NULL, NULL);

  free(dir_path);
  buf_free(&buf);
  if (err) return op_return(-ENOENT, "ftpfs_readlink");
  return op_return(0, "ftpfs_readlink");
}

#if FUSE_VERSION >= 25
static int ftpfs_statfs(const char *path, struct statvfs *buf)
{
    (void) path;

    buf->f_namemax = 255;
    buf->f_bsize = ftpfs.blksize;
    buf->f_frsize = 512;
    buf->f_blocks = 999999999 * 2;
    buf->f_bfree =  999999999 * 2;
    buf->f_bavail = 999999999 * 2;
    buf->f_files =  999999999;
    buf->f_ffree =  999999999;
    return op_return(0, "ftpfs_statfs");
}
#else
static int ftpfs_statfs(const char *path, struct statfs *buf)
{
    (void) path;

    buf->f_namelen = 255;
    buf->f_bsize = ftpfs.blksize;
    buf->f_blocks = 999999999 * 2;
    buf->f_bfree =  999999999 * 2;
    buf->f_bavail = 999999999 * 2;
    buf->f_files =  999999999;
    buf->f_ffree =  999999999;
    return op_return(0, "ftpfs_statfs");
}
#endif

struct fuse_cache_operations ftpfs_oper = {
  .oper = {
/*    .init       = ftpfs_init, */
    .getattr    = ftpfs_getattr,
    .readlink   = ftpfs_readlink,
    .mknod      = ftpfs_mknod,
    .mkdir      = ftpfs_mkdir,
/*    .symlink    = ftpfs_symlink, */
    .unlink     = ftpfs_unlink,
    .rmdir      = ftpfs_rmdir,
    .rename     = ftpfs_rename,
    .chmod      = ftpfs_chmod,
    .chown      = ftpfs_chown,
    .truncate   = ftpfs_truncate,
    .utime      = ftpfs_utime,
    .open       = ftpfs_open,
    .flush      = ftpfs_flush,
    .fsync      = ftpfs_fsync,
    .release    = ftpfs_release,
    .read       = ftpfs_read,
    .write      = ftpfs_write,
    .statfs     = ftpfs_statfs,
#if FUSE_VERSION >= 25
    .create     = ftpfs_create,
    .ftruncate  = ftpfs_ftruncate,
/*    .fgetattr   = ftpfs_fgetattr, */
#endif
  },
  .cache_getdir = ftpfs_getdir,
};

static int ftpfilemethod(const char *str)
{
  if(!strcmp("singlecwd", str))
    return CURLFTPMETHOD_SINGLECWD;
  if(!strcmp("multicwd", str))
    return CURLFTPMETHOD_MULTICWD;
  DEBUG(1, "unrecognized ftp file method '%s', using default\n", str);
  return CURLFTPMETHOD_MULTICWD;
}

void set_common_curl_stuff(CURL* easy) {
  curl_easy_setopt_or_die(easy, CURLOPT_WRITEFUNCTION, read_data);
  curl_easy_setopt_or_die(easy, CURLOPT_READFUNCTION, write_data);
  curl_easy_setopt_or_die(easy, CURLOPT_ERRORBUFFER, error_buf);
  curl_easy_setopt_or_die(easy, CURLOPT_URL, ftpfs.host);
  curl_easy_setopt_or_die(easy, CURLOPT_NETRC, CURL_NETRC_OPTIONAL);
  curl_easy_setopt_or_die(easy, CURLOPT_NOSIGNAL, 1);
  curl_easy_setopt_or_die(easy, CURLOPT_CUSTOMREQUEST, "LIST -a");

  if (ftpfs.custom_list) {
    curl_easy_setopt_or_die(easy, CURLOPT_CUSTOMREQUEST, ftpfs.custom_list);
  }

  if (ftpfs.tryutf8) {
    /* We'll let the slist leak, as it will still be accessible within
       libcurl. If we ever want to add more commands to CURLOPT_QUOTE, we'll
       have to think of a better strategy. */
    struct curl_slist *slist = NULL;

    /* Adding the QUOTE here will make this command be sent with every request.
       This is necessary to ensure that the server is still in UTF8 mode after
       we get disconnected and automatically reconnect. */
    slist = curl_slist_append(slist, "OPTS UTF8 ON");
    curl_easy_setopt_or_die(easy, CURLOPT_QUOTE, slist);
  }

  if (ftpfs.verbose) {
    curl_easy_setopt_or_die(easy, CURLOPT_VERBOSE, TRUE);
  }

  if (ftpfs.disable_epsv) {
    curl_easy_setopt_or_die(easy, CURLOPT_FTP_USE_EPSV, FALSE);
  }

  if (ftpfs.skip_pasv_ip) {
    curl_easy_setopt_or_die(easy, CURLOPT_FTP_SKIP_PASV_IP, TRUE);
  }

  if (ftpfs.ftp_port) {
    curl_easy_setopt_or_die(easy, CURLOPT_FTPPORT, ftpfs.ftp_port);
  }

  if (ftpfs.disable_eprt) {
    curl_easy_setopt_or_die(easy, CURLOPT_FTP_USE_EPRT, FALSE);
  }

  if (ftpfs.ftp_method) {
    curl_easy_setopt_or_die(easy, CURLOPT_FTP_FILEMETHOD,
                            ftpfilemethod(ftpfs.ftp_method));
  }

  if (ftpfs.tcp_nodelay) {
    /* CURLOPT_TCP_NODELAY is not defined in older versions */
    curl_easy_setopt_or_die(easy, CURLOPT_TCP_NODELAY, 1);
  }

  curl_easy_setopt_or_die(easy, CURLOPT_CONNECTTIMEOUT, ftpfs.connect_timeout);

  /* CURLFTPSSL_CONTROL and CURLFTPSSL_ALL should make the connection fail if
   * the server doesn't support SSL but libcurl only honors this beginning
   * with version 7.15.4 */
  if (ftpfs.use_ssl > CURLFTPSSL_TRY &&
      ftpfs.curl_version->version_num <= CURLFTPFS_BAD_SSL) {
    int i;
    const int time_to_wait = 10;
    fprintf(stderr,
"WARNING: you are using libcurl %s.\n"
"This version of libcurl does not respect the mandatory SSL flag.\n"
"It will try to send the user and password even if the server doesn't support\n"
"SSL. Please upgrade to libcurl version 7.15.4 or higher.\n"
"You can abort the connection now by pressing ctrl+c.\n",
            ftpfs.curl_version->version);
    for (i = 0; i < time_to_wait; i++) {
      fprintf(stderr, "%d.. ", time_to_wait - i);
      sleep(1);
    }
    fprintf(stderr, "\n");
  }
  curl_easy_setopt_or_die(easy, CURLOPT_FTP_SSL, ftpfs.use_ssl);

  curl_easy_setopt_or_die(easy, CURLOPT_SSLCERT, ftpfs.cert);
  curl_easy_setopt_or_die(easy, CURLOPT_SSLCERTTYPE, ftpfs.cert_type);
  curl_easy_setopt_or_die(easy, CURLOPT_SSLKEY, ftpfs.key);
  curl_easy_setopt_or_die(easy, CURLOPT_SSLKEYTYPE, ftpfs.key_type);
  curl_easy_setopt_or_die(easy, CURLOPT_SSLKEYPASSWD, ftpfs.key_password);

  if (ftpfs.engine) {
    curl_easy_setopt_or_die(easy, CURLOPT_SSLENGINE, ftpfs.engine);
    curl_easy_setopt_or_die(easy, CURLOPT_SSLENGINE_DEFAULT, 1);
  }

  curl_easy_setopt_or_die(easy, CURLOPT_SSL_VERIFYPEER, TRUE);
  if (ftpfs.no_verify_peer) {
    curl_easy_setopt_or_die(easy, CURLOPT_SSL_VERIFYPEER, FALSE);
  }

  if (ftpfs.cacert || ftpfs.capath) {
    if (ftpfs.cacert) {
      curl_easy_setopt_or_die(easy, CURLOPT_CAINFO, ftpfs.cacert);
    }
    if (ftpfs.capath) {
      curl_easy_setopt_or_die(easy, CURLOPT_CAPATH, ftpfs.capath);
    }
  }

  if (ftpfs.ciphers) {
    curl_easy_setopt_or_die(easy, CURLOPT_SSL_CIPHER_LIST, ftpfs.ciphers);
  }

  if (ftpfs.no_verify_hostname) {
  /* The default is 2 which verifies even the host string. When the value
   * is 0, the connection succeeds regardless of the names in the certificate.
   * http://curl.haxx.se/libcurl/c/curl_easy_setopt.html#CURLOPTSSLVERIFYHOST */
    curl_easy_setopt_or_die(easy, CURLOPT_SSL_VERIFYHOST, 0);
  }

  curl_easy_setopt_or_die(easy, CURLOPT_INTERFACE, ftpfs.interface);
  curl_easy_setopt_or_die(easy, CURLOPT_KRB4LEVEL, ftpfs.krb4);

  if (ftpfs.proxy) {
    curl_easy_setopt_or_die(easy, CURLOPT_PROXY, ftpfs.proxy);
  }

  /* The default proxy type is HTTP */
  if (!ftpfs.proxytype) {
    ftpfs.proxytype = CURLPROXY_HTTP;
  }
  curl_easy_setopt_or_die(easy, CURLOPT_PROXYTYPE, ftpfs.proxytype);

  /* Connection to FTP servers only make sense with a HTTP tunnel proxy */
  if (ftpfs.proxytype == CURLPROXY_HTTP || ftpfs.proxytunnel) {
    curl_easy_setopt_or_die(easy, CURLOPT_HTTPPROXYTUNNEL, TRUE);
  }

  if (ftpfs.proxyanyauth) {
    curl_easy_setopt_or_die(easy, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
  } else if (ftpfs.proxyntlm) {
    curl_easy_setopt_or_die(easy, CURLOPT_PROXYAUTH, CURLAUTH_NTLM);
  } else if (ftpfs.proxydigest) {
    curl_easy_setopt_or_die(easy, CURLOPT_PROXYAUTH, CURLAUTH_DIGEST);
  } else if (ftpfs.proxybasic) {
    curl_easy_setopt_or_die(easy, CURLOPT_PROXYAUTH, CURLAUTH_BASIC);
  }

  curl_easy_setopt_or_die(easy, CURLOPT_USERPWD, ftpfs.user);
  curl_easy_setopt_or_die(easy, CURLOPT_PROXYUSERPWD, ftpfs.proxy_user);
  curl_easy_setopt_or_die(easy, CURLOPT_SSLVERSION, ftpfs.ssl_version);
  curl_easy_setopt_or_die(easy, CURLOPT_IPRESOLVE, ftpfs.ip_version);
}

