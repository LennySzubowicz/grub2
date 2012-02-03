/* getroot.c - Get root device */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 1999,2000,2001,2002,2003,2006,2007,2008,2009,2010,2011  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config-util.h>
#include <config.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <error.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#include <grub/util/misc.h>
#include <grub/util/lvm.h>
#include <grub/cryptodisk.h>
#include <grub/i18n.h>

#ifdef HAVE_DEVICE_MAPPER
# include <libdevmapper.h>
#endif

#ifdef __GNU__
#include <hurd.h>
#include <hurd/lookup.h>
#include <hurd/fs.h>
#include <sys/mman.h>
#endif

#ifdef __linux__
# include <sys/types.h>
# include <sys/wait.h>
#endif

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
# include <sys/mount.h>
#endif

#if defined(HAVE_LIBZFS) && defined(HAVE_LIBNVPAIR)
# include <grub/util/libzfs.h>
# include <grub/util/libnvpair.h>
#endif

#ifdef __sun__
# include <sys/types.h>
# include <sys/mkdev.h>
#endif

#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/emu/misc.h>
#include <grub/emu/hostdisk.h>
#include <grub/emu/getroot.h>

#ifdef __linux__
# include <sys/ioctl.h>         /* ioctl */
# include <sys/mount.h>
# ifndef MAJOR
#  ifndef MINORBITS
#   define MINORBITS	8
#  endif /* ! MINORBITS */
#  define MAJOR(dev)	((unsigned) ((dev) >> MINORBITS))
# endif /* ! MAJOR */
# ifndef FLOPPY_MAJOR
#  define FLOPPY_MAJOR	2
# endif /* ! FLOPPY_MAJOR */
#endif

#ifdef __CYGWIN__
# include <sys/ioctl.h>
# include <cygwin/fs.h> /* BLKGETSIZE64 */
# include <cygwin/hdreg.h> /* HDIO_GETGEO */
# define MAJOR(dev)	((unsigned) ((dev) >> 16))
# define FLOPPY_MAJOR	2
#endif

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
# include <sys/disk.h> /* DIOCGMEDIASIZE */
# include <sys/param.h>
# include <sys/sysctl.h>
# include <sys/mount.h>
#include <libgeom.h>
# define MAJOR(dev) major(dev)
# define FLOPPY_MAJOR	2
#endif

#if defined (__sun__)
# include <sys/dkio.h>
#endif

#if defined(__APPLE__)
# include <sys/disk.h>
#endif

#ifdef HAVE_DEVICE_MAPPER
# include <libdevmapper.h>
#endif

#if defined(__NetBSD__)
# define HAVE_DIOCGDINFO
# include <sys/ioctl.h>
# include <sys/disklabel.h>    /* struct disklabel */
# include <sys/disk.h>    /* struct dkwedge_info */
#else /* !defined(__NetBSD__) && !defined(__FreeBSD__) && !defined(__FreeBSD_kernel__) */
# undef HAVE_DIOCGDINFO
#endif /* defined(__NetBSD__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) */

#if defined(__NetBSD__)
# ifdef HAVE_GETRAWPARTITION
#  include <util.h>    /* getrawpartition */
# endif /* HAVE_GETRAWPARTITION */
# include <sys/fdio.h>
# ifndef FLOPPY_MAJOR
#  define FLOPPY_MAJOR	2
# endif /* ! FLOPPY_MAJOR */
# ifndef RAW_FLOPPY_MAJOR
#  define RAW_FLOPPY_MAJOR	9
# endif /* ! RAW_FLOPPY_MAJOR */
#endif /* defined(__NetBSD__) */

static void
strip_extra_slashes (char *dir)
{
  char *p = dir;

  while ((p = strchr (p, '/')) != 0)
    {
      if (p[1] == '/')
	{
	  memmove (p, p + 1, strlen (p));
	  continue;
	}
      else if (p[1] == '\0')
	{
	  if (p > dir)
	    p[0] = '\0';
	  break;
	}

      p++;
    }
}

static char *
xgetcwd (void)
{
  size_t size = 10;
  char *path;

  path = xmalloc (size);
  while (! getcwd (path, size))
    {
      size <<= 1;
      path = xrealloc (path, size);
    }

  return path;
}

static char **
find_root_devices_from_poolname (char *poolname)
{
  char **devices = 0;
  size_t ndevices = 0;
  size_t devices_allocated = 0;

#if defined(HAVE_LIBZFS) && defined(HAVE_LIBNVPAIR)
  zpool_handle_t *zpool;
  libzfs_handle_t *libzfs;
  nvlist_t *config, *vdev_tree;
  nvlist_t **children, **path;
  unsigned int nvlist_count;
  unsigned int i;
  char *device = 0;

  libzfs = grub_get_libzfs_handle ();
  if (! libzfs)
    return NULL;

  zpool = zpool_open (libzfs, poolname);
  config = zpool_get_config (zpool, NULL);

  if (nvlist_lookup_nvlist (config, "vdev_tree", &vdev_tree) != 0)
    error (1, errno, "nvlist_lookup_nvlist (\"vdev_tree\")");

  if (nvlist_lookup_nvlist_array (vdev_tree, "children", &children, &nvlist_count) != 0)
    error (1, errno, "nvlist_lookup_nvlist_array (\"children\")");
  assert (nvlist_count > 0);

  while (nvlist_lookup_nvlist_array (children[0], "children",
				     &children, &nvlist_count) == 0)
    assert (nvlist_count > 0);

  for (i = 0; i < nvlist_count; i++)
    {
      if (nvlist_lookup_string (children[i], "path", &device) != 0)
	error (1, errno, "nvlist_lookup_string (\"path\")");

      struct stat st;
      if (stat (device, &st) == 0)
	{
#ifdef __sun__
	  if (grub_memcmp (device, "/dev/dsk/", sizeof ("/dev/dsk/") - 1)
	      == 0)
	    device = xasprintf ("/dev/rdsk/%s",
				device + sizeof ("/dev/dsk/") - 1);
	  else if (grub_memcmp (device, "/devices", sizeof ("/devices") - 1)
		   == 0
		   && grub_memcmp (device + strlen (device) - 4,
				   ",raw", 4) != 0)
	    device = xasprintf ("%s,raw", device);
	  else
#endif
	    device = xstrdup (device);
	  if (ndevices >= devices_allocated)
	    {
	      devices_allocated = 2 * (devices_allocated + 8);
	      devices = xrealloc (devices, sizeof (devices[0])
				  * devices_allocated);
	    }
	  devices[ndevices++] = device;
	}

      device = NULL;
    }

  zpool_close (zpool);
#else
  char *cmd;
  FILE *fp;
  int ret;
  char *line;
  size_t len;
  int st;

  char name[PATH_MAX + 1], state[257], readlen[257], writelen[257];
  char cksum[257], notes[257];
  unsigned int dummy;

  cmd = xasprintf ("zpool status %s", poolname);
  fp = popen (cmd, "r");
  free (cmd);

  st = 0;
  while (1)
    {
      line = NULL;
      ret = getline (&line, &len, fp);
      if (ret == -1)
	break;
	
      if (sscanf (line, " %s %256s %256s %256s %256s %256s",
		  name, state, readlen, writelen, cksum, notes) >= 5)
	switch (st)
	  {
	  case 0:
	    if (!strcmp (name, "NAME")
		&& !strcmp (state, "STATE")
		&& !strcmp (readlen, "READ")
		&& !strcmp (writelen, "WRITE")
		&& !strcmp (cksum, "CKSUM"))
	      st++;
	    break;
	  case 1:
	    if (!strcmp (name, poolname))
	      st++;
	    break;
	  case 2:
	    if (strcmp (name, "mirror") && !sscanf (name, "mirror-%u", &dummy)
		&& !sscanf (name, "raidz%u", &dummy)
		&& !sscanf (name, "raidz1%u", &dummy)
		&& !sscanf (name, "raidz2%u", &dummy)
		&& !sscanf (name, "raidz3%u", &dummy)
		&& !strcmp (state, "ONLINE"))
	      {
		if (ndevices >= devices_allocated)
		  {
		    devices_allocated = 2 * (devices_allocated + 8);
		    devices = xrealloc (devices, sizeof (devices[0])
					* devices_allocated);
		  }
		devices[ndevices++] = xasprintf ("/dev/%s", name);
	      }
	    break;
	  }
	
      free (line);
    }

  pclose (fp);
#endif
  if (devices)
    {
      if (ndevices >= devices_allocated)
	{
	  devices_allocated = 2 * (devices_allocated + 8);
	  devices = xrealloc (devices, sizeof (devices[0])
			      * devices_allocated);
	}
      devices[ndevices++] = 0;
    }
  return devices;
}

#ifdef __linux__

#define ESCAPED_PATH_MAX (4 * PATH_MAX)
struct mountinfo_entry
{
  int id;
  int major, minor;
  char enc_root[ESCAPED_PATH_MAX + 1], enc_path[ESCAPED_PATH_MAX + 1];
  char fstype[ESCAPED_PATH_MAX + 1], device[ESCAPED_PATH_MAX + 1];
};

/* Statting something on a btrfs filesystem always returns a virtual device
   major/minor pair rather than the real underlying device, because btrfs
   can span multiple underlying devices (and even if it's currently only
   using a single device it can be dynamically extended onto another).  We
   can't deal with the multiple-device case yet, but in the meantime, we can
   at least cope with the single-device case by scanning
   /proc/self/mountinfo.  */
