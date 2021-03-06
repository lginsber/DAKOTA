/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright (c) 2010, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Description:  Filesystem_utils (to be replaced by Boost, eventually)
//-
//- Version: $Id$

#include "dakota_filesystem_utils.hpp"
#include "dakota_global_defs.hpp"
#include "WorkdirHelper.hpp" // for WorkdirHelper::dakPreferredEnvPath
#include <boost/array.hpp>

#include <sys/types.h>
#include <sys/stat.h>

#undef Want_Heartbeat

// WJB - ToDo, next iteration: evaluate dependencies; are posix includes needed??
#ifndef _WIN32 /*{{*/
#ifndef NO_HEARTBEAT /*{*/
#define Want_Heartbeat
#include <sys/param.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <time.h>
#endif /*}*/
#endif /*}}*/

#undef  WIN32
#ifdef __CYGWIN__
#define WIN32
#undef _WIN32
#elif defined(_WIN32)
#define WIN32
#endif


namespace Dakota {


#ifdef Want_Heartbeat /*{*/
 static time_t start_time;

extern "C" void dak_sigcatch(int sig)
{
	struct rusage ru, ruc;
	unsigned long elapsed;

	if (getrusage(RUSAGE_SELF, &ru))
		std::memset(&ru, 0, sizeof(ru));
	if (getrusage(RUSAGE_SELF, &ruc))
		std::memset(&ruc, 0, sizeof(ru));
	elapsed = time(0) - start_time;
	std::printf("\n<<<<DAKOTA_HEARTBEAT seconds: elapsed %lu, cpu %.3g, child %.3g>>>>\n",
		elapsed, ru.ru_utime.tv_sec + ru.ru_stime.tv_sec
			+ 1e-6*(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec),
			ruc.ru_utime.tv_sec + ruc.ru_stime.tv_sec
			+ 1e-6*(ruc.ru_utime.tv_usec + ruc.ru_stime.tv_usec));
	std::fflush(stdout);
	signal(SIGALRM, dak_sigcatch);
	}
#endif /*Want_Heartbeat }*/

void start_dakota_heartbeat(int seconds)
{
#ifdef Want_Heartbeat /*{*/
	char *s;
	struct itimerval itv;
	static void(*oldsig)(int);
	void (*oldsig1)(int);

	start_time = time(0);

	if (seconds <= 0 && (s = std::getenv("DAKOTA_HEARTBEAT")))
		seconds = (int)std::strtol(s,0,10);
	if (seconds > 0) {
		std::memset(&itv, 0, sizeof(itv));
		itv.it_interval.tv_sec = itv.it_value.tv_sec = seconds;
		setitimer(ITIMER_REAL, &itv, 0);
		oldsig1 = signal(SIGALRM, dak_sigcatch);
		if (!oldsig)
			oldsig = oldsig1;
		}
	else if (oldsig) {
		signal(SIGALRM, oldsig);
		oldsig = 0;
		}
#endif /*Want_Heartbeat }*/
	}


/* Adding sftw.c, sftw.h and rec_cp, rec_rm here for simplicity... */

#include <fcntl.h>

/*************** sftw.h ****************/

/* Sorted variant of ftw -- directories read all at once and sorted, then visited. */
/* Written by David M. Gay.  This is similar to the variant of ftw that I wrote and */
/* used for years at Bell Labs. */

extern int sftw(const char *name,
	int (*fn)(const char *file, const struct stat*, int ftype, int depth, void *v),
	void *v);

/* name can be a file or directory */
/* depth = 0 on name, 1 on name's children, etc. */
/* v is passed without change to (*fn) */

enum {	/* ftype values pass to (*fn) */
	sFTW_F,		/* ordinary file */
	sFTW_SL,	/* symbolic link */
	sFTW_D,		/* start of directory */
	sFTW_DP,	/* end of directory */
	sFTW_DNR,	/* unreadable directory */
	sFTW_O,		/* other file type: socket, block or character special (etc.) */
	sFTW_NS		/* stat failed */
	};

enum {	/* return values from (*fn) */
	sFTWret_OK,	/* keep going */
	sFTWret_quit,	/* terminate */
	sFTWret_skipdir,/* skip (the rest of) this directory; no sFTW_DP */
	sFTWret_Follow,	/* follow symbolic link */
	sFTWret_mallocfailure
	};

/*************** sftw.c ****************/

/* Sorted treewalk (variant of ftw); written by David M. Gay */

/*#include "sftw.h"*/
#include <dirent.h>
#if 0
#include <stdlib.h>
#include <string.h>
#endif

