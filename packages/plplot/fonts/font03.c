/* $Id: font03.c 3186 2006-02-15 18:17:33Z slbrow $
   $Log$
   Revision 1.1  2006/02/15 18:15:44  slbrow
   This is the commit of the DAKOTA 'autoconfiscation' I've been working on, which
   enables builds using the GNU Autotools.  I have been merging in changes to the
   mainline the entire time I've been working on these changes to my working copy
   (in ~slbrow/projects/Dakota), so none of your changes to DAKOTA will be lost.
   In addition, the final pre-autotools version of DAKOTA has been tagged with:

   Version-3-3-Plus-pre-autotools

   The GNU autotools will be needed by all developers who check DAKOTA out from
   CVS with intentions to build; this is because the version of DAKOTA in the
   repository after this commit does not have any configure or Makefile.in files
   (since they are now machine-generated).  All the source tarballs rolled for
   external consumption (both release and votd) will automatically include all the
   configure and Makefile.in files our non-autotools savvy users will need, while
   still omitting those proprietary sources we may not distribute (DOT, NPSOL).

   To re-generate the missing configure and Makefile.in files in a checkout of
   DAKOTA, only one GNU Autotools script is necessary to run from the
   command-line:

   $ autoreconf --install

   This script (and all the scripts it runs in sequence on a copy of DAKOTA
   checked out of the repository) can be found on the SCICO LAN in the directory:

   /usr/netpub/autotools/bin

   This directory exists on all five of the DAKOTA nightly build platforms (AIX,
   IRIX, Linux, Solaris and Tru64), and should be added to the beginning of your
   $PATH if you are on the SCICO LAN.  If you are not, the versions of the tools
   you need can be easily downloaded from the GNU website.  The versions we use:

   autoconf 2.59  (http://ftp.gnu.org/gnu/autoconf/autoconf-2.59.tar.gz)
   automake 1.9.6 (http://ftp.gnu.org/gnu/automake/automake-1.9.6.tar.gz)
   libtool 1.5.22 (http://ftp.gnu.org/gnu/libtool/libtool-1.5.22.tar.gz)
   m4 1.4.3       (http://ftp.gnu.org/gnu/m4/m4-1.4.3.tar.gz)

   Here is a "short" summary of the major changes made in this )commit:

   1. All configure.in and Makefile.in files are no longer in the CVS repository.
      They are replaced by configure.ac and Makefile.am files, respectively.  When
      you run autoreconf, the configure and Makefile.in files are regenerated
      automatically and recursively.  No $DAKOTA environment variable needs to be
      set in order to build DAKOTA.

   2. Build directories as they were in DAKOTA using the Cygnus configure system
      no longer exist.  Executables built by the GNU Autotools (e.g., the dakota*
      binaries which were built in Dakota/src/i686-unknown-linux on Linux) are now
      built in Dakota/src; the autotools provide deployment targets for installing
      binaries and libraries in a deployment directory, as well as a simple means
      of maintaining more than one build of Dakota for multiple platforms (i.e.,
      using VPATH builds and source- and build-tree separation).

      Suppose you have in your home directory a checked-out and 'autoreconf'd copy
      of Dakota, and you want to simultaneously build versions of DAKOTA for Linux
      and Solaris.  You would create two uniquely named directories in your home
      directory, called 'my_linux_build' and 'my_solaris_build', respectively.
      Assuming two terminals are open on both machines, you would 'cd' into the
      appropriate build directory on each platform and run this command on both:

      $ ../Dakota/configure -C

      This will create a directory tree which is identical to the source tree, but
      containing only Makefiles.  Then, when you run 'make', it will traverse the
      build tree, referencing the sources by relative paths to create objects and
      binaries.  At your option, you can then delete these build directories
      without touching your source tree (the 'autoreconf'd checkout of Dakota).  I
      should note here that the GNU Autotools do not _force_ this separation of
      trees; you can do a 'make distclean' after building for each platform in
      your 'autoreconf'd copy of Dakota with no problems.

   3. All the configure flags which were supported in the old build system (i.e.,
      --without-acro, --enable-debugging, etc.), are supported in the new system
      with the following changes and additions:

      --with-mpi/--without-mpi           -> --enable-mpi/--disable-mpi
      --with-graphics/--without-graphics -> --enable-graphics/--disable-graphics
      --enable-docs (default is off)

      I should note that all the default settings have been preserved.  Further,
      since ACRO, OPT++ and DDACE haven't been integrated yet (see item 9), their
      flags are still commented out in 'Dakota/config/dak_vendoroptimizers.m4'.

   4. The epetra and plplot directories under VendorPackages have been completely
      replaced with the versions available online (versions 3.3 and 5.5.2,
      respectively), which were originally flattened to provide support for our
      build system.  Epetra has been altered from the version distributed by the
      Trilinos team to provide full libtool support as that is necessary for
      building mixed-language archives under Solaris, while plplot has had all
      language-bindings except C++ turned off by default.  Additionally, GSL has
      been updated from version 1.7 available online, but preserving the flattened
      structure it had prior to autoconfiscation.

   5. All other packages in the DAKOTA repository under VendorPackages and
      VendorOptimizers have been 'autoconfiscated' separately from DAKOTA.  To
      avoid duplicated configure-time probes, run configure from the top-level
      DAKOTA directory with the '-C' argument; this will cache the probe results
      from each subpackage, removing the need to do anything other than a lookup
      the next time the probe runs.

   6. No 'mpi' symbolic link needs to be made from VendorPackages/mpi to the MPI
      implementation you wish to build against; MPI wrapper scripts for the C++
      compiler are used instead; if you wish to change the default choices for the
      MPI implementation to build against, you can specify where the wrapper
      scripts you wish to use are via environment variables, which may be set
      on separate lines (using setenv in CSH), or with env on the configure line:

      $ env MPICC=mpicc MPICXX=mpiCC MPIF77=mpif77 ./configure -C

   7. All the clean-up make targets ('clean', 'distclean' and a new one for those
      unfamiliar with the GNU Autotools, 'maintainer-clean') are now machine-
      generated.  The targets remove increasing numbers of files as you would
      expect; the last one removes all the autotools-generated files, returning
      you to a pristine CVS checkout.

   8. Many changes have been made to the sources in Dakota/src:

      A) Fortran calls are now done using FC_FUNC and F77_FUNC macro wrappers,
         removing the need for #ifdef RS6K preprocessor checks for underscores.
      B) Platform testing preprocessor conditionals have been replaced* with the
         feature that was assumed to be supported on that platform 'a priori'
         * mostly: MPI implementation choice is still done by platform.
           See ParallelLibrary.C for details.
      C) TFLOPS and COUGAR #ifdef's have been removed, leaving their #else
         branches to execute unconditionally (as those machines have gone away).

   9. Since the GNU Autotools preferentially look for GCC compilers first; if you
      wish to build on Solaris with the Sun compilers you must set the compilers
      to use in environment variables in the same way the MPI wrappers must be set
      (see item 5 above):

       $ env CC=cc CXX=CC F77=f77 FC=f90 ./configure ...

   Additionally, there are some minor problems to fix with the new build system:

   10. This autoconfiscation covers Dakota, as integration with ACRO, OPT++ and
       DDACE has yet to be done.  I have integrated Surfpack and removed the
       separate ann and kriging packages from VendorPackages, based on positive
       results from some integration testing I've done on the Surfpack versions of
       ann and kriging against those in the separate versions in VendorPackages.

   11. Purely static-linked executables have proved difficult with libtool.  The
       way libtool sets things up, all the DAKOTA package libraries are static and
       linked as such, but system libraries are linked against dynamically (the
       way it is currently done on Solaris).  Strangely though, the MPI libraries
       aren't linked against dynamically, so this is an area for investigation
       (especially as I planned on having it working for the ASC White/Purple
       deployment of Dakota 4.0).

   12. Running 'make -j <n>' with positive integer n for parallel builds is not
       yet supported, as there are a couple packages with dependency requirements
       (LHS and IDR, namely) which preclude compiling in parallel until their
       requirements are met.  In LHS, those requirements arise from the need for
       all the *.mod module files to exist prior to anything which USE's them, and
       in IDR, they arise from the lex/yacc generated source dependencies.

   13. Disabling F90 support (via the --disable-f90 configure flag) doesn't work
       yet.  GNU Automake isn't quite smart enough yet to accept arbitrary
       suffixes for F90 files, so it expects that the suffixes will be .f90 or
       .f95, _not_ .F!  The Automake developers are working on this, so in the
       meantime, I'm using a hack.  I put that hack into Dakota/src/Makefile.am to
       address this situation (the same one I use in LHS, by the way, which sets
       F77 and FFLAGS to their F90-equivalents) doesn't work when F90 is disabled.

   14. The nightly build and test system (in Dakota/test/sqa) will need to be
       modified to use the new system, so the nightlies will be broken until I can
       get the scripts modified to use the new system.  That will be my next CVS
       checkin, and I don't expect it to take very long to make those mods.

   Revision 1.1  1992/05/20 21:33:22  furnish
   Initial checkin of the whole PLPLOT project.

*/

      short int subbuffer036[100] = {
       7863,   64, 8126, 8518, 8778, 8907, 9164, 9292, 9547, 9673,
       9671, 9538, 9275, 9272, 9399, 9527, 9784, 9913,10172,    0,
        -16, 7116, 6603, 7237, 6982, 6856, 6857, 6987, 7244, 7372,
       7627, 7753, 7751, 7618, 7486, 7223,   64, 7486, 7878, 8138,
       8267, 8524, 8780, 9035, 9161, 9159, 9026, 8763, 8760, 8887,
       9015, 9272, 9401, 9660,    0,  -16, 7116, 6987, 8524, 8139,
       7881, 7622, 7492, 7360, 7356, 7481, 7608, 7863, 8119, 8504,
       8762, 9021, 9151, 9283, 9287, 9162, 9035, 8780, 8524, 8266,
       8263, 8388, 8641, 8895, 9277, 9532,    0,  -16, 7116, 6733,
       8394, 8521, 8518, 8386, 8255, 8125, 7866, 7608, 7351, 7223
      };
      short int subbuffer037[100] = {
       7096, 7099, 7232, 7363, 7493, 7752, 8010, 8267, 8652, 9292,
       9547, 9674, 9800, 9797, 9667, 9538, 9281, 8897, 8642, 8515,
          0,  -16, 7116, 6988, 8646, 8516, 8387, 8130, 7874, 7748,
       7750, 7881, 8139, 8524, 8908, 9163, 9289, 9285, 9154, 8895,
       8379, 7993, 7736, 7351, 7095, 6968, 6970, 7099, 7355, 7610,
       7992, 8375, 8759, 9144, 9402,    0,  -16, 7116, 6733, 8394,
       8521, 8518, 8386, 8255, 8125, 7866, 7608, 7351, 7223, 7096,
       7099, 7232, 7363, 7493, 7752, 8010, 8267, 8652, 9164, 9419,
       9546, 9672, 9669, 9539, 9410, 9153, 8769, 8386, 8513, 8639,
       8634, 8760, 9015, 9272, 9401, 9660,    0,  -16, 7116, 6986
      };
      short int subbuffer038[100] = {
       6967, 7224, 7482, 7870, 8129, 8389, 8520, 8523, 8396, 8268,
       8139, 8009, 8007, 8133, 8387, 8769, 9023, 9149, 9147, 9017,
       8888, 8503, 7991, 7608, 7354, 7228, 7230,    0,  -16, 7116,
       6985, 8262, 8006, 7751, 7625, 7755, 8140, 8524, 9035, 9419,
       9676,   64, 9035, 8772, 8510, 8250, 7992, 7735, 7479, 7224,
       7098, 7100, 7229, 7485, 7740,    0,  -16, 7116, 6603, 7237,
       6982, 6856, 6857, 6987, 7244, 7372, 7627, 7753, 7751, 7619,
       7488, 7356, 7354, 7480, 7735, 7991, 8248, 8377, 8637, 9029,
       9292,   64, 9029, 8897, 8763, 8760, 8887, 9015, 9272, 9401,
       9660,    0,  -16, 7116, 6731, 7365, 7110, 6984, 6985, 7115
      };
      short int subbuffer039[100] = {
       7372, 7500, 7755, 7881, 7879, 7747, 7616, 7484, 7481, 7607,
       7863, 8120, 8507, 8766, 9026, 9157, 9289, 9291, 9164, 9036,
       8907, 8777, 8775, 8900, 9154, 9409,    0,  -16, 7116, 6349,
       6981, 6726, 6600, 6601, 6731, 6988, 7116, 7371, 7497, 7494,
       7351,   64, 8652, 7351,   64, 8652, 8375,   64,10188, 9931,
       9544, 9156, 8766, 8375,    0,  -16, 7116, 6732, 7750, 7494,
       7367, 7369, 7499, 7756, 8012, 8267, 8393, 8390, 8125, 8122,
       8248, 8503, 8759, 9016, 9146, 9148, 9021, 8765,   64, 9673,
       9675, 9548, 9292, 9035, 8777, 8518, 7997, 7738, 7480, 7223,
       6967, 6840, 6842,    0,  -16, 7116, 6731, 7365, 7110, 6984
      };
      short int subbuffer040[100] = {
       6985, 7115, 7372, 7500, 7755, 7881, 7879, 7747, 7616, 7484,
       7482, 7608, 7735, 7991, 8248, 8506, 8765, 8895, 9157,   64,
       9420, 9157, 8763, 8501, 8240, 7980, 7723, 7596, 7598, 7729,
       7988, 8375, 8761, 9404,    0,  -16, 7116, 6987, 8646, 8516,
       8387, 8130, 7874, 7748, 7750, 7881, 8139, 8524, 8908, 9163,
       9289, 9285, 9154, 8894, 8507, 7992, 7735, 7351, 7224, 7226,
       7355, 7739, 7994, 8121, 8247, 8244, 8113, 7983, 7724, 7467,
       7340, 7342, 7473, 7732, 8119, 8505, 9276,    0,  -16, 7116,
       7113, 7244, 8247,   64, 9292, 8247,   64, 7244, 9292,    0,
        -16, 7116, 7114, 9029, 9015,   64, 9026, 8772, 8517, 8133
      };
      short int subbuffer041[100] = {
       7876, 7618, 7487, 7485, 7610, 7864, 8119, 8503, 8760, 9018,
          0,  -16, 7116, 6985, 7500, 7479,   64, 7490, 7748, 8005,
       8389, 8644, 8898, 9023, 9021, 8890, 8632, 8375, 7991, 7736,
       7482,    0,  -16, 7116, 7113, 9026, 8772, 8517, 8133, 7876,
       7618, 7487, 7485, 7610, 7864, 8119, 8503, 8760, 9018,    0,
        -16, 7116, 7114, 9036, 9015,   64, 9026, 8772, 8517, 8133,
       7876, 7618, 7487, 7485, 7610, 7864, 8119, 8503, 8760, 9018,
          0,  -16, 7116, 7113, 7487, 9023, 9025, 8899, 8772, 8517,
       8133, 7876, 7618, 7487, 7485, 7610, 7864, 8119, 8503, 8760,
       9018,    0,  -16, 7116, 7623, 8908, 8652, 8395, 8264, 8247
      };
      short int subbuffer042[100] = {
         64, 7877, 8773,    0,  -16, 7116, 7114, 9029, 9013, 8882,
       8753, 8496, 8112, 7857,   64, 9026, 8772, 8517, 8133, 7876,
       7618, 7487, 7485, 7610, 7864, 8119, 8503, 8760, 9018,    0,
        -16, 7116, 7114, 7628, 7607,   64, 7617, 8004, 8261, 8645,
       8900, 9025, 9015,    0,  -16, 7116, 7748, 8140, 8267, 8396,
       8269, 8140,   64, 8261, 8247,    0,  -16, 7116, 7621, 8268,
       8395, 8524, 8397, 8268,   64, 8389, 8372, 8241, 7984, 7728,
          0,  -16, 7116, 7112, 7628, 7607,   64, 8901, 7611,   64,
       8127, 9015,    0,  -16, 7116, 7748, 8268, 8247,    0,  -16,
       7116, 6351, 6853, 6839,   64, 6849, 7236, 7493, 7877, 8132
      };
      short int subbuffer043[100] = {
       8257, 8247,   64, 8257, 8644, 8901, 9285, 9540, 9665, 9655,
          0,  -16, 7116, 7114, 7621, 7607,   64, 7617, 8004, 8261,
       8645, 8900, 9025, 9015,    0,  -16, 7116, 7114, 8133, 7876,
       7618, 7487, 7485, 7610, 7864, 8119, 8503, 8760, 9018, 9149,
       9151, 9026, 8772, 8517, 8133,    0,  -16, 7116, 6985, 7493,
       7472,   64, 7490, 7748, 8005, 8389, 8644, 8898, 9023, 9021,
       8890, 8632, 8375, 7991, 7736, 7482,    0,  -16, 7116, 7114,
       9029, 9008,   64, 9026, 8772, 8517, 8133, 7876, 7618, 7487,
       7485, 7610, 7864, 8119, 8503, 8760, 9018,    0,  -16, 7116,
       7366, 7877, 7863,   64, 7871, 8002, 8260, 8517, 8901,    0
      };
      short int subbuffer044[100] = {
        -16, 7116, 7241, 9026, 8900, 8517, 8133, 7748, 7618, 7744,
       7999, 8638, 8893, 9019, 9018, 8888, 8503, 8119, 7736, 7610,
          0,  -16, 7116, 7623, 8268, 8251, 8376, 8631, 8887,   64,
       7877, 8773,    0,  -16, 7116, 7114, 7621, 7611, 7736, 7991,
       8375, 8632, 9019,   64, 9029, 9015,    0,  -16, 7116, 7240,
       7493, 8247,   64, 9029, 8247,    0,  -16, 7116, 6859, 7237,
       7735,   64, 8261, 7735,   64, 8261, 8759,   64, 9285, 8759,
          0,  -16, 7116, 7241, 7621, 9015,   64, 9029, 7607,    0,
        -16, 7116, 7240, 7493, 8247,   64, 9029, 8247, 7987, 7729,
       7472, 7344,    0,  -16, 7116, 7241, 9029, 7607,   64, 7621
      };
      short int subbuffer045[100] = {
       9029,   64, 7607, 9015,    0,  -16, 7116, 6987, 8133, 7876,
       7618, 7488, 7357, 7354, 7480, 7735, 7991, 8248, 8635, 8894,
       9154, 9285,   64, 8133, 8389, 8516, 8642, 8890, 9016, 9143,
       9271,    0,  -16, 7116, 7114, 8652, 8395, 8137, 7877, 7746,
       7614, 7480, 7344,   64, 8652, 8908, 9162, 9159, 9029, 8900,
       8643, 8259,   64, 8259, 8514, 8768, 8894, 8891, 8761, 8632,
       8375, 8119, 7864, 7737, 7612,    0,  -16, 7116, 7114, 7234,
       7492, 7749, 7877, 8132, 8259, 8384, 8380, 8247,   64, 9285,
       9154, 9024, 8247, 7987, 7856,    0,  -16, 7116, 7113, 8517,
       8133, 7876, 7618, 7487, 7484, 7609, 7736, 7991, 8247, 8504
      };
      short int subbuffer046[100] = {
       8762, 8893, 8896, 8771, 8517, 8263, 8137, 8139, 8268, 8524,
       8779, 9033,    0,  -16, 7116, 7240, 8899, 8772, 8517, 8133,
       7876, 7874, 8000, 8383,   64, 8383, 7870, 7612, 7610, 7736,
       7991, 8375, 8632, 8890,    0,  -16, 7116, 7239, 8524, 8267,
       8138, 8137, 8264, 8647, 9031,   64, 9031, 8517, 8131, 7744,
       7613, 7611, 7737, 7991, 8373, 8499, 8497, 8368, 8112, 7986,
          0,  -16, 7116, 6986, 7105, 7235, 7493, 7749, 7876, 7874,
       7742, 7479,   64, 7742, 8002, 8260, 8517, 8773, 9027, 9024,
       8891, 8496,    0,  -16, 7116, 6858, 6977, 7107, 7365, 7621,
       7748, 7746, 7613, 7610, 7736, 7863, 8119, 8376, 8635, 8765
      };
      short int subbuffer047[100] = {
       8896, 9029, 9032, 8907, 8652, 8396, 8266, 8264, 8389, 8642,
       8896, 9278,    0,  -16, 7116, 7493, 8261, 7998, 7866, 7864,
       7991, 8247, 8505, 8635,    0,  -16, 7116, 7113, 7877, 7351,
         64, 9156, 9029, 8901, 8644, 8128, 7871, 7743,   64, 7743,
       7998, 8125, 8376, 8503, 8631, 8760,    0,  -16, 7116, 7240,
       7372, 7628, 7883, 8010, 9015,   64, 8261, 7479,    0,  -16,
       7116, 6987, 7877, 7088,   64, 7745, 7612, 7609, 7863, 8119,
       8376, 8634, 8894,   64, 9157, 8894, 8762, 8760, 8887, 9143,
       9401, 9531,    0,  -16, 7116, 7113, 7493, 7877, 7743, 7610,
       7479,   64, 9157, 9026, 8896, 8637, 8250, 7864, 7479,    0
      };
      short int subbuffer048[100] = {
        -16, 7116, 7240, 8524, 8267, 8138, 8137, 8264, 8647, 9031,
         64, 8647, 8262, 8005, 7875, 7873, 8127, 8510, 8766,   64,
       8510, 7997, 7740, 7610, 7608, 7862, 8372, 8499, 8497, 8240,
       7984,    0,  -16, 7116, 7241, 8261, 8004, 7746, 7615, 7612,
       7737, 7864, 8119, 8375, 8632, 8890, 9021, 9024, 8899, 8772,
       8517, 8261,    0,  -16, 7116, 6859, 8005, 7479,   64, 8645,
       8767, 8890, 9015,   64, 7106, 7364, 7749, 9413,    0,  -16,
       7116, 7113, 7615, 7612, 7737, 7864, 8119, 8375, 8632, 8890,
       9021, 9024, 8899, 8772, 8517, 8261, 8004, 7746, 7615, 7088,
          0,  -16, 7116, 7115, 9413, 8133, 7876, 7618, 7487, 7484
      };
      short int subbuffer049[100] = {
       7609, 7736, 7991, 8247, 8504, 8762, 8893, 8896, 8771, 8644,
       8389,    0,  -16, 7116, 6986, 8389, 7991,   64, 7234, 7492,
       7877, 9285,    0,  -16, 7116, 6986, 7105, 7235, 7493, 7749,
       7876, 7874, 7612, 7609, 7863, 8119, 8504, 8762, 9022, 9154,
       9157,    0,  -16, 7116, 6859, 7876, 7619, 7361, 7230, 7227,
       7353, 7480, 7735, 8119, 8504, 8890, 9149, 9280, 9283, 9029,
       8773, 8515, 8255, 7994, 7600,    0,  -16, 7116, 7113, 7365,
       7621, 7875, 8626, 8880, 9136,   64, 9285, 9155, 8896, 7605,
       7346, 7216,    0,  -16, 7116, 6731, 8780, 7728,   64, 6849,
       6979, 7237, 7493, 7620, 7618, 7485, 7482, 7608, 7863, 8119
      };
      short int subbuffer050[100] = {
       8504, 8762, 9021, 9282, 9413,    0,  -16, 7116, 6731, 7749,
       7492, 7233, 7102, 7099, 7224, 7351, 7607, 7864, 8123,   64,
       8255, 8123, 8248, 8375, 8631, 8888, 9147, 9278, 9281, 9156,
       9029,    0,  -16, 7116, 7498, 8637, 8511, 8256, 8000, 7743,
       7614, 7484, 7482, 7608, 7863, 8119, 8376, 8506, 8768, 8635,
       8632, 8759, 8887, 9144, 9273, 9532,    0,  -16, 7116, 7625,
       7612, 7871, 8260, 8390, 8521, 8523, 8396, 8139, 8009, 7877,
       7742, 7736, 7863, 7991, 8248, 8506, 8637, 8640, 8764, 8891,
       9147, 9404,    0,  -16, 7116, 7622, 8510, 8511, 8384, 8128,
       7871, 7742, 7612, 7610, 7736, 7991, 8375, 8761, 9020,    0
      };
      short int subbuffer051[100] = {
        -16, 7116, 7498, 8637, 8511, 8256, 8000, 7743, 7614, 7484,
       7482, 7608, 7863, 8119, 8376, 8506, 9292,   64, 8768, 8635,
       8632, 8759, 8887, 9144, 9273, 9532,    0,  -16, 7116, 7750,
       7865, 8122, 8251, 8381, 8383, 8256, 8128, 7871, 7741, 7738,
       7864, 8119, 8375, 8632, 8761, 9020,    0,  -16, 7116, 7877,
       7868, 8385, 8644, 8774, 8905, 8907, 8780, 8523, 8393, 8129,
       7736, 7345, 7214, 7212, 7339, 7596, 7727, 7864, 7991, 8247,
       8504, 8633, 8892,    0,  -16, 7116, 7497, 8637, 8511, 8256,
       8000, 7743, 7614, 7484, 7482, 7608, 7863, 8119, 8376, 8505,
         64, 8768, 8505, 7982, 7852, 7595, 7468, 7470, 7601, 7988
      };
      short int subbuffer052[100] = {
       8374, 8631, 9017, 9404,    0,  -16, 7116, 7626, 7612, 7871,
       8260, 8390, 8521, 8523, 8396, 8139, 8009, 7877, 7743, 7607,
         64, 7607, 7738, 7868, 8127, 8384, 8640, 8767, 8765, 8634,
       8632, 8759, 8887, 9144, 9273, 9532,    0,  -16, 7116, 8005,
       8389, 8388, 8516, 8517, 8389,   64, 7996, 8256, 7994, 7992,
       8119, 8247, 8504, 8633, 8892,    0,  -16, 7116, 8005, 8389,
       8388, 8516, 8517, 8389,   64, 7996, 8256, 7470, 7340, 7083,
       6956, 6958, 7089, 7476, 7862, 8119, 8505, 8892,    0,  -16,
       7116, 7625, 7612, 7871, 8260, 8390, 8521, 8523, 8396, 8139,
       8009, 7877, 7743, 7607,   64, 7607, 7738, 7868, 8127, 8384
      };
      short int subbuffer053[100] = {
       8640, 8767, 8765, 8508, 8124,   64, 8124, 8379, 8504, 8631,
       8759, 9016, 9145, 9404,    0,  -16, 7116, 7877, 7868, 8127,
       8516, 8646, 8777, 8779, 8652, 8395, 8265, 8133, 7998, 7992,
       8119, 8247, 8504, 8633, 8892,    0,  -16, 7116, 6604, 6588,
       6847, 7104, 7231, 7230, 7098, 6967,   64, 7098, 7228, 7487,
       7744, 8000, 8127, 8126, 7994, 7863,   64, 7994, 8124, 8383,
       8640, 8896, 9023, 9021, 8890, 8888, 9015, 9143, 9400, 9529,
       9788,    0,  -16, 7116, 7242, 7228, 7487, 7744, 7871, 7870,
       7738, 7607,   64, 7738, 7868, 8127, 8384, 8640, 8767, 8765,
       8634, 8632, 8759, 8887, 9144, 9273, 9532,    0,  -16, 7116
      };
      short int subbuffer054[100] = {
       7496, 8256, 8000, 7743, 7614, 7484, 7482, 7608, 7863, 8119,
       8376, 8505, 8635, 8637, 8511, 8256, 8127, 8125, 8251, 8506,
       8890, 9147, 9276,    0,  -16, 7116, 7368, 7356, 7615, 7745,
       7613, 6827,   64, 7613, 7743, 8000, 8256, 8511, 8637, 8635,
       8505, 8376, 8119,   64, 7608, 7863, 8247, 8632, 8889, 9276,
          0,  -16, 7116, 7497, 8637, 8511, 8256, 8000, 7743, 7614,
       7484, 7482, 7608, 7863, 8119, 8376,   64, 8768, 8637, 8376,
       7985, 7854, 7852, 7979, 8236, 8367, 8374, 8631, 9017, 9404,
          0,  -16, 7116, 7624, 7612, 7871, 8001, 7999, 8383, 8510,
       8508, 8377, 8376, 8503, 8631, 8888, 9017, 9276,    0,  -16
      };
      short int subbuffer055[100] = {
       7116, 7751, 7740, 7999, 8129, 8127, 8380, 8506, 8504, 8247,
         64, 7736, 7991, 8503, 8760, 8889, 9148,    0,  -16, 7116,
       7878, 7868, 8127, 8387,   64, 8780, 7994, 7992, 8119, 8375,
       8632, 8761, 9020,   64, 8004, 8900,    0,  -16, 7116, 7497,
       7484, 7744, 7482, 7480, 7607, 7863, 8120, 8378, 8637,   64,
       8768, 8506, 8504, 8631, 8759, 9016, 9145, 9404,    0,  -16,
       7116, 7497, 7484, 7744, 7611, 7608, 7735, 7863, 8248, 8506,
       8637, 8640,   64, 8640, 8764, 8891, 9147, 9404,    0,  -16,
       7116, 7116, 7488, 7230, 7099, 7097, 7223, 7479, 7736, 7994,
         64, 8256, 7994, 7992, 8119, 8375, 8632, 8890, 9021, 9024
      };
      short int subbuffer056[100] = {
         64, 9024, 9148, 9275, 9531, 9788,    0,  -16, 7116, 7240,
       7228, 7487, 7744, 8000, 8127, 8120, 8247, 8631, 9017, 9276,
         64, 8895, 8768, 8512, 8383, 7864, 7735, 7479, 7352,    0,
        -16, 7116, 7497, 7484, 7744, 7482, 7480, 7607, 7863, 8120,
       8378, 8637,   64, 8768, 7982, 7852, 7595, 7468, 7470, 7601,
       7988, 8374, 8631, 9017, 9404,    0,  -16, 7116, 7496, 7484,
       7743, 8000, 8256, 8510, 8508, 8378, 8120, 7735, 7990, 8116,
       8113, 7982, 7852, 7595, 7468, 7470, 7601, 7988, 8374, 8889,
       9276,    0,  -16, 7116, 7242, 7351, 7736, 8122, 8380, 8639,
       8899, 9031, 9033, 8907, 8652, 8395, 8266, 8136, 8003, 7998
      };
      short int subbuffer057[100] = {
       8122, 8248, 8503, 8759, 9016, 9145,    0,  -16, 7116, 7113,
       8893, 8896, 8771, 8644, 8389, 8133, 7876, 7618, 7487, 7484,
       7609, 7736, 7991, 8247, 8504, 8762, 8893, 9026, 9031, 8906,
       8779, 8524, 8268, 8011, 7753,    0,  -16, 7116, 7240, 8900,
       8645, 8261, 8004, 7746, 7615, 7612, 7737, 7864, 8119, 8503,
       8760,   64, 7614, 8638,    0,  -16, 7116, 7241, 8524, 8267,
       8008, 7878, 7747, 7614, 7610, 7736, 7863, 8119, 8376, 8635,
       8765, 8896, 9029, 9033, 8907, 8780, 8524,   64, 7746, 8898,
          0,  -16, 7116, 6986, 8780, 7728,   64, 8133, 7748, 7490,
       7359, 7356, 7482, 7736, 8119, 8375, 8760, 9018, 9149, 9152
      };
      short int subbuffer058[100] = {
       9026, 8772, 8389, 8133,    0,  -16, 7116, 7113, 9027, 8900,
       8517, 8133, 7748, 7619, 7489, 7487, 7613, 7867, 8376, 8502,
       8500, 8371, 8115,    0,  -16, 7116, 8256,   64,    0,  -16,
       7116, 7748,   64,    0,  -16, 7116, 7240,   64,    0,  -16,
       7116, 6986, 8140, 7755, 7496, 7363, 7360, 7483, 7736, 8119,
       8375, 8760, 9019, 9152, 9155, 9032, 8779, 8396, 8140,    0,
        -16, 7116, 6986, 7752, 8009, 8396, 8375,    0,  -16, 7116,
       6986, 7495, 7496, 7626, 7755, 8012, 8524, 8779, 8906, 9032,
       9030, 8900, 8641, 7351, 9143,    0,  -16, 7116, 6986, 7628,
       9036, 8260, 8644, 8899, 9026, 9151, 9149, 9018, 8760, 8375
      };
      short int subbuffer059[100] = {
       7991, 7608, 7481, 7355,    0,  -16, 7116, 6986, 8652, 7358,
       9278,   64, 8652, 8631,    0,  -16, 7116, 6986, 8908, 7628,
       7491, 7620, 8005, 8389, 8772, 9026, 9151, 9149, 9018, 8760,
       8375, 7991, 7608, 7481, 7355,    0,  -16, 7116, 6986, 9033,
       8907, 8524, 8268, 7883, 7624, 7491, 7486, 7610, 7864, 8247,
       8375, 8760, 9018, 9149, 9150, 9025, 8771, 8388, 8260, 7875,
       7617, 7486,    0,  -16, 7116, 6986, 9164, 7863,   64, 7372,
       9164,    0,  -16, 7116, 6986, 8012, 7627, 7497, 7495, 7621,
       7876, 8387, 8770, 9024, 9150, 9147, 9017, 8888, 8503, 7991,
       7608, 7481, 7355, 7358, 7488, 7746, 8131, 8644, 8901, 9031
      };
      short int subbuffer060[100] = {
       9033, 8907, 8524, 8012,    0,  -16, 7116, 6986, 9029, 8898,
       8640, 8255, 8127, 7744, 7490, 7365, 7366, 7497, 7755, 8140,
       8268, 8651, 8905, 9029, 9024, 8891, 8632, 8247, 7991, 7608,
       7482,    0,  -16, 7116, 7621, 8249, 8120, 8247, 8376, 8249,
          0,  -16, 7116, 7621, 8376, 8247, 8120, 8249, 8376, 8374,
       8244, 8115,    0,  -16, 7116, 7621, 8261, 8132, 8259, 8388,
       8261,   64, 8249, 8120, 8247, 8376, 8249,    0,  -16, 7116,
       7621, 8261, 8132, 8259, 8388, 8261,   64, 8376, 8247, 8120,
       8249, 8376, 8374, 8244, 8115,    0,  -16, 7116, 7621, 8268,
       8254,   64, 8249, 8120, 8247, 8376, 8249,    0,  -16, 7116
      };
      short int subbuffer061[100] = {
       7113, 7495, 7496, 7626, 7755, 8012, 8524, 8779, 8906, 9032,
       9030, 8900, 8771, 8257, 8254,   64, 8249, 8120, 8247, 8376,
       8249,    0,  -16, 7116, 7748, 8268, 8261,    0,  -16, 7116,
       7240, 7756, 7749,   64, 8780, 8773,    0,  -16, 7116, 7367,
       8140, 7883, 7753, 7751, 7877, 8132, 8388, 8645, 8775, 8777,
       8651, 8396, 8140,    0,  -16, 7116, 6986, 8016, 7987,   64,
       8528, 8499,   64, 9161, 8907, 8524, 8012, 7627, 7369, 7367,
       7493, 7620, 7875, 8641, 8896, 9023, 9149, 9146, 8888, 8503,
       7991, 7608, 7354,    0,  -16, 7116, 6859, 9424, 7088,    0,
        -16, 7116, 7367, 8784, 8526, 8267, 8007, 7874, 7870, 7993
      };
      short int subbuffer062[100] = {
       8245, 8498, 8752,    0,  -16, 7116, 7367, 7760, 8014, 8267,
       8519, 8642, 8638, 8505, 8245, 7986, 7728,    0,  -16, 7116,
       7748, 8272, 8240,    0,  -16, 7116, 6605, 7104, 9408,    0,
        -16, 7116, 6605, 8265, 8247,   64, 7104, 9408,    0,  -16,
       7116, 6605, 7107, 9411,   64, 7101, 9405,    0,  -16, 7116,
       6859, 7367, 9145,   64, 9159, 7353,    0,  -16, 7116, 7240,
       8262, 8250,   64, 7619, 8893,   64, 8899, 7613,    0,  -16,
       7116, 7621, 8257, 8128, 8255, 8384, 8257,    0,  -16, 7116,
       7621, 8396, 8267, 8137, 8135, 8262, 8391, 8264,    0,  -16,
       7116, 7621, 8266, 8139, 8268, 8395, 8393, 8263, 8134,    0
      };
      short int subbuffer063[100] = {
        -16, 7116, 6605, 8645, 9026, 9408, 9022, 8635,   64, 7104,
       9408,    0,  -16, 7116, 6987, 8400, 7472,   64, 9168, 8240,
         64, 7491, 9283,   64, 7357, 9149,    0,  -16, 7116, 6605,
       9539, 9540, 9413, 9285, 9156, 9026, 8765, 8506, 8248, 7991,
       7479, 7224, 7097, 6971, 6973, 7103, 7232, 8132, 8261, 8391,
       8393, 8267, 8012, 7755, 7625, 7623, 7748, 8001, 8634, 8888,
       9143, 9399, 9528, 9529,    0,  -16, 7116, 6859, 7113, 7239,
       7363, 7357, 7225, 7095,   64, 9417, 9287, 9155, 9149, 9273,
       9399,   64, 7113, 7368, 7879, 8647, 9160, 9417,   64, 7095,
       7352, 7865, 8633, 9144, 9399,    0,  -16, 7116, 7367, 7888
      };
      short int subbuffer064[100] = {
       7863,   64, 8656, 8631,    0,  -16, 7116, 6732, 8272, 8247,
         64, 7095, 9399,    0,  -16, 7116, 6732, 9424, 7095, 9399,
          0,  -16, 7116, 6605, 8265, 8136, 8263, 8392, 8265,   64,
       7097, 6968, 7095, 7224, 7097,   64, 9401, 9272, 9399, 9528,
       9401,    0,  -16, 7116, 6732, 8266, 7750, 7362, 7231, 7229,
       7355, 7610, 7866, 8123, 8253,   64, 8266, 8774, 9154, 9279,
       9277, 9147, 8890, 8634, 8379, 8253,   64, 8253, 8121, 7990,
         64, 8253, 8377, 8502,   64, 7990, 8502,    0,  -16, 7116,
       6732, 8260, 8135, 8009, 7754, 7626, 7369, 7239, 7235, 7360,
       7486, 7739, 8246,   64, 8260, 8391, 8521, 8778, 8906, 9161
      };
      short int subbuffer065[100] = {
       9287, 9283, 9152, 9022, 8763, 8246,    0,  -16, 7116, 6732,
       8267, 8008, 7491, 7104,   64, 8267, 8520, 9027, 9408,   64,
       7104, 7485, 7992, 8245,   64, 9408, 9021, 8504, 8245,    0,
        -16, 7116, 6732, 8254, 8507, 8762, 9018, 9275, 9405, 9407,
       9281, 9026, 8770, 8385,   64, 8385, 8643, 8773, 8775, 8649,
       8394, 8138, 7881, 7751, 7749, 7875, 8129,   64, 8129, 7746,
       7490, 7233, 7103, 7101, 7227, 7482, 7738, 7995, 8254,   64,
       8254, 8121, 7990,   64, 8254, 8377, 8502,   64, 7990, 8502,
          0,  -16, 7116, 6732, 8256, 8247, 8118,   64, 8252, 8118,
         64, 8265, 8138, 7882, 7753, 7751, 7876, 8256,   64, 8265
      };
      short int subbuffer066[100] = {
       8394, 8650, 8777, 8775, 8644, 8256,   64, 8256, 7747, 7492,
       7236, 7107, 7105, 7232,   64, 8256, 8771, 9028, 9284, 9411,
       9409, 9280,   64, 8256, 7741, 7484, 7228, 7101, 7103, 7232,
         64, 8256, 8765, 9020, 9276, 9405, 9407, 9280,    0,  -16,
       7116, 6732, 7103, 7231, 7486, 7612, 7610, 7480,   64, 7103,
       7104, 7233, 7489, 7616, 7742, 7739, 7609, 7480,   64, 8267,
       8009, 7878, 7875, 8125, 8122, 7992, 8246,   64, 8267, 8521,
       8646, 8643, 8381, 8378, 8504, 8246,   64, 9407, 9408, 9281,
       9025, 8896, 8766, 8763, 8889, 9016,   64, 9407, 9279, 9022,
       8892, 8890, 9016,   64, 7356, 9148,    0,  -16, 7116, 8001
      };