static void
unescape (char *str)
{
  char *optr;
  const char *iptr;
  for (iptr = optr = str; *iptr; optr++)
    {
      if (iptr[0] == '\\' && iptr[1] >= '0' && iptr[1] < '8'
	  && iptr[2] >= '0' && iptr[2] < '8'
	  && iptr[3] >= '0' && iptr[3] < '8')
	{
	  *optr = (((iptr[1] - '0') << 6) | ((iptr[2] - '0') << 3)
		   | (iptr[3] - '0'));
	  iptr += 4;
	}
      else
	*optr = *iptr++;
    }
  *optr = 0;
}

static char **
grub_find_root_devices_from_mountinfo (const char *dir, char **relroot)
{
  FILE *fp;
  char *buf = NULL;
  size_t len = 0;
  char **ret = NULL;
  int entry_len = 0, entry_max = 4;
  struct mountinfo_entry *entries;
  struct mountinfo_entry parent_entry = { 0, 0, 0, "", "", "", "" };
  int i;

  if (! *dir)
    dir = "/";
  if (relroot)
    *relroot = NULL;

  fp = fopen ("/proc/self/mountinfo", "r");
  if (! fp)
    return NULL; /* fall through to other methods */

  entries = xmalloc (entry_max * sizeof (*entries));

  /* First, build a list of relevant visible mounts.  */
  while (getline (&buf, &len, fp) > 0)
    {
      struct mountinfo_entry entry;
      int count;
      size_t enc_path_len;
      const char *sep;

      if (sscanf (buf, "%d %d %u:%u %s %s%n",
		  &entry.id, &parent_entry.id, &entry.major, &entry.minor,
		  entry.enc_root, entry.enc_path, &count) < 6)
	continue;

      unescape (entry.enc_root);
      unescape (entry.enc_path);

      enc_path_len = strlen (entry.enc_path);
      /* Check that enc_path is a prefix of dir.  The prefix must either be
         the entire string, or end with a slash, or be immediately followed
         by a slash.  */
      if (strncmp (dir, entry.enc_path, enc_path_len) != 0 ||
	  (enc_path_len && dir[enc_path_len - 1] != '/' &&
	   dir[enc_path_len] && dir[enc_path_len] != '/'))
	continue;

      sep = strstr (buf + count, " - ");
      if (!sep)
	continue;

      sep += sizeof (" - ") - 1;
      if (sscanf (sep, "%s %s", entry.fstype, entry.device) != 2)
	continue;

      /* Using the mount IDs, find out where this fits in the list of
	 visible mount entries we've seen so far.  There are three
	 interesting cases.  Firstly, it may be inserted at the end: this is
	 the usual case of /foo/bar being mounted after /foo.  Secondly, it
	 may be inserted at the start: for example, this can happen for
	 filesystems that are mounted before / and later moved under it.
	 Thirdly, it may occlude part or all of the existing filesystem
	 tree, in which case the end of the list needs to be pruned and this
	 new entry will be inserted at the end.  */
      if (entry_len >= entry_max)
	{
	  entry_max <<= 1;
	  entries = xrealloc (entries, entry_max * sizeof (*entries));
	}

      if (!entry_len)
	{
	  /* Initialise list.  */
	  entry_len = 2;
	  entries[0] = parent_entry;
	  entries[1] = entry;
	}
      else
	{
	  for (i = entry_len - 1; i >= 0; i--)
	    {
	      if (entries[i].id == parent_entry.id)
		{
		  /* Insert at end, pruning anything previously above this.  */
		  entry_len = i + 2;
		  entries[i + 1] = entry;
		  break;
		}
	      else if (i == 0 && entries[i].id == entry.id)
		{
		  /* Insert at start.  */
		  entry_len++;
		  memmove (entries + 1, entries,
			   (entry_len - 1) * sizeof (*entries));
		  entries[0] = parent_entry;
		  entries[1] = entry;
		  break;
		}
	    }
	}
    }

  /* Now scan visible mounts for the ones we're interested in.  */
  for (i = entry_len - 1; i >= 0; i--)
    {
      if (!*entries[i].device)
	continue;

      if (grub_strcmp (entries[i].fstype, "fuse.zfs") == 0)
	{
	  char *slash;
	  slash = strchr (entries[i].device, '/');
	  if (slash)
	    *slash = 0;
	  ret = find_root_devices_from_poolname (entries[i].device);
	  if (slash)
	    *slash = '/';
	  if (relroot)
	    {
	      if (!slash)
		*relroot = xasprintf ("/@%s", entries[i].enc_root);
	      else if (strchr (slash + 1, '@'))
		*relroot = xasprintf ("/%s%s", slash + 1, entries[i].enc_root);
	      else
		*relroot = xasprintf ("/%s@%s", slash + 1, entries[i].enc_root);
	    }
	}
      else
	{
	  ret = xmalloc (2 * sizeof (ret[0]));
	  ret[0] = strdup (entries[i].device);
	  ret[1] = 0;
	  if (relroot)
	    *relroot = strdup (entries[i].enc_root);
	}
      break;
    }

  free (buf);
  free (entries);
  fclose (fp);
  return ret;
}

#endif /* __linux__ */

static char **
find_root_devices_from_libzfs (const char *dir)
{
  char **devices = NULL;
  char *poolname;
  char *poolfs;

  grub_find_zpool_from_dir (dir, &poolname, &poolfs);
  if (! poolname)
    return NULL;

  devices = find_root_devices_from_poolname (poolname);

  free (poolname);
  if (poolfs)
    free (poolfs);

  return devices;
}

#ifdef __MINGW32__

char *
grub_find_device (const char *dir __attribute__ ((unused)),
                  dev_t dev __attribute__ ((unused)))
{
  return 0;
}

#elif ! defined(__CYGWIN__)

char *
grub_find_device (const char *dir, dev_t dev)
{
  DIR *dp;
  char *saved_cwd;
  struct dirent *ent;

  if (! dir)
    {
#ifdef __CYGWIN__
      return NULL;
#else
      dir = "/dev";
#endif
    }

  dp = opendir (dir);
  if (! dp)
    return 0;

  saved_cwd = xgetcwd ();

  grub_util_info ("changing current directory to %s", dir);
  if (chdir (dir) < 0)
    {
      free (saved_cwd);
      closedir (dp);
      return 0;
    }

  while ((ent = readdir (dp)) != 0)
    {
      struct stat st;

      /* Avoid:
	 - dotfiles (like "/dev/.tmp.md0") since they could be duplicates.
	 - dotdirs (like "/dev/.static") since they could contain duplicates.  */
      if (ent->d_name[0] == '.')
	continue;

      if (lstat (ent->d_name, &st) < 0)
	/* Ignore any error.  */
	continue;

      if (S_ISLNK (st.st_mode)) {
#ifdef __linux__
	if (strcmp (dir, "mapper") == 0 || strcmp (dir, "/dev/mapper") == 0) {
	  /* Follow symbolic links under /dev/mapper/; the canonical name
	     may be something like /dev/dm-0, but the names under
	     /dev/mapper/ are more human-readable and so we prefer them if
	     we can get them.  */
	  if (stat (ent->d_name, &st) < 0)
	    continue;
	} else
#endif /* __linux__ */
	/* Don't follow other symbolic links.  */
	continue;
      }

      if (S_ISDIR (st.st_mode))
	{
	  /* Find it recursively.  */
	  char *res;

	  res = grub_find_device (ent->d_name, dev);

	  if (res)
	    {
	      if (chdir (saved_cwd) < 0)
		grub_util_error (_("cannot restore the original directory"));

	      free (saved_cwd);
	      closedir (dp);
	      return res;
	    }
	}

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__APPLE__)
      if (S_ISCHR (st.st_mode) && st.st_rdev == dev)
#else
      if (S_ISBLK (st.st_mode) && st.st_rdev == dev)
#endif
	{
#ifdef __linux__
	  /* Skip device names like /dev/dm-0, which are short-hand aliases
	     to more descriptive device names, e.g. those under /dev/mapper */
	  if (ent->d_name[0] == 'd' &&
	      ent->d_name[1] == 'm' &&
	      ent->d_name[2] == '-' &&
	      ent->d_name[3] >= '0' &&
	      ent->d_name[3] <= '9')
	    continue;
#endif

	  /* Found!  */
	  char *res;
	  char *cwd;
#if defined(__NetBSD__)
	  /* Convert this block device to its character (raw) device.  */
	  const char *template = "%s/r%s";
#else
	  /* Keep the device name as it is.  */
	  const char *template = "%s/%s";
#endif

	  cwd = xgetcwd ();
	  res = xmalloc (strlen (cwd) + strlen (ent->d_name) + 3);
	  sprintf (res, template, cwd, ent->d_name);
	  strip_extra_slashes (res);
	  free (cwd);

	  /* /dev/root is not a real block device keep looking, takes care
	     of situation where root filesystem is on the same partition as
	     grub files */

	  if (strcmp(res, "/dev/root") == 0)
		continue;

	  if (chdir (saved_cwd) < 0)
	    grub_util_error (_("cannot restore the original directory"));

	  free (saved_cwd);
	  closedir (dp);
	  return res;
	}
    }

  if (chdir (saved_cwd) < 0)
    grub_util_error (_("cannot restore the original directory"));

  free (saved_cwd);
  closedir (dp);
  return 0;
}

#else /* __CYGWIN__ */

/* Read drive/partition serial number from mbr/boot sector,
   return 0 on read error, ~0 on unknown serial.  */