 typedef int (*ftw_fn)(const char *file, const struct stat*, int ftype, int depth, void *v);

 typedef struct dirent dirent;

 static int ftw1(char *name, size_t namelen, size_t namemaxlen, ftw_fn fn, int, void *v);


 typedef struct
Cbuf {
	struct Cbuf *next;
	char buf[4080];
	} Cbuf;

 static int
compar(const void *a, const void *b)
{ return std::strcmp(*(const char**)a, *(const char**)b); }

 static int
dodir(DIR *dir, char *name, size_t namelen, size_t namemaxlen, ftw_fn fn, int depth, void *v, struct stat *sb)
{
	Cbuf *cb, cb0, *cb1;
	char *b, *be, **fn0, *fn00[256], **fn1, **fnames, **fne, *name1, *name2, *s;
	dirent *de;
	int rc;
	size_t L, L1, Lmax;

	cb = &cb0;
	cb0.next = 0;
	b = cb0.buf;
	be = b + sizeof(cb0.buf);
	fnames = fn0 = fn00;
	fne = fn00 + sizeof(fn00)/sizeof(char*);

	Lmax = 0;
	while(de = readdir(dir)) {
		L = std::strlen(s = de->d_name) + 1;
		if (L <= 3 && *s == '.') {
			if (L == 2 || s[1] == '.')
				continue;
			}
		if (Lmax < L)
			Lmax = L;
		if (be-b < L) {
			if (L > sizeof(cb0.buf)
			 || !(cb1 = (Cbuf*)std::malloc(sizeof(Cbuf)+L)))
				return sFTWret_mallocfailure;
			cb1->next = cb;
			cb = cb1;
			b = cb->buf;
			be = b + sizeof(cb0.buf)+L;
			}
		std::strcpy(b, s);
		if (fnames >= fne) {
			L1 = (fne-fn0)*sizeof(char*);
			fn1 = (char**)std::malloc(2*L1);
			if (!fn1)
				return sFTWret_mallocfailure;
			std::memcpy(fn1, fn0, L1);
			if (fn0 != fn00)
				std::free(fn0);
			fne = (char**)((char*)fn1 + 2*L1);
			fnames = (char**)((char*)fn1 + L1);
			fn0 = fn1;
			}
		*fnames++ = b;
		b += L;
		}
	closedir(dir);
	L = fnames - fn0;
	if (L == 0)
		return 0;
	if (L > 1)
		std::qsort(fn0, L, sizeof(char*), compar);
	name1 = name;
	++Lmax;
	if (namemaxlen < namelen + Lmax) {
		name1 = (char*)std::malloc(2*namemaxlen + Lmax);
		if (!name1)
			return sFTWret_mallocfailure;
		namemaxlen += namemaxlen + Lmax;
		std::strcpy(name1, name);
		}
	name1[namelen++] = '/';
	name2 = name1 + namelen;
	rc = 0;
	for(fn1 = fn0; fn1 < fnames; fn1++) {
		L = std::strlen(*fn1);
		std::strcpy(name2, *fn1);
		if (rc = ftw1(name1, namelen+L, namemaxlen, fn, depth, v))
			break;
		}
	name1[namelen-1] = 0;
	if (fn0 != fn00)
		std::free(fn0);
	while(cb1 = cb->next) {
		std::free(cb);
		cb = cb1;
		}
	return rc;
	}

