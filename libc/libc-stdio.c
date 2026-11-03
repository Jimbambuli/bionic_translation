#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#include "libc-stdio.h"

// Bionic standard stream support pre-M Android
// Post-M it's saner and they point to stdin/stdout/stderr symbols instead
const struct bionic___sFILE bionic___sF[3] = {
    {{'s', 't', 'd', 'i', 'n'}},
    {{'s', 't', 'd', 'o', 'u', 't'}},
    {{'s', 't', 'd', 'e', 'r', 'r'}}};

// these only exist in bionic AFAICT

int __srget(FILE *fp)
{
	return fgetc(bionic_file_to_glibc_file(fp));
}

int __swbuf(int c, FILE *fp)
{
	return fputc(c, bionic_file_to_glibc_file(fp));
}

// libstdc++ uses these directly for standard streams, thus we need to wrap em
// and IO_file wraps aren't enough.

int bionic_fflush(FILE *f)
{
	return fflush(bionic_file_to_glibc_file(f));
}

size_t
bionic_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	return fwrite(ptr, size, nmemb, bionic_file_to_glibc_file(stream));
}

int bionic_putc(int ch, FILE *f)
{
	return putc(ch, bionic_file_to_glibc_file(f));
}

#ifdef __GLIBC__

// Pre-M code (e.g. libvlc) passes &__sF[n] to the stdio functions. glibc reads the FILE structure itself before
// any vtable call (fputc() checks the write buffer and calls __overflow(), which aborts with "invalid stdio
// handle"), so every function that may get a standard stream maps it, as on musl (libc-musl.c).

int bionic_fputc(int c, FILE *f) { return fputc(c, bionic_file_to_glibc_file(f)); }
int bionic_fputc_unlocked(int c, FILE *f) { return fputc_unlocked(c, bionic_file_to_glibc_file(f)); }
int bionic_putc_unlocked(int c, FILE *f) { return putc_unlocked(c, bionic_file_to_glibc_file(f)); }
int bionic_fputs(const char *s, FILE *f) { return fputs(s, bionic_file_to_glibc_file(f)); }
wint_t bionic_fputwc(wchar_t c, FILE *f) { return fputwc(c, bionic_file_to_glibc_file(f)); }
int bionic_vfprintf(FILE *f, const char *fmt, va_list ap) { return vfprintf(bionic_file_to_glibc_file(f), fmt, ap); }
int bionic_fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int ret = vfprintf(bionic_file_to_glibc_file(f), fmt, ap);
	va_end(ap);
	return ret;
}
int bionic_fgetc(FILE *f) { return fgetc(bionic_file_to_glibc_file(f)); }
int bionic_getc(FILE *f) { return getc(bionic_file_to_glibc_file(f)); }
int bionic_getc_unlocked(FILE *f) { return getc_unlocked(bionic_file_to_glibc_file(f)); }
int bionic_ungetc(int c, FILE *f) { return ungetc(c, bionic_file_to_glibc_file(f)); }
int bionic_fileno(FILE *f) { return fileno(bionic_file_to_glibc_file(f)); }
int bionic_ferror(FILE *f) { return ferror(bionic_file_to_glibc_file(f)); }
int bionic_feof(FILE *f) { return feof(bionic_file_to_glibc_file(f)); }
void bionic_clearerr(FILE *f) { clearerr(bionic_file_to_glibc_file(f)); }
void bionic_flockfile(FILE *f) { flockfile(bionic_file_to_glibc_file(f)); }
void bionic_funlockfile(FILE *f) { funlockfile(bionic_file_to_glibc_file(f)); }
int bionic_setvbuf(FILE *f, char *buf, int mode, size_t size) { return setvbuf(bionic_file_to_glibc_file(f), buf, mode, size); }
void bionic_setbuf(FILE *f, char *buf) { setbuf(bionic_file_to_glibc_file(f), buf); }

// Wrapping internal glibc VTABLE functions to handle bionic's pre-M crap
// We define __real_IO_file_xsputn in libc.c so linker will link our library,
// it's not used however for anything.

// no such luck with bionic, we need to wrap every single function that gets passed a FILE *

extern size_t __real_IO_file_xsputn(FILE *f, const void *buf, size_t n);

size_t __wrap_IO_file_xsputn(FILE *f, const void *buf, size_t n)
{
	return __real_IO_file_xsputn(bionic_file_to_glibc_file(f), buf, n);
}

#endif