static unsigned
get_bootsec_serial (const char *os_dev, int mbr)
{
  /* Read boot sector.  */
  int fd = open (os_dev, O_RDONLY);
  if (fd < 0)
    return 0;
  unsigned char buf[0x200];
  int n = read (fd, buf, sizeof (buf));
  close (fd);
  if (n != sizeof(buf))
    return 0;

  /* Check signature.  */
  if (!(buf[0x1fe] == 0x55 && buf[0x1ff] == 0xaa))
    return ~0;

  /* Serial number offset depends on boot sector type.  */
  if (mbr)
    n = 0x1b8;
  else if (memcmp (buf + 0x03, "NTFS", 4) == 0)
    n = 0x048;
  else if (memcmp (buf + 0x52, "FAT32", 5) == 0)
    n = 0x043;
  else if (memcmp (buf + 0x36, "FAT", 3) == 0)
    n = 0x027;
  else
    return ~0;

  unsigned serial = *(unsigned *)(buf + n);
  if (serial == 0)
    return ~0;
  return serial;
}

char *
grub_find_device (const char *path, dev_t dev)
{
  /* No root device for /cygdrive.  */
  if (dev == (DEV_CYGDRIVE_MAJOR << 16))
    return 0;

  /* Convert to full POSIX and Win32 path.  */
  char fullpath[PATH_MAX], winpath[PATH_MAX];
  cygwin_conv_to_full_posix_path (path, fullpath);
  cygwin_conv_to_full_win32_path (fullpath, winpath);

  /* If identical, this is no real filesystem path.  */
  if (strcmp (fullpath, winpath) == 0)
    return 0;

  /* Check for floppy drive letter.  */
  if (winpath[0] && winpath[1] == ':' && strchr ("AaBb", winpath[0]))
    return xstrdup (strchr ("Aa", winpath[0]) ? "/dev/fd0" : "/dev/fd1");

  /* Cygwin returns the partition serial number in stat.st_dev.
     This is never identical to the device number of the emulated
     /dev/sdXN device, so above grub_find_device () does not work.
     Search the partition with the same serial in boot sector instead.  */
  char devpath[sizeof ("/dev/sda15") + 13]; /* Size + Paranoia.  */
  int d;
  for (d = 'a'; d <= 'z'; d++)
    {
      sprintf (devpath, "/dev/sd%c", d);
      if (get_bootsec_serial (devpath, 1) == 0)
	continue;
      int p;
      for (p = 1; p <= 15; p++)
	{
	  sprintf (devpath, "/dev/sd%c%d", d, p);
	  unsigned ser = get_bootsec_serial (devpath, 0);
	  if (ser == 0)
	    break;
	  if (ser != (unsigned)~0 && dev == (dev_t)ser)
	    return xstrdup (devpath);
	}
    }
  return 0;
}

#endif /* __CYGWIN__ */

char **
grub_guess_root_devices (const char *dir)
{
  char **os_dev = NULL;
#ifdef __GNU__
  file_t file;
  mach_port_t *ports;
  int *ints;
  loff_t *offsets;
  char *data;
  error_t err;
  mach_msg_type_number_t num_ports = 0, num_ints = 0, num_offsets = 0, data_len = 0;
  size_t name_len;

  file = file_name_lookup (dir, 0, 0);
  if (file == MACH_PORT_NULL)
    return 0;

  err = file_get_storage_info (file,
			       &ports, &num_ports,
			       &ints, &num_ints,
			       &offsets, &num_offsets,
			       &data, &data_len);

  if (num_ints < 1)
    grub_util_error (_("Storage info for `%s' does not include type"), dir);
  if (ints[0] != STORAGE_DEVICE)
    grub_util_error (_("Filesystem of `%s' is not stored on local disk"), dir);

  if (num_ints < 5)
    grub_util_error (_("Storage info for `%s' does not include name"), dir);
  name_len = ints[4];
  if (name_len < data_len)
    grub_util_error (_("Bogus name length for storage info for `%s'"), dir);
  if (data[name_len - 1] != '\0')
    grub_util_error (_("Storage name for `%s' not NUL-terminated"), dir);

  os_dev = xmalloc (2 * sizeof (os_dev[0]));
  os_dev[0] = xmalloc (sizeof ("/dev/") - 1 + data_len);
  memcpy (os_dev[0], "/dev/", sizeof ("/dev/") - 1);
  memcpy (os_dev[0] + sizeof ("/dev/") - 1, data, data_len);
  os_dev[1] = 0;

  if (ports && num_ports > 0)
    {
      mach_msg_type_number_t i;
      for (i = 0; i < num_ports; i++)
        {
	  mach_port_t port = ports[i];
	  if (port != MACH_PORT_NULL)
	    mach_port_deallocate (mach_task_self(), port);
        }
      munmap ((caddr_t) ports, num_ports * sizeof (*ports));
    }

  if (ints && num_ints > 0)
    munmap ((caddr_t) ints, num_ints * sizeof (*ints));
  if (offsets && num_offsets > 0)
    munmap ((caddr_t) offsets, num_offsets * sizeof (*offsets));
  if (data && data_len > 0)
    munmap (data, data_len);
  mach_port_deallocate (mach_task_self (), file);
#else /* !__GNU__ */
  struct stat st;
  dev_t dev;

#ifdef __linux__
  if (!os_dev)
    os_dev = grub_find_root_devices_from_mountinfo (dir, NULL);
#endif /* __linux__ */

  if (!os_dev)
    os_dev = find_root_devices_from_libzfs (dir);

  if (os_dev)
    {
      char **cur;
      for (cur = os_dev; *cur; cur++)
	{
	  char *tmp = *cur;
	  int root, dm;
	  *cur = canonicalize_file_name (tmp);
	  if (*cur == NULL)
	    grub_util_error (_("failed to get canonical path of %s"), tmp);
	  free (tmp);
	  root = (strcmp (*cur, "/dev/root") == 0);
	  dm = (strncmp (*cur, "/dev/dm-", sizeof ("/dev/dm-") - 1) == 0);
	  if (!dm && !root)
	    continue;
	  if (stat (*cur, &st) < 0)
	    break;
	  free (*cur);
	  dev = st.st_rdev;
	  *cur = grub_find_device (dm ? "/dev/mapper" : "/dev", dev);
	}
      if (!*cur)
	return os_dev;
      for (cur = os_dev; *cur; cur++)
	free (*cur);
      free (os_dev);
      os_dev = 0;
    }

  if (stat (dir, &st) < 0)
    grub_util_error (_("cannot stat `%s'"), dir);

  dev = st.st_dev;

  os_dev = xmalloc (2 * sizeof (os_dev[0]));
  
#ifdef __CYGWIN__
  /* Cygwin specific function.  */
  os_dev[0] = grub_find_device (dir, dev);

#else

  /* This might be truly slow, but is there any better way?  */
  os_dev[0] = grub_find_device ("/dev", dev);
#endif
  os_dev[1] = 0;
#endif /* !__GNU__ */

  return os_dev;
}

#ifdef HAVE_DEVICE_MAPPER

static int
grub_util_open_dm (const char *os_dev, struct dm_tree **tree,
		   struct dm_tree_node **node)
{
  uint32_t maj, min;
  struct stat st;

  *node = NULL;
  *tree = NULL;

  if ((strncmp ("/dev/mapper/", os_dev, 12) != 0))
    return 0;

  if (stat (os_dev, &st) < 0)
    return 0;

  *tree = dm_tree_create ();
  if (! *tree)
    {
      grub_puts_ (N_("Failed to create tree"));
      grub_dprintf ("hostdisk", "dm_tree_create failed\n");
      return 0;
    }

  maj = major (st.st_rdev);
  min = minor (st.st_rdev);

  if (! dm_tree_add_dev (*tree, maj, min))
    {
      grub_dprintf ("hostdisk", "dm_tree_add_dev failed\n");
      dm_tree_free (*tree);
      *tree = NULL;
      return 0;
    }

  *node = dm_tree_find_node (*tree, maj, min);
  if (! *node)
    {
      grub_dprintf ("hostdisk", "dm_tree_find_node failed\n");
      dm_tree_free (*tree);
      *tree = NULL;
      return 0;
    }
  return 1;
}

#endif

static char *
get_dm_uuid (const char *os_dev)
{
  if ((strncmp ("/dev/mapper/", os_dev, 12) != 0))
    return NULL;
  
#ifdef HAVE_DEVICE_MAPPER
  {
    struct dm_tree *tree;
    struct dm_tree_node *node;
    const char *node_uuid;
    char *ret;

    if (!grub_util_open_dm (os_dev, &tree, &node))
      return NULL;

    node_uuid = dm_tree_node_get_uuid (node);
    if (! node_uuid)
      {
	grub_dprintf ("hostdisk", "%s has no DM uuid\n", os_dev);
	dm_tree_free (tree);
	return NULL;
      }

    ret = grub_strdup (node_uuid);

    dm_tree_free (tree);

    return ret;
  }
#endif

  return NULL;
}

static enum grub_dev_abstraction_types
grub_util_get_dm_abstraction (const char *os_dev)
{
#ifdef HAVE_DEVICE_MAPPER
  char *uuid;

  uuid = get_dm_uuid (os_dev);

  if (uuid == NULL)
    return GRUB_DEV_ABSTRACTION_NONE;

  if (strncmp (uuid, "LVM-", 4) == 0)
    {
      grub_free (uuid);
      return GRUB_DEV_ABSTRACTION_LVM;
    }
  if (strncmp (uuid, "CRYPT-LUKS1-", 4) == 0)
    {
      grub_free (uuid);
      return GRUB_DEV_ABSTRACTION_LUKS;
    }

  grub_free (uuid);
  return GRUB_DEV_ABSTRACTION_NONE;
#else
  if ((strncmp ("/dev/mapper/", os_dev, 12) != 0))
    return GRUB_DEV_ABSTRACTION_NONE;
  return GRUB_DEV_ABSTRACTION_LVM;  
#endif
}