 static int
ftw1(char *name, size_t namelen, size_t namemaxlen, ftw_fn fn, int depth, void *v)
{
	DIR *dir;
	int ftype, rc;
	struct stat sb;

#ifdef S_ISLNK
	if (lstat(name,&sb))
		ftype = sFTW_NS;
	else if (S_ISREG(sb.st_mode))
		ftype = sFTW_F;
	else if (S_ISDIR(sb.st_mode))
		goto do_dir;
	else if (S_ISLNK(sb.st_mode))
		goto do_slink;
	else
		ftype = sFTW_O;
	return (*fn)(name, &sb, ftype, depth, v);

 do_slink:
	if ((rc = (*fn)(name, &sb, sFTW_SL, depth, v)) != sFTWret_Follow)
		return rc;
	if (stat(name,&sb))
		ftype = sFTW_NS;
	else if (S_ISREG(sb.st_mode))
		ftype = sFTW_F;
	else if (S_ISDIR(sb.st_mode))
		goto do_dir;
	else
		ftype = sFTW_O;
	return (*fn)(name, &sb, ftype, depth, v);
#else /*!S_ISLNK*/
#ifndef S_ISDIR
#define	S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)	/* directory */
#define	S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#endif
	if (stat(name,&sb))
		ftype = sFTW_NS;
	else if (S_ISREG(sb.st_mode))
		ftype = sFTW_F;
	else if (S_ISDIR(sb.st_mode))
		goto do_dir;
	else
		ftype = sFTW_O;
	return (*fn)(name, &sb, ftype, depth, v);
#endif /*S_ISLNK*/

 do_dir:
	if (dir = opendir(name)) {
		switch(rc = (*fn)(name, &sb, sFTW_D, depth, v)) {
		  case sFTWret_OK: break;
		  case sFTWret_skipdir:
			rc = 0;
			/* no break */
		  default:
			closedir(dir);
			return rc;
		  }
		if (rc = dodir(dir, name, namelen, namemaxlen, fn, depth+1, v, &sb)) {
			if (rc == sFTWret_skipdir)
				rc = 0;
			return rc;
			}
		rc = sFTW_DP;
		}
	else
		rc = sFTW_DNR;
	return (*fn)(name, &sb, rc, depth, v);
	}

 int
sftw(const char *name, ftw_fn fn, void *v)
{
	char buf[DAK_MAXPATHLEN];
	size_t L = std::strlen(name);

	if (L >= sizeof(buf))
		return sFTWret_mallocfailure;
	std::strcpy(buf, name);
	return ftw1(buf, L, sizeof(buf), fn, 0, v);
	}


#ifndef _WIN32
 static int
Symlink(const char *from, const char *to)
{
#if 0
        char buf[DAK_MAXPATHLEN], *b;
        int rc;
        size_t L, L1;
        static size_t ddlen;

        b = buf;

        if (*from != '/') {
                if (!ddlen)
                        ddlen = WorkdirHelper::startup_pwd().size();
                L = std::strlen(from);
                L1 = L + ddlen + 2;
                if (L1 > sizeof(buf))
                        b = new char [L1];
                std::strcpy(b, WorkdirHelper::startup_pwd().c_str());
                b[ddlen] = '/';
                std::strcpy(b + ddlen + 1, from);
                from = b;
                }

        Cout << "oldSymlink from: " << from << '\n' << std::endl;

        rc = symlink(from, to);
        if (b != buf)
                delete [] b;
        return rc;
#endif
  std::string tmp("/");
  tmp += from;
  std::string adjusted_from( (*from != '/') ?
                             WorkdirHelper::startup_pwd() + tmp : from );

  //Cout << "newSymlink from: " << adjusted_from << '\n' << std::endl;

  return symlink(adjusted_from.c_str(), to);
}
#endif


/*************** rec_cp and rec_rmdir ****************/

 static int
my_recrm(const char *file, const struct stat *sb, int ftype, int depth, void *v)
{
	const char *what;

	switch(ftype) {
	 case sFTW_F:
	 case sFTW_NS:
#ifdef S_ISLNK
	 case sFTW_SL:
#endif
		if (unlink(file) && ++*(int*)v == 1)
			std::fprintf(stderr, "rec_rm: could not unlink \"%s\"\n", file);
	 case sFTW_D:
		break;
	 case sFTW_DP:
		if (rmdir(file) && ++*(int*)v == 1)
			std::fprintf(stderr, "rec_rm: could not remove directory \"%s\"\n", file);
		break;
	 case sFTW_DNR:
		what = "Unreadable_directory";
		goto bad;
	 case sFTW_O:
		what = "Mystery_file";
		goto bad;
	 default:
		what = "Bug:Surprise_ftype";
 bad:
		if (++*(int*)v == 1)
			std::fprintf(stderr, "\nrec_rm: %s\n", what);
	 }
	return 0;
	}