#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <libgeom.h>

static const char *
grub_util_get_geom_abstraction (const char *dev)
{
  char *whole;
  struct gmesh mesh;
  struct gclass *class;
  const char *name;
  int error;

  if (strncmp (dev, "/dev/", sizeof ("/dev/") - 1) != 0)
    return 0;
  name = dev + sizeof ("/dev/") - 1;
  grub_util_follow_gpart_up (name, NULL, &whole);

  grub_util_info ("following geom '%s'", name);

  error = geom_gettree (&mesh);
  if (error != 0)
    grub_util_error (_("couldn't open geom"));

  LIST_FOREACH (class, &mesh.lg_class, lg_class)
    {
      struct ggeom *geom;
      LIST_FOREACH (geom, &class->lg_geom, lg_geom)
	{ 
	  struct gprovider *provider;
	  LIST_FOREACH (provider, &geom->lg_provider, lg_provider)
	    if (strcmp (provider->lg_name, name) == 0)
	      return class->lg_name;
	}
    }
  return NULL;
}
#endif

int
grub_util_get_dev_abstraction (const char *os_dev)
{
#if defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
  /* User explicitly claims that this drive is visible by BIOS.  */
  if (grub_util_biosdisk_is_present (os_dev))
    return GRUB_DEV_ABSTRACTION_NONE;
#endif

#ifdef __linux__
  enum grub_dev_abstraction_types ret;

  /* Check for LVM and LUKS.  */
  ret = grub_util_get_dm_abstraction (os_dev);

  if (ret != GRUB_DEV_ABSTRACTION_NONE)
    return ret;

  /* Check for RAID.  */
  if (!strncmp (os_dev, "/dev/md", 7) && ! grub_util_device_is_mapped (os_dev))
    return GRUB_DEV_ABSTRACTION_RAID;
#endif

#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
  const char *abs;
  abs = grub_util_get_geom_abstraction (os_dev);
  grub_util_info ("abstraction of %s is %s", os_dev, abs);
  if (abs && grub_strcasecmp (abs, "eli") == 0)
    return GRUB_DEV_ABSTRACTION_GELI;

  /* Check for LVM.  */
  if (!strncmp (os_dev, LVM_DEV_MAPPER_STRING, sizeof(LVM_DEV_MAPPER_STRING)-1))
    return GRUB_DEV_ABSTRACTION_LVM;
#endif

  /* No abstraction found.  */
  return GRUB_DEV_ABSTRACTION_NONE;
}

#ifdef __linux__
static char *
get_mdadm_uuid (const char *os_dev)
{
  int mdadm_pipe[2];
  pid_t mdadm_pid;
  char *name = NULL;

  if (pipe (mdadm_pipe) < 0)
    {
      grub_util_warn (_("Unable to create pipe for mdadm: %s"),
		      strerror (errno));
      return NULL;
    }

  mdadm_pid = fork ();
  if (mdadm_pid < 0)
    grub_util_warn (_("Unable to fork mdadm: %s"), strerror (errno));
  else if (mdadm_pid == 0)
    {
      /* Child.  */
      char *argv[5];

      close (mdadm_pipe[0]);
      dup2 (mdadm_pipe[1], STDOUT_FILENO);
      close (mdadm_pipe[1]);

      /* execvp has inconvenient types, hence the casts.  None of these
         strings will actually be modified.  */
      argv[0] = (char *) "mdadm";
      argv[1] = (char *) "--detail";
      argv[2] = (char *) "--export";
      argv[3] = (char *) os_dev;
      argv[4] = NULL;
      execvp ("mdadm", argv);
      exit (127);
    }
  else
    {
      /* Parent.  Read mdadm's output.  */
      FILE *mdadm;
      char *buf = NULL;
      size_t len = 0;

      close (mdadm_pipe[1]);
      mdadm = fdopen (mdadm_pipe[0], "r");
      if (! mdadm)
	{
	  grub_util_warn (_("Unable to open stream from mdadm: %s"),
			  strerror (errno));
	  goto out;
	}

      while (getline (&buf, &len, mdadm) > 0)
	{
	  if (strncmp (buf, "MD_UUID=", sizeof ("MD_UUID=") - 1) == 0)
	    {
	      char *name_start, *ptri, *ptro;
	      size_t name_len;

	      free (name);
	      name_start = buf + sizeof ("MD_UUID=") - 1;
	      ptro = name = xmalloc (strlen (name_start) + 1);
	      for (ptri = name_start; *ptri && *ptri != '\n' && *ptri != '\r';
		   ptri++)
		if ((*ptri >= '0' && *ptri <= '9')
		    || (*ptri >= 'a' && *ptri <= 'f')
		    || (*ptri >= 'A' && *ptri <= 'F'))
		  *ptro++ = *ptri;
	      *ptro = 0;
	    }
	}

out:
      close (mdadm_pipe[0]);
      waitpid (mdadm_pid, NULL, 0);
    }

  return name;
}
#endif /* __linux__ */

void
grub_util_pull_device (const char *os_dev)
{
  int ab;
  ab = grub_util_get_dev_abstraction (os_dev);
  switch (ab)
    {
    case GRUB_DEV_ABSTRACTION_GELI:
#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
      {
	char *whole;
	struct gmesh mesh;
	struct gclass *class;
	const char *name;
	int error;
	char *lastsubdev = NULL;

	if (strncmp (os_dev, "/dev/", sizeof ("/dev/") - 1) != 0)
	  return;
	name = os_dev + sizeof ("/dev/") - 1;
	grub_util_follow_gpart_up (name, NULL, &whole);

	grub_util_info ("following geom '%s'", name);

	error = geom_gettree (&mesh);
	if (error != 0)
	  grub_util_error (_("couldn't open geom"));

	LIST_FOREACH (class, &mesh.lg_class, lg_class)
	  {
	    struct ggeom *geom;
	    LIST_FOREACH (geom, &class->lg_geom, lg_geom)
	      { 
		struct gprovider *provider;
		LIST_FOREACH (provider, &geom->lg_provider, lg_provider)
		  if (strcmp (provider->lg_name, name) == 0)
		    {
		      struct gconsumer *consumer;
		      char *fname;
		      char *uuid;

		      LIST_FOREACH (consumer, &geom->lg_consumer, lg_consumer)
			break;
		      if (!consumer)
			grub_util_error (_("couldn't find geli consumer"));
		      fname = xasprintf ("/dev/%s", consumer->lg_provider->lg_name);
		      grub_util_info ("consumer %s", consumer->lg_provider->lg_name);
		      lastsubdev = consumer->lg_provider->lg_name;
		      grub_util_pull_device (fname);
		      free (fname);
		    }
	      }
	  }
	if (ab == GRUB_DEV_ABSTRACTION_GELI && lastsubdev)
	  {
	    char *fname = xasprintf ("/dev/%s", lastsubdev);
	    char *grdev = grub_util_get_grub_dev (fname);
	    free (fname);

	    if (grdev)
	      {
		grub_err_t err;
		err = grub_cryptodisk_cheat_mount (grdev, os_dev);
		if (err)
		  grub_util_error (_("Can't mount crypto: %s"), _(grub_errmsg));
	      }

	    grub_free (grdev);
	  }
      }
#endif
      break;

    case GRUB_DEV_ABSTRACTION_LVM:
    case GRUB_DEV_ABSTRACTION_LUKS:
#ifdef HAVE_DEVICE_MAPPER
      {
	struct dm_tree *tree;
	struct dm_tree_node *node;
	struct dm_tree_node *child;
	void *handle = NULL;
	char *lastsubdev = NULL;

	if (!grub_util_open_dm (os_dev, &tree, &node))
	  return;

	while ((child = dm_tree_next_child (&handle, node, 0)))
	  {
	    const struct dm_info *dm = dm_tree_node_get_info (child);
	    char *subdev;
	    if (!dm)
	      continue;
	    subdev = grub_find_device ("/dev", makedev (dm->major, dm->minor));
	    if (subdev)
	      {
		lastsubdev = subdev;
		grub_util_pull_device (subdev);
	      }
	  }
	if (ab == GRUB_DEV_ABSTRACTION_LUKS && lastsubdev)
	  {
	    char *grdev = grub_util_get_grub_dev (lastsubdev);
	    dm_tree_free (tree);
	    if (grdev)
	      {
		grub_err_t err;
		err = grub_cryptodisk_cheat_mount (grdev, os_dev);
		if (err)
		  grub_util_error (_("Can't mount crypto: %s"), _(grub_errmsg));
	      }
	    grub_free (grdev);
	  }
	else
	  dm_tree_free (tree);
      }
#endif
      return;
    case GRUB_DEV_ABSTRACTION_RAID:
#ifdef __linux__
      {
	char **devicelist = grub_util_raid_getmembers (os_dev, 0);
	int i;
	for (i = 0; devicelist[i];i++)
	  grub_util_pull_device (devicelist[i]);
	free (devicelist);
      }
#endif
      return;

    default:  /* GRUB_DEV_ABSTRACTION_NONE */
      free (grub_util_biosdisk_get_grub_dev (os_dev));
      return;
    }
}

#ifdef HAVE_DEVICE_MAPPER
static int
grub_util_get_dm_node_linear_info (const char *dev,
				   int *maj, int *min)
{
  struct dm_task *dmt;
  void *next = NULL;
  uint64_t length, start;
  char *target, *params;
  char *ptr;
  int major, minor;

  dmt = dm_task_create(DM_DEVICE_TABLE);
  if (!dmt)
    return 0;
  
  if (!dm_task_set_name(dmt, dev))
    {
      dm_task_destroy (dmt);
      return 0;
    }
  dm_task_no_open_count(dmt);
  if (!dm_task_run(dmt))
    {
      dm_task_destroy (dmt);
      return 0;
    }
  next = dm_get_next_target(dmt, next, &start, &length,
			    &target, &params);
  if (grub_strcmp (target, "linear") != 0)
    {
      dm_task_destroy (dmt);
      return 0;
    }
  major = grub_strtoul (params, &ptr, 10);
  if (grub_errno)
    {
      dm_task_destroy (dmt);
      grub_errno = GRUB_ERR_NONE;
      return 0;
    }
  if (*ptr != ':')
    {
      dm_task_destroy (dmt);
      return 0;
    }
  ptr++;
  minor = grub_strtoul (ptr, 0, 10);
  if (grub_errno)
    {
      grub_errno = GRUB_ERR_NONE;
      dm_task_destroy (dmt);
      return 0;
    }
  if (maj)
    *maj = major;
  if (min)
    *min = minor;
  dm_task_destroy (dmt);
  return 1;
}
#endif

int
grub_util_biosdisk_is_floppy (grub_disk_t disk)
{
  struct stat st;
  int fd;
  const char *dname;

  dname = grub_util_biosdisk_get_osdev (disk);

  if (!dname)
    return 0;

  fd = open (dname, O_RDONLY);
  /* Shouldn't happen.  */
  if (fd == -1)
    return 0;

  /* Shouldn't happen either.  */
  if (fstat (fd, &st) < 0)
    {
      close (fd);
      return 0;
    }

  close (fd);

#if defined(__NetBSD__)
  if (major(st.st_rdev) == RAW_FLOPPY_MAJOR)
    return 1;
#endif

#if defined(FLOPPY_MAJOR)
  if (major(st.st_rdev) == FLOPPY_MAJOR)
#else
  /* Some kernels (e.g. kFreeBSD) don't have a static major number
     for floppies, but they still use a "fd[0-9]" pathname.  */
  if (map[disk->id].device[5] == 'f'
      && map[disk->id].device[6] == 'd'
      && map[disk->id].device[7] >= '0'
      && map[disk->id].device[7] <= '9')
#endif
    return 1;

  return 0;
}

static char *
convert_system_partition_to_system_disk (const char *os_dev, struct stat *st)
{
#if defined(__linux__)
  char *path = xmalloc (PATH_MAX);
  if (! realpath (os_dev, path))
    return NULL;

  if (strncmp ("/dev/", path, 5) == 0)
    {
      char *p = path + 5;

      /* If this is an IDE disk.  */
      if (strncmp ("ide/", p, 4) == 0)
	{
	  p = strstr (p, "part");
	  if (p)
	    strcpy (p, "disc");

	  return path;
	}

      /* If this is a SCSI disk.  */
      if (strncmp ("scsi/", p, 5) == 0)
	{
	  p = strstr (p, "part");
	  if (p)
	    strcpy (p, "disc");

	  return path;
	}

      /* If this is a DAC960 disk.  */
      if (strncmp ("rd/c", p, 4) == 0)
	{
	  /* /dev/rd/c[0-9]+d[0-9]+(p[0-9]+)? */
	  p = strchr (p, 'p');
	  if (p)
	    *p = '\0';

	  return path;
	}

      /* If this is a Mylex AcceleRAID Array.  */
      if (strncmp ("rs/c", p, 4) == 0)
	{
	  /* /dev/rd/c[0-9]+d[0-9]+(p[0-9]+)? */
	  p = strchr (p, 'p');
	  if (p)
	    *p = '\0';

	  return path;
	}
      /* If this is a CCISS disk.  */
      if (strncmp ("cciss/c", p, sizeof ("cciss/c") - 1) == 0)
	{
	  /* /dev/cciss/c[0-9]+d[0-9]+(p[0-9]+)? */
	  p = strchr (p, 'p');
	  if (p)
	    *p = '\0';

	  return path;
	}

      /* If this is a Compaq Intelligent Drive Array.  */
      if (strncmp ("ida/c", p, sizeof ("ida/c") - 1) == 0)
	{
	  /* /dev/ida/c[0-9]+d[0-9]+(p[0-9]+)? */
	  p = strchr (p, 'p');
	  if (p)
	    *p = '\0';

	  return path;
	}

      /* If this is an I2O disk.  */
      if (strncmp ("i2o/hd", p, sizeof ("i2o/hd") - 1) == 0)
      	{
	  /* /dev/i2o/hd[a-z]([0-9]+)? */
	  p[sizeof ("i2o/hda") - 1] = '\0';
	  return path;
	}

      /* If this is a MultiMediaCard (MMC).  */
      if (strncmp ("mmcblk", p, sizeof ("mmcblk") - 1) == 0)
	{
	  /* /dev/mmcblk[0-9]+(p[0-9]+)? */
	  p = strchr (p, 'p');
	  if (p)
	    *p = '\0';

	  return path;
	}

      if (strncmp ("md", p, 2) == 0
	  && p[2] >= '0' && p[2] <= '9')
	{
	  char *ptr = p + 2;
	  while (*ptr >= '0' && *ptr <= '9')
	    ptr++;
	  *ptr = 0;
	  return path;
	}

      /* If this is an IDE, SCSI or Virtio disk.  */
      if (strncmp ("vdisk", p, 5) == 0
	  && p[5] >= 'a' && p[5] <= 'z')
	{
	  /* /dev/vdisk[a-z][0-9]* */
	  p[6] = '\0';
	  return path;
	}
      if ((strncmp ("hd", p, 2) == 0
	   || strncmp ("vd", p, 2) == 0
	   || strncmp ("sd", p, 2) == 0)
	  && p[2] >= 'a' && p[2] <= 'z')
	{
	  char *pp = p + 2;
	  while (*pp >= 'a' && *pp <= 'z')
	    pp++;
	  /* /dev/[hsv]d[a-z]+[0-9]* */
	  *pp = '\0';
	  return path;
	}

      /* If this is a Xen virtual block device.  */
      if ((strncmp ("xvd", p, 3) == 0) && p[3] >= 'a' && p[3] <= 'z')
	{
	  char *pp = p + 3;
	  while (*pp >= 'a' && *pp <= 'z')
	    pp++;
	  /* /dev/xvd[a-z]+[0-9]* */
	  *pp = '\0';
	  return path;
	}

#ifdef HAVE_DEVICE_MAPPER
      /* If this is a DM-RAID device.
         Compare os_dev rather than path here, since nodes under
         /dev/mapper/ are often symlinks.  */
      if ((strncmp ("/dev/mapper/", os_dev, 12) == 0))
	{
	  struct dm_tree *tree;
	  uint32_t maj, min;
	  struct dm_tree_node *node = NULL, *child;
	  void *handle;
	  const char *node_uuid, *mapper_name = NULL, *child_uuid, *child_name;

	  tree = dm_tree_create ();
	  if (! tree)
	    {
	      grub_dprintf ("hostdisk", "dm_tree_create failed\n");
	      goto devmapper_out;
	    }

	  maj = major (st->st_rdev);
	  min = minor (st->st_rdev);
	  if (! dm_tree_add_dev (tree, maj, min))
	    {
	      grub_dprintf ("hostdisk", "dm_tree_add_dev failed\n");
	      goto devmapper_out;
	    }

	  node = dm_tree_find_node (tree, maj, min);
	  if (! node)
	    {
	      grub_dprintf ("hostdisk", "dm_tree_find_node failed\n");
	      goto devmapper_out;
	    }
	  node_uuid = dm_tree_node_get_uuid (node);
	  if (! node_uuid)
	    {
	      grub_dprintf ("hostdisk", "%s has no DM uuid\n", path);
	      node = NULL;
	      goto devmapper_out;
	    }
	  if (strncmp (node_uuid, "LVM-", 4) == 0)
	    {
	      grub_dprintf ("hostdisk", "%s is an LVM\n", path);
	      node = NULL;
	      goto devmapper_out;
	    }
	  if (strncmp (node_uuid, "mpath-", 6) == 0)
	    {
	      /* Multipath partitions have partN-mpath-* UUIDs, and are
		 linear mappings so are handled by
		 grub_util_get_dm_node_linear_info.  Multipath disks are not
		 linear mappings and must be handled specially.  */
	      grub_dprintf ("hostdisk", "%s is a multipath disk\n", path);
	      mapper_name = dm_tree_node_get_name (node);
	      goto devmapper_out;
	    }
	  if (strncmp (node_uuid, "DMRAID-", 7) != 0)
	    {
	      int major, minor;
	      const char *node_name;
	      grub_dprintf ("hostdisk", "%s is not DM-RAID\n", path);

	      if ((node_name = dm_tree_node_get_name (node))
		  && grub_util_get_dm_node_linear_info (node_name,
							&major, &minor))
		{
		  if (tree)
		    dm_tree_free (tree);
		  free (path);
		  char *ret = grub_find_device ("/dev",
						(major << 8) | minor);
		  return ret;
		}

	      node = NULL;
	      goto devmapper_out;
	    }

	  handle = NULL;
	  /* Counter-intuitively, device-mapper refers to the disk-like
	     device containing a DM-RAID partition device as a "child" of
	     the partition device.  */
	  child = dm_tree_next_child (&handle, node, 0);
	  if (! child)
	    {
	      grub_dprintf ("hostdisk", "%s has no DM children\n", path);
	      goto devmapper_out;
	    }
	  child_uuid = dm_tree_node_get_uuid (child);
	  if (! child_uuid)
	    {
	      grub_dprintf ("hostdisk", "%s child has no DM uuid\n", path);
	      goto devmapper_out;
	    }
	  else if (strncmp (child_uuid, "DMRAID-", 7) != 0)
	    {
	      grub_dprintf ("hostdisk", "%s child is not DM-RAID\n", path);
	      goto devmapper_out;
	    }
	  child_name = dm_tree_node_get_name (child);
	  if (! child_name)
	    {
	      grub_dprintf ("hostdisk", "%s child has no DM name\n", path);
	      goto devmapper_out;
	    }
	  mapper_name = child_name;

devmapper_out:
	  if (! mapper_name && node)
	    {
	      /* This is a DM-RAID disk, not a partition.  */
	      mapper_name = dm_tree_node_get_name (node);
	      if (! mapper_name)
		grub_dprintf ("hostdisk", "%s has no DM name\n", path);
	    }
	  if (tree)
	    dm_tree_free (tree);
	  free (path);
	  if (mapper_name)
	    return xasprintf ("/dev/mapper/%s", mapper_name);
	  else
	    return NULL;
	}
#endif /* HAVE_DEVICE_MAPPER */
    }

  return path;

#elif defined(__GNU__)
  char *path = xstrdup (os_dev);
  if (strncmp ("/dev/sd", path, 7) == 0 || strncmp ("/dev/hd", path, 7) == 0)
    {
      char *p = strchr (path + 7, 's');
      if (p)
	*p = '\0';
    }
  return path;

#elif defined(__CYGWIN__)
  char *path = xstrdup (os_dev);
  if (strncmp ("/dev/sd", path, 7) == 0 && 'a' <= path[7] && path[7] <= 'z')
    path[8] = 0;
  return path;

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
  char *out, *out2;
  if (strncmp (os_dev, "/dev/", sizeof ("/dev/") - 1) != 0)
    return xstrdup (os_dev);
  grub_util_follow_gpart_up (os_dev + sizeof ("/dev/") - 1, NULL, &out);

  out2 = xasprintf ("/dev/%s", out);
  free (out);

  return out2;
#elif defined(__APPLE__)
  char *path = xstrdup (os_dev);
  if (strncmp ("/dev/", path, 5) == 0)
    {
      char *p;
      for (p = path + 5; *p; ++p)
        if (grub_isdigit(*p))
          {
            p = strpbrk (p, "sp");
            if (p)
              *p = '\0';
            break;
          }
    }
  return path;

#elif defined(__NetBSD__)
  int rawpart = -1;
# ifdef HAVE_GETRAWPARTITION
  rawpart = getrawpartition();
# endif /* HAVE_GETRAWPARTITION */
  if (rawpart < 0)
    return xstrdup (os_dev);

  /* NetBSD disk wedges are of the form "/dev/rdk.*".  */
  if (strncmp ("/dev/rdk", os_dev, sizeof("/dev/rdk") - 1) == 0)
    {
      struct dkwedge_info dkw;
      int fd;

      fd = open (os_dev, O_RDONLY);
      if (fd == -1)
	{
	  grub_error (GRUB_ERR_BAD_DEVICE,
		      "cannot open `%s' while attempting to get disk wedge info", os_dev);
	  return xstrdup (os_dev);
	}
      /* We don't call configure_device_driver since this isn't a floppy device name.  */
      if (ioctl (fd, DIOCGWEDGEINFO, &dkw) == -1)
	{
	  grub_error (GRUB_ERR_BAD_DEVICE,
		      "cannot get disk wedge info of `%s'", os_dev);
	  close (fd);
	  return xstrdup (os_dev);
	}
      close (fd);
      return xasprintf ("/dev/r%s%c", dkw.dkw_parent, 'a' + rawpart);
    }

  /* NetBSD (disk label) partitions are of the form "/dev/r[a-z]+[0-9][a-z]".  */
  if (strncmp ("/dev/r", os_dev, sizeof("/dev/r") - 1) == 0 &&
      (os_dev[sizeof("/dev/r") - 1] >= 'a' && os_dev[sizeof("/dev/r") - 1] <= 'z') &&
      strncmp ("fd", os_dev + sizeof("/dev/r") - 1, sizeof("fd") - 1) != 0)    /* not a floppy device name */
    {
      char *path = xstrdup (os_dev);
      char *p;
      for (p = path + sizeof("/dev/r"); *p >= 'a' && *p <= 'z'; p++);
      if (grub_isdigit(*p))
	{
	  p++;
	  if ((*p >= 'a' && *p <= 'z') && (*(p+1) == '\0'))
	    {
	      /* path matches the required regular expression and
		 p points to its last character.  */
	      *p = 'a' + rawpart;
	    }
	}
      return path;
    }

  return xstrdup (os_dev);

#elif defined (__sun__)
  char *colon = grub_strrchr (os_dev, ':');
  if (grub_memcmp (os_dev, "/devices", sizeof ("/devices") - 1) == 0
      && colon)
    {
      char *ret = xmalloc (colon - os_dev + sizeof (":q,raw"));
      grub_memcpy (ret, os_dev, colon - os_dev);
      grub_memcpy (ret + (colon - os_dev), ":q,raw", sizeof (":q,raw"));
      return ret;
    }
  else
    return xstrdup (os_dev);
#else
# warning "The function `convert_system_partition_to_system_disk' might not work on your OS correctly."
  return xstrdup (os_dev);
#endif
}