 int
rec_rmdir(const char *name)
{
	int nbad = 0, rc;
	rc = sftw(name, my_recrm, &nbad);
	if (nbad) {
		rc != 1;
		if (nbad > 1)
			std::fprintf(stderr, "%d other error(s) ignored.\n", nbad);
		}
	return rc;
	}

 typedef struct
Buf {
	char *buf;
	size_t buflen;
	} Buf;

 typedef struct
Finfo {
	Buf b[2];
	const char *fromdir, *todir;
	size_t fromlen, tolen;
	int copy, flatten, replace;
	} Finfo;

#ifndef REC_CP_BUFMAX
#define REC_CP_BUFMAX (1L << 24)	/* 2^24, about 1.6e7 */
#endif

#ifndef REC_CP_BUFMIN
#define REC_CP_BUFMIN (1L << 16)	/* 2^16, about 6.4e4 */
#endif

 static void
buf_incr(Buf *b, size_t Lt)
{
	char *s;
	size_t L, L1;

	if (!(s = b->buf))
		b->buflen = Lt >= REC_CP_BUFMIN ? REC_CP_BUFMIN : 4096;
	else
		std::free(s);
	for(L = L1 = b->buflen; L < Lt; L1 = L) {
		L <<= 1;
		if (L <= L1) {
			std::fprintf(stderr, "\nImpossible desired length %ld in buf_incr.\n",
				(long)L1);
			std::exit(1);
			}
		}
	if (!(b->buf = (char*)std::malloc(L))) {
		std::fprintf(stderr, "\nmalloc(%ld) failure in buf_incr.\n", (long)L);
		std::exit(1);
		}
	b->buflen = L;
	}

 int
my_cp(const char *file, const struct stat *sb, int ftype, int depth, void *v)
{
	Finfo *f = (Finfo*)v;
	char *s, *s1;
	const char *what;
	int ff, rc, tf;
	size_t L, Lf, Lt;
#ifdef _MSC_VER
   typedef __int64 ssize_t;
#endif
	ssize_t m, n;
	struct stat sb1;

	if (depth == 0 && f->flatten)
		return 0;

	switch(ftype) {
	 case sFTW_F:
	 case sFTW_D:
#ifdef S_ISLNK
	 case sFTW_SL:
#endif
		Lf = std::strlen(file + f->fromlen);
		Lt = f->tolen + Lf + 1;
		if (f->b[0].buflen <= Lt)
			buf_incr(f->b, Lt);
		std::memcpy(s = f->b[0].buf, f->todir, f->tolen);
		s1 = s + f->tolen;
		if (*(what = file + f->fromlen) != '/')
			*s1++ = '/';
		std::strcpy(s1, what);
		if (!stat(s, &sb1))
			if (f->replace)
				rec_rmdir(s);
			else {
				std::fprintf(stderr, "\nrec_cp: \"%s\" already exists.\n", s);
				return sFTWret_quit;
				}
		rc = 0;
		if (ftype == sFTW_D) {
			if (DAK_MKDIR(s, 0700)) {
				std::fprintf(stderr, "rec_cp: mkdir(\"%s\") failed.\n", s);
				rc = sFTWret_quit;
				}
			}
		else
#ifndef _WIN32
		     if (f->copy
			|| (Symlink(file,s) && link(file,s)))
#endif
				{
				L = sb->st_size;
				if (L > f->b[1].buflen) {
					if (L < REC_CP_BUFMIN)
						L = REC_CP_BUFMIN;
					else if (L > REC_CP_BUFMAX)
						L = REC_CP_BUFMAX;
					buf_incr(f->b+1, L);
					}
				if ((ff = open(file, O_RDONLY)) < 0) {
					std::fprintf(stderr, "\nrec_cp: could not open \"%s\".\n", file);
					return sFTWret_quit;
					}
#if defined( _MSC_VER ) || defined( __MINGW32__ )
#define S_IRUSR S_IREAD
#define S_IWUSR S_IWRITE
#define S_IXUSR S_IEXEC
#endif
				tf = creat(s, S_IRUSR | S_IWUSR | (sb->st_mode & S_IXUSR));
				if (tf < 0) {
					std::fprintf(stderr, "\nrec_cp: could not create \"%s\".\n",s);
					close(ff);
					return sFTWret_quit;
					}
				L = f->b[1].buflen;
				s = f->b[1].buf;
				rc = 0;
				while((n = read(ff, s,  L)) > 0)
					if ((m = write(tf, s, n)) != m) {
						std::fprintf(stderr,
						  "\nrec_cp: failure writing \"%s\"\n",
						  f->b[0].buf);
						rc = sFTWret_quit;
						break;
						}
				close(ff);
				close(tf);
				}
		return rc;
	 case sFTW_DP:
		break;
	 case sFTW_DNR:
		what = "Unreadable_directory";
		goto bad;
	 case sFTW_O:
		what = "Mystery_file";
		goto bad;
	 case sFTW_NS:
		std::fprintf(stderr, "\nrec_cp: cannot open \"%s\"\n", file);
		return sFTWret_quit;
	 default:
		what = "Bug:Surprise_ftype";
 bad:
		std::fprintf(stderr, "\nrec_cp: %s\n", what);
		return sFTWret_quit;
	 }
	return 0;
	}

 int
rec_cp(const char *from, const char *todir, int copy, int flatten, int replace)
{
	Finfo f;
	int i, rc;
	struct stat sb;

	if (stat(todir,&sb) || !S_ISDIR(sb.st_mode)) {
		std::fprintf(stderr, "\nrec_cp: \"%s\" is not a directory.\n", from);
		return 1;
		}
	std::memset(&f, 0, sizeof(f));
	f.fromdir = from;
	f.todir = todir;
	if ((f.flatten = flatten))
		f.fromlen = std::strlen(from);
	f.tolen = std::strlen(todir);
	f.copy = copy;
	f.replace = replace;
	rc = sftw(from, my_cp, &f);
	for(i = 0; i < 2; ++i)
		if (f.b[i].buf)
			std::free(f.b[i].buf);
	return rc;
	}

/* from ~dmgay/h/src/pathadjust.c */

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
static int dakdrive;
static char slmap[256] = { /* Identity except that slmap['\\'] == slmap[0x5c] == '/' */
			   /* (note that Win32 system calls treat \ and / alike in filenames) */
			   /* and a-z mapped to A-Z */
 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, '/',  0x5d, 0x5e, 0x5f,
 0x60, 'A',  'B',  'C',  'D',  'E',  'F',  'G',  'H',  'I',  'J',  'K',  'L',  'M',  'N',  'O',
 'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',  'X',  'Y',  'Z',  0x7b, 0x7c, 0x7d, 0x7e, 0x7f,
 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
#define Map(x) slmap[x]
#define N_wdpath 27
#else
// Should NOT be needed, but define anyway (candidate for replacement with BFS)
#define Map(x) x
#define N_wdpath 1
#endif

#ifdef DEBUG_GET_NPATH

 static char *
pathsimp(char *t0)
{
	char *f, *t, *to;
	int n;

	f = to = t0;
#ifdef _WIN32
	if (f[0] == '\\' && f[1] == '\\') {
		to[0] = to[1] = '\\';
		to += 2;
		f += 2;
		while(*f == '\\')
			++f;
		while(*f != '\\') {
			if (!*f)
				return f;
			*to++ = *f++;
			}
		*to++ = '\\';
		while(*f == '\\')
			++f;
		while(*f != '\\') {
			if (!*f)
				return f;
			*to++ = *f++;
			}
		while(*f == '\\')
			++f;
		*to++ = '\\';
		t0 = to;
		}
	else if (f[0] && f[1] == ':') {
		*to++ = *f++;
		*to++ = *f++;
		t0 = to;
		}
#endif
	while(*f)
		switch(Map(*to = *f++)) {
		case '/':
			if (!(n = to - t0))
				goto dflt;
			if (to[-1] == '.') {
				if (n == 1 || Map(to[-2]) == '/') {
					--to;
					continue;
					}
				if (n >= 3 && to[-2] == '.'
				 && Map(to[-3]) == '/') {
					if (n == 3) {
						to = t0 + 1;
						continue;
						}
					if (n > 4 && Map(to[-4]) == '.'
					 && Map(to[-5]) == '.'
					 && (n == 5 || Map(to[-6]) == '/'))
						goto dflt;
					t = to - 4;
					while(t > t0 && Map(t[-1]) != '/')
						--t;
					to = t;
					continue;
					}
				}
			else if (Map(to[-1]) == '/')
				continue;
#ifdef _WIN32
 dflt:
			*to = DAK_SLASH;
#endif
		default:
#ifndef _WIN32
 dflt:
#endif
			++to;
		}
	if (to-t0 >= 4 && to[-1] == '.' && to[-2] == '.'
	 && Map(to[-3]) == '/' && Map(to[-4]) != '/') {
		for(to -= 4; ; --to)
			if (to == t0 || Map(*to) == '/') {
				*to = 0;
				break;
				}
		}
	*to = 0;
	return to;
	}


// WJB:  Come back and re-enable if disabling does NOT work!
 void
get_npath(int appdrive, std::string *pnpath)
{
	char *p, *q, *q0;

	int c, dot, nrel, needcolon;
	size_t L;
#if defined(_WIN32) || defined(_WIN64)
	char *appdir, buf[DAK_MAXPATHLEN];
	int nrel2;
#endif

	appdrive = Map(appdrive & 0xff);

	const std::string cwd_str = get_cwd();
	const char* cwd = cwd_str.c_str();
	size_t wd_strlen = cwd_str.size();

	char* env_path = (char*)WorkdirHelper::dakPreferredEnvPath.c_str();
	size_t env_path_strlen = std::strlen(env_path);

#ifdef _WIN32
	if (appdrive && appdrive != dakdrive) {
		const char* s;
		buf[0] = appdrive;
		buf[1] = ':';
		buf[2] = 0;
		if (!SetCurrentDirectory(s = buf)) {
cd_fail:
			std::fprintf(stderr,
				"get_npath: SetCurrentDirectory(\"%s\") failed.\n",s);
			std::exit(1);
			}
		L = GetCurrentDirectory(sizeof(buf), buf);
		if (L <= 0 || L >= sizeof(buf)) {
			std::fprintf(stderr, "get_npath: GetCurrentDirectory() failed.\n");
			std::exit(1);
			}
		appdir = buf;
		appdir[L] = 0;

                // WJB - NOTE:  seems odd that non-WIN32 case has no chdir
		if (!SetCurrentDirectory(s = cwd))
			goto cd_fail;
		}
	else
		appdir = (char*)cwd;
	dot = 1;
	nrel2 = 0;
#else
	dot = 0;
#endif
	nrel = 0;
	for(p = env_path+5; *p; ++p) {
		switch(Map(*p)) {
		  case DAK_PATH_SEP:
			++dot;
			continue;
		  default:
#ifdef _WIN32
			c = 0;
			if (p[1] == ':') {
				c = Map(*p);
				p += 2;
				if (Map(*p) == '/')
					goto slashadj;
				if (c != appdrive && c != dakdrive)
					goto slashadj;
				}
			if (c == 0 || c == dakdrive)
				++nrel;
			else
				++nrel2;
			goto slashadj;
#else
			++nrel;
#endif
			/* no break */
		  case '/':
#ifdef _WIN32
 slashadj:
#endif
			while(*++p != DAK_PATH_SEP)
				if (!*p) {
					--p;
					break;
					}
			c = *p;
			if (p[1])
				continue;
#ifndef _WIN32
			if (c == ':')
				++dot;
#endif
		  }
		break;
		}
	if (dot) {
		++nrel;
		dot = 0;
		}
	else
		dot = 1;
#ifdef _WIN32
	L = env_path_strlen + nrel*(wd_strlen+1) + 1;
	if (nrel2)
		L += nrel2*(std::strlen(appdir) + 1);

	// WJB: for now, let the p/q funk go as long as pnpath is un-molested
	//pnpath_buf = q = new char [L];

	q = new char [L];
	std::memcpy(q, env_path, 5);
	std::memcpy(q += 5, cwd, wd_strlen);
	q += wd_strlen;
	dot = 1;
#else
	L = env_path_strlen + nrel*(wd_strlen+1) + 3;

	// WJB: for now, let the p/q funk go as long as pnpath is un-molested
	//pnpath_buf = q = new char [L];

	q = new char [L];
	std::memcpy(q, env_path, 5);
	q += 5;
	*q++ = '.';
#endif
	needcolon = 1;
	for(p = env_path+5; *p; ++p) {
		const char* s;
		switch(c = Map(*q = *p)) {
		  case DAK_PATH_SEP:
 dotcheck:
#ifndef _WIN32
			if (!dot++) {
				/* expand only the first "." */
				if (needcolon) {
					*q++ = ':';
					q0 = q;
					}
				for(s= cwd; (*q = *s); ++q, ++s);
				q = pathsimp(q0);
				needcolon = 1;
				}
#endif
			while(p[1] == DAK_PATH_SEP)
				++p;
			continue;
		  default:
			if (*p == '.' && (p[1] == DAK_PATH_SEP || !p[1]))
				goto dotcheck;
			if (needcolon) {
				*q++ = DAK_PATH_SEP;
				q0 = q;
				needcolon = 0;
				}
			s = cwd;
#ifdef _WIN32
			if (p[1] == ':') {
				*q = *p;
				if (Map(p[2]) == '/')
					goto justcopy;
				if (c == appdrive)
					s = appdir;
				else if (c != dakdrive)
					goto justcopy;
				p += 2;
				}
#endif
			for(; (*q = *s); ++q, ++s);
			*q++ = DAK_SLASH;
			*q = *p;
			/* no break */
		  case '/':
			if (needcolon) {
				*q++ = DAK_PATH_SEP;
				*q = *p;
				}
#ifdef _WIN32
 justcopy:
#endif
			q0 = q;
			for(;;) {
				if (*++p == DAK_PATH_SEP) {
					*++q = 0;
					break;
					}
				if (!(*++q = *p)) {
					--p;
					break;
					}
				}
			q = pathsimp(q0);
			needcolon = 1;
			if (p[1])
				continue;
			goto dotcheck;
		  }
		break;
		}

	// WJB: after ALL THE POINTER MANIP, restore PATH to its original state
	*pnpath = WorkdirHelper::dakPreferredEnvPath;
	//delete [] q;

#if defined(DEBUG)
	Cout << "get_npath: CWD=" << cwd << '\n'
	     << *pnpath << '\n' << std::endl;
#endif

	}
#endif // DEBUG_GET_NPATH


void workdir_adjust(const std::string& workdir)
{
  size_t appdrive = 0;

#if defined(_WIN32) || defined(_WIN64)
  if (workdir.c_str() && workdir[0] && workdir[1] == ':') {
    appdrive = Map(workdir[0] & 0xff);
    if (appdrive >= 'A' && appdrive <= 'Z')
      appdrive += 1 - 'A';
    else
      appdrive = 0;
  }
#endif

#ifdef DEBUG_GET_NPATH
  static boost::array<std::string, N_wdpath> wd_path = { {std::string()} };
  if ( wd_path[appdrive].empty() )
    get_npath(appdrive, &wd_path[appdrive]);
#endif

  if ( DAK_CHDIR(workdir.c_str()) ) {
    Cerr << "\nError: chdir(" << workdir
         << ") failed in workdir_adjust()" << std::endl;
    abort_handler(-1);
  }

#ifdef DEBUG_GET_NPATH
  if ( !wd_path[appdrive].empty() )
    putenv_impl( wd_path[appdrive].c_str() );
#endif
}


/** Portability adapter for getcwd: return the string in OS-native
    format.  TODO: change paths throughout code to use bfs::path where
    possible, since Windows (and Cygwin) use wchar_t instead of
    char_t. */
std::string get_cwd()
{
  // string is wchar on Windows and Cygwin, char elsewhere. This will
  // get the native path and return as a string, using locale-specific
  // conversion from wchar to char if needed.
  return bfs::current_path().string();
}

} // namespace Dakota