static const char *
find_system_device (const char *os_dev, struct stat *st, int convert, int add)
{
  unsigned int i;
  char *os_disk;
  const char *drive;

  if (convert)
    os_disk = convert_system_partition_to_system_disk (os_dev, st);
  else
    os_disk = xstrdup (os_dev);
  if (! os_disk)
    return NULL;

  drive = grub_hostdisk_os_dev_to_grub_drive (os_disk, add);
  free (os_disk);
  return drive;
}

/*
 * Note: we do not use the new partition naming scheme as dos_part does not
 * necessarily correspond to an msdos partition.
 */
static char *
make_device_name (const char *drive, int dos_part, int bsd_part)
{
  char *ret, *ptr, *end;
  const char *iptr;

  ret = xmalloc (strlen (drive) * 2 
		 + sizeof (",XXXXXXXXXXXXXXXXXXXXXXXXXX"
			   ",XXXXXXXXXXXXXXXXXXXXXXXXXX"));
  end = (ret + strlen (drive) * 2 
	 + sizeof (",XXXXXXXXXXXXXXXXXXXXXXXXXX"
		   ",XXXXXXXXXXXXXXXXXXXXXXXXXX"));
  ptr = ret;
  for (iptr = drive; *iptr; iptr++)
    {
      if (*iptr == ',')
	*ptr++ = '\\';
      *ptr++ = *iptr;
    }
  *ptr = 0;
  if (dos_part >= 0)
    snprintf (ptr, end - ptr, ",%d", dos_part + 1);
  ptr += strlen (ptr);
  if (bsd_part >= 0)
    snprintf (ptr, end - ptr, ",%d", bsd_part + 1); 

  return ret;
}

#if defined(__sun__)
static int
device_is_wholedisk (const char *os_dev)
{
  if (grub_memcmp (os_dev, "/devices/", sizeof ("/devices/") - 1) != 0)
    return 1;
  if (grub_memcmp (os_dev + strlen (os_dev) - (sizeof (":q,raw") - 1),
		   ":q,raw", (sizeof (":q,raw") - 1)) == 0)
    return 1;
  return 0;
}
#endif

#if defined(__linux__) || defined(__CYGWIN__)
static int
device_is_wholedisk (const char *os_dev)
{
  int len = strlen (os_dev);

  if (os_dev[len - 1] < '0' || os_dev[len - 1] > '9')
    return 1;
  return 0;
}
#endif

#if defined(__NetBSD__)
/* Try to determine whether a given device name corresponds to a whole disk.
   This function should give in most cases a definite answer, but it may
   actually give an approximate one in the following sense: if the return
   value is 0 then the device name does not correspond to a whole disk.  */
static int
device_is_wholedisk (const char *os_dev)
{
  int len = strlen (os_dev);
  int rawpart = -1;

# ifdef HAVE_GETRAWPARTITION
  rawpart = getrawpartition();
# endif /* HAVE_GETRAWPARTITION */
  if (rawpart < 0)
    return 1;
  return (os_dev[len - 1] == ('a' + rawpart));
}
#endif /* defined(__NetBSD__) */

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
static int
device_is_wholedisk (const char *os_dev)
{
  const char *p;

  if (strncmp (os_dev, "/dev/", sizeof ("/dev/") - 1) != 0)
    return 0;

  for (p = os_dev + sizeof ("/dev/") - 1; *p; ++p)
    if (grub_isdigit (*p))
      {
	if (strchr (p, 's'))
	  return 0;
	break;
      }

  return 1;
}
#endif /* defined(__FreeBSD__) || defined(__FreeBSD_kernel__) */

char *
grub_util_biosdisk_get_grub_dev (const char *os_dev)
{
  struct stat st;
  const char *drive;
  char *sys_disk;

  grub_util_info ("Looking for %s", os_dev);

  if (stat (os_dev, &st) < 0)
    {
      grub_error (GRUB_ERR_BAD_DEVICE, "cannot stat `%s'", os_dev);
      grub_util_info ("cannot stat `%s'", os_dev);
      return 0;
    }

  drive = find_system_device (os_dev, &st, 1, 1);
  if (!drive)
    {
      grub_error (GRUB_ERR_UNKNOWN_DEVICE,
		  "no mapping exists for `%s'", os_dev);
      grub_util_info ("no mapping exists for `%s'", os_dev);
      return 0;
    }

  sys_disk = convert_system_partition_to_system_disk (os_dev, &st);
  if (grub_strcmp (os_dev, sys_disk) == 0)
    {
      free (sys_disk);
      return make_device_name (drive, -1, -1);
    }
  free (sys_disk);

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__APPLE__) || defined(__NetBSD__) || defined (__sun__)
  if (! S_ISCHR (st.st_mode))
#else
  if (! S_ISBLK (st.st_mode))
#endif
    return make_device_name (drive, -1, -1);

#if defined(__linux__) || defined(__CYGWIN__) || defined(HAVE_DIOCGDINFO) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined (__sun__)

  /* Linux counts partitions uniformly, whether a BSD partition or a DOS
     partition, so mapping them to GRUB devices is not trivial.
     Here, get the start sector of a partition by HDIO_GETGEO, and
     compare it with each partition GRUB recognizes.

     Cygwin /dev/sdXN emulation uses Windows partition mapping. It
     does not count the extended partition and missing primary
     partitions.  Use same method as on Linux here.

     For NetBSD and FreeBSD, proceed as for Linux, except that the start
     sector is obtained from the disk label.  */
  {
    char *name, *partname;
    grub_disk_t disk;
    grub_disk_addr_t start;
    auto int find_partition (grub_disk_t dsk,
			     const grub_partition_t partition);

    int find_partition (grub_disk_t dsk __attribute__ ((unused)),
			const grub_partition_t partition)
      {
	grub_disk_addr_t part_start = 0;
	grub_util_info ("Partition %d starts from %lu",
			partition->number, partition->start);

	part_start = grub_partition_get_start (partition);

	if (start == part_start)
	  {
	    partname = grub_partition_get_name (partition);
	    return 1;
	  }

	return 0;
      }

    name = make_device_name (drive, -1, -1);

# if !defined(HAVE_DIOCGDINFO) && !defined(__sun__)
    if (MAJOR (st.st_rdev) == FLOPPY_MAJOR)
      return name;
# else /* defined(HAVE_DIOCGDINFO) */
    /* Since os_dev and convert_system_partition_to_system_disk (os_dev) are
     * different, we know that os_dev cannot be a floppy device.  */
# endif /* !defined(HAVE_DIOCGDINFO) */

    start = grub_hostdisk_find_partition_start (os_dev);
    if (grub_errno != GRUB_ERR_NONE)
      {
	free (name);
	return 0;
      }

    grub_util_info ("%s starts from %lu", os_dev, start);

    if (start == 0 && device_is_wholedisk (os_dev))
      return name;

    grub_util_info ("opening the device %s", name);
    disk = grub_disk_open (name);
    free (name);

    if (! disk)
      {
	/* We already know that the partition exists.  Given that we already
	   checked the device map above, we can only get
	   GRUB_ERR_UNKNOWN_DEVICE at this point if the disk does not exist.
	   This can happen on Xen, where disk images in the host can be
	   assigned to devices that have partition-like names in the guest
	   but are really more like disks.  */
	if (grub_errno == GRUB_ERR_UNKNOWN_DEVICE)
	  {
	    grub_util_warn
	      (_("disk does not exist, so falling back to partition device %s"),
	       os_dev);

	    drive = find_system_device (os_dev, &st, 0, 1);
	    if (!drive)
	      {
		grub_error (GRUB_ERR_UNKNOWN_DEVICE,
			    "no mapping exists for `%s'", os_dev);
		return 0;
	      }

	    return make_device_name (drive, -1, -1);
	  }
	else
	  return 0;
      }

    name = grub_util_get_ldm (disk, start);
    if (name)
      return name;

    partname = NULL;

    grub_partition_iterate (disk, find_partition);
    if (grub_errno != GRUB_ERR_NONE)
      {
	grub_disk_close (disk);
	return 0;
      }

    if (partname == NULL)
      {
	grub_disk_close (disk);
	grub_util_info ("cannot find the partition of `%s'", os_dev);
	grub_error (GRUB_ERR_BAD_DEVICE,
		    "cannot find the partition of `%s'", os_dev);
	return 0;
      }

    name = grub_xasprintf ("%s,%s", disk->name, partname);
    free (partname);
    grub_disk_close (disk);
    return name;
  }

#elif defined(__GNU__)
  /* GNU uses "/dev/[hs]d[0-9]+(s[0-9]+[a-z]?)?".  */
  {
    char *p;
    int dos_part = -1;
    int bsd_part = -1;

    p = strrchr (os_dev, 's');
    if (p)
      {
	long int n;
	char *q;

	p++;
	n = strtol (p, &q, 10);
	if (p != q && n != GRUB_LONG_MIN && n != GRUB_LONG_MAX)
	  {
	    dos_part = (int) n - 1;

	    if (*q >= 'a' && *q <= 'g')
	      bsd_part = *q - 'a';
	  }
      }

    return make_device_name (drive, dos_part, bsd_part);
  }

#else
# warning "The function `grub_util_biosdisk_get_grub_dev' might not work on your OS correctly."
  return make_device_name (drive, -1, -1);
#endif
}

int
grub_util_biosdisk_is_present (const char *os_dev)
{
  struct stat st;

  if (stat (os_dev, &st) < 0)
    return 0;

  return find_system_device (os_dev, &st, 1, 0) != NULL;
}

char *
grub_util_get_grub_dev (const char *os_dev)
{
  char *grub_dev = NULL;

  grub_util_pull_device (os_dev);

  switch (grub_util_get_dev_abstraction (os_dev))
    {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    case GRUB_DEV_ABSTRACTION_LVM:

      {
	unsigned short i, len;
	grub_size_t offset = sizeof (LVM_DEV_MAPPER_STRING) - 1;

	len = strlen (os_dev) - offset + 1;
	grub_dev = xmalloc (len + sizeof ("lvm/"));

	grub_memcpy (grub_dev, "lvm/", sizeof ("lvm/") - 1);
	grub_memcpy (grub_dev + sizeof ("lvm/") - 1, os_dev + offset, len);
      }

      break;
#endif

#ifdef __linux__
    case GRUB_DEV_ABSTRACTION_LUKS:
      {
	char *uuid, *dash;
	uuid = get_dm_uuid (os_dev);
	if (!uuid)
	  break;
	dash = grub_strchr (uuid + sizeof ("CRYPT-LUKS1-") - 1, '-');
	if (dash)
	  *dash = 0;
	grub_dev = grub_xasprintf ("cryptouuid/%s",
				   uuid + sizeof ("CRYPT-LUKS1-") - 1);
	grub_free (uuid);
      }
      break;
#endif

#if defined (__FreeBSD__) || defined(__FreeBSD_kernel__)
    case GRUB_DEV_ABSTRACTION_GELI:
      {
	char *whole;
	struct gmesh mesh;
	struct gclass *class;
	const char *name;
	int error;

	if (strncmp (os_dev, "/dev/", sizeof ("/dev/") - 1) != 0)
	  return 0;
	name = os_dev + sizeof ("/dev/") - 1;
	grub_util_follow_gpart_up (name, NULL, &whole);

	grub_util_info ("following geom '%s'", name);

	error = geom_gettree (&mesh);
	if (error != 0)
	  grub_util_error (_("couldn't open geom"));

	LIST_FOREACH (class, &mesh.lg_class, lg_class)
	  {
	    struct ggeom *geom;
	    LIST_FOREACH (geom, &class->lg_geom, lg_geom)
	      { 
		struct gprovider *provider;
		LIST_FOREACH (provider, &geom->lg_provider, lg_provider)
		  if (strcmp (provider->lg_name, name) == 0)
		    {
		      struct gconsumer *consumer;
		      char *fname;
		      char *uuid;

		      LIST_FOREACH (consumer, &geom->lg_consumer, lg_consumer)
			break;
		      if (!consumer)
			grub_util_error (_("couldn't find geli consumer"));
		      fname = xasprintf ("/dev/%s", consumer->lg_provider->lg_name);
		      uuid = grub_util_get_geli_uuid (fname);
		      if (!uuid)
			grub_util_error (_("couldn't retrieve geli UUID"));
		      grub_dev = xasprintf ("cryptouuid/%s", uuid);
		      free (fname);
		      free (uuid);
		    }
	      }
	  }
      }
      break;
#endif

#ifdef __linux__
    case GRUB_DEV_ABSTRACTION_RAID:

      if (os_dev[7] == '_' && os_dev[8] == 'd')
	{
	  /* This a partitionable RAID device of the form /dev/md_dNNpMM. */

	  char *p, *q;

	  p = strdup (os_dev + sizeof ("/dev/md_d") - 1);

	  q = strchr (p, 'p');
	  if (q)
	    *q = ',';

	  grub_dev = xasprintf ("md%s", p);
	  free (p);
	}
      else if (os_dev[7] == '/' && os_dev[8] == 'd')
	{
	  /* This a partitionable RAID device of the form /dev/md/dNNpMM. */

	  char *p, *q;

	  p = strdup (os_dev + sizeof ("/dev/md/d") - 1);

	  q = strchr (p, 'p');
	  if (q)
	    *q = ',';

	  grub_dev = xasprintf ("md%s", p);
	  free (p);
	}
      else if (os_dev[7] >= '0' && os_dev[7] <= '9')
	{
	  char *p , *q;

	  p = strdup (os_dev + sizeof ("/dev/md") - 1);

	  q = strchr (p, 'p');
	  if (q)
	    *q = ',';

	  grub_dev = xasprintf ("md%s", p);
	  free (p);
	}
      else if (os_dev[7] == '/' && os_dev[8] >= '0' && os_dev[8] <= '9')
	{
	  char *p , *q;

	  p = strdup (os_dev + sizeof ("/dev/md/") - 1);

	  q = strchr (p, 'p');
	  if (q)
	    *q = ',';

	  grub_dev = xasprintf ("md%s", p);
	  free (p);
	}
      else if (os_dev[7] == '/')
	{
	  /* mdraid 1.x with a free name.  */
	  char *p , *q;

	  p = strdup (os_dev + sizeof ("/dev/md/") - 1);

	  q = strchr (p, 'p');
	  if (q)
	    *q = ',';

	  grub_dev = xasprintf ("md/%s", p);
	  free (p);
	}
      else
	grub_util_error (_("unknown kind of RAID device `%s'"), os_dev);

      {
	char *mdadm_name = get_mdadm_uuid (os_dev);
	struct stat st;

	if (mdadm_name)
	  {
	    const char *q;

	    for (q = os_dev + strlen (os_dev) - 1; q >= os_dev
		   && grub_isdigit (*q); q--);

	    if (q >= os_dev && *q == 'p')
	      {
		free (grub_dev);
		grub_dev = xasprintf ("mduuid/%s,%s", mdadm_name, q + 1);
		goto done;
	      }
	    free (grub_dev);
	    grub_dev = xasprintf ("mduuid/%s", mdadm_name);

	  done:
	    free (mdadm_name);
	  }
      }
      break;
#endif /* __linux__ */

    default:  /* GRUB_DEV_ABSTRACTION_NONE */
      grub_dev = grub_util_biosdisk_get_grub_dev (os_dev);
    }

  return grub_dev;
}

const char *
grub_util_check_block_device (const char *blk_dev)
{
  struct stat st;

  if (stat (blk_dev, &st) < 0)
    grub_util_error (_("cannot stat `%s'"), blk_dev);

  if (S_ISBLK (st.st_mode))
    return (blk_dev);
  else
    return 0;
}

const char *
grub_util_check_char_device (const char *blk_dev)
{
  struct stat st;

  if (stat (blk_dev, &st) < 0)
    grub_util_error (_("cannot stat `%s'"), blk_dev);

  if (S_ISCHR (st.st_mode))
    return (blk_dev);
  else
    return 0;
}

#ifdef __CYGWIN__
/* Convert POSIX path to Win32 path,
   remove drive letter, replace backslashes.  */
static char *
get_win32_path (const char *path)
{
  char winpath[PATH_MAX];
  if (cygwin_conv_path (CCP_POSIX_TO_WIN_A, path, winpath, sizeof(winpath)))
    grub_util_error (_("cygwin_conv_path() failed"));

  int len = strlen (winpath);
  int offs = (len > 2 && winpath[1] == ':' ? 2 : 0);

  int i;
  for (i = offs; i < len; i++)
    if (winpath[i] == '\\')
      winpath[i] = '/';
  return xstrdup (winpath + offs);
}
#endif

#ifdef HAVE_LIBZFS
static libzfs_handle_t *__libzfs_handle;

static void
fini_libzfs (void)
{
  libzfs_fini (__libzfs_handle);
}

libzfs_handle_t *
grub_get_libzfs_handle (void)
{
  if (! __libzfs_handle)
    {
      __libzfs_handle = libzfs_init ();

      if (__libzfs_handle)
	atexit (fini_libzfs);
    }

  return __libzfs_handle;
}
#endif /* HAVE_LIBZFS */

/* ZFS has similar problems to those of btrfs (see above).  */
void
grub_find_zpool_from_dir (const char *dir, char **poolname, char **poolfs)
{
  char *slash;

  *poolname = *poolfs = NULL;

#if defined(HAVE_STRUCT_STATFS_F_FSTYPENAME) && defined(HAVE_STRUCT_STATFS_F_MNTFROMNAME)
  /* FreeBSD and GNU/kFreeBSD.  */
  {
    struct statfs mnt;

    if (statfs (dir, &mnt) != 0)
      return;

    if (strcmp (mnt.f_fstypename, "zfs") != 0)
      return;

    *poolname = xstrdup (mnt.f_mntfromname);
  }
#elif defined(HAVE_GETEXTMNTENT)
  /* Solaris.  */
  {
    struct stat st;
    struct extmnttab mnt;

    if (stat (dir, &st) != 0)
      return;

    FILE *mnttab = fopen ("/etc/mnttab", "r");
    if (! mnttab)
      return;

    while (getextmntent (mnttab, &mnt, sizeof (mnt)) == 0)
      {
	if (makedev (mnt.mnt_major, mnt.mnt_minor) == st.st_dev
	    && !strcmp (mnt.mnt_fstype, "zfs"))
	  {
	    *poolname = xstrdup (mnt.mnt_special);
	    break;
	  }
      }

    fclose (mnttab);
  }
#endif

  if (! *poolname)
    return;

  slash = strchr (*poolname, '/');
  if (slash)
    {
      *slash = '\0';
      *poolfs = xstrdup (slash + 1);
    }
  else
    *poolfs = xstrdup ("");
}

/* This function never prints trailing slashes (so that its output
   can be appended a slash unconditionally).  */
char *
grub_make_system_path_relative_to_its_root (const char *path)
{
  struct stat st;
  char *p, *buf, *buf2, *buf3, *ret;
  uintptr_t offset = 0;
  dev_t num;
  size_t len;
  char *poolfs = NULL;

  /* canonicalize.  */
  p = canonicalize_file_name (path);
  if (p == NULL)
    grub_util_error (_("failed to get canonical path of %s"), path);

  /* For ZFS sub-pool filesystems, could be extended to others (btrfs?).  */
  {
    char *dummy;
    grub_find_zpool_from_dir (p, &dummy, &poolfs);
  }

  len = strlen (p) + 1;
  buf = xstrdup (p);
  free (p);

  if (stat (buf, &st) < 0)
    grub_util_error (_("cannot stat %s: %s"), buf, strerror (errno));

  buf2 = xstrdup (buf);
  num = st.st_dev;

  /* This loop sets offset to the number of chars of the root
     directory we're inspecting.  */
  while (1)
    {
      p = strrchr (buf, '/');
      if (p == NULL)
	/* This should never happen.  */
	grub_util_error (_("FIXME: no / in buf. (make_system_path_relative_to_its_root)"));
      if (p != buf)
	*p = 0;
      else
	*++p = 0;

      if (stat (buf, &st) < 0)
	grub_util_error (_("cannot stat %s: %s"), buf, strerror (errno));

      /* buf is another filesystem; we found it.  */
      if (st.st_dev != num)
	{
	  /* offset == 0 means path given is the mount point.
	     This works around special-casing of "/" in Un*x.  This function never
	     prints trailing slashes (so that its output can be appended a slash
	     unconditionally).  Each slash in is considered a preceding slash, and
	     therefore the root directory is an empty string.  */
	  if (offset == 0)
	    {
	      free (buf);
#ifdef __linux__
	      {
		char *bind;
		grub_free (grub_find_root_devices_from_mountinfo (buf2, &bind));
		if (bind && bind[0] && bind[1])
		  {
		    buf3 = bind;
		    goto parsedir;
		  }
		grub_free (bind);
	      }
#endif
	      free (buf2);
	      if (poolfs)
		return xasprintf ("/%s/@", poolfs);
	      return xstrdup ("");
	    }
	  else
	    break;
	}

      offset = p - buf;
      /* offset == 1 means root directory.  */
      if (offset == 1)
	{
	  /* Include leading slash.  */
	  offset = 0;
	  break;
	}
    }
  free (buf);
  buf3 = xstrdup (buf2 + offset);
  buf2[offset] = 0;
#ifdef __linux__
  {
    char *bind;
    grub_free (grub_find_root_devices_from_mountinfo (buf2, &bind));
    if (bind && bind[0] && bind[1])
      {
	char *temp = buf3;
	buf3 = grub_xasprintf ("%s%s%s", bind, buf3[0] == '/' ?"":"/", buf3);
	grub_free (temp);
      }
    grub_free (bind);
  }
#endif
  
  free (buf2);

#ifdef __CYGWIN__
  if (st.st_dev != (DEV_CYGDRIVE_MAJOR << 16))
    {
      /* Reached some mount point not below /cygdrive.
	 GRUB does not know Cygwin's emulated mounts,
	 convert to Win32 path.  */
      grub_util_info ("Cygwin path = %s\n", buf3);
      char * temp = get_win32_path (buf3);
      free (buf3);
      buf3 = temp;
    }
#endif

 parsedir:
  /* Remove trailing slashes, return empty string if root directory.  */
  len = strlen (buf3);
  while (len > 0 && buf3[len - 1] == '/')
    {
      buf3[len - 1] = '\0';
      len--;
    }

  if (poolfs)
    {
      ret = xasprintf ("/%s/@%s", poolfs, buf3);
      free (buf3);
    }
  else
    ret = buf3;

  return ret;
}
