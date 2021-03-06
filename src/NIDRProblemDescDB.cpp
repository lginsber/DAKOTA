/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright (c) 2010, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

// Class:        NIDRProblemDescDB
//- Description: Implementation code for the New IDR ProblemDescDB class.
//-              It defines the keyword handlers that yacc calls to populate
//-              the database based on the parsed input.
//- Owner:       David M. Gay
//- Checked by:
//- Version: $Id$

#include "NIDRProblemDescDB.hpp"
#include "ParallelLibrary.hpp"
#include "WorkdirHelper.hpp"     // for DAK_MKDIR, prepend_preferred_env_path
#include "dakota_filesystem_utils.hpp"  // for get_cwd()
#include "pecos_stat_util.hpp"
#include <functional>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef HAVE_OPTPP
#include "globals.h"
#endif

#ifdef DAKOTA_DL_SOLVER
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#define dlopen(x,y) LoadLibrary(x)
#else
#include <dlfcn.h>
#endif
#endif /*DAKOTA_DL_SOLVER*/

extern "C" void nidr_lib_cleanup(void);

namespace Dakota {
extern ProblemDescDB *Dak_pddb;

extern "C" FILE *nidrin;
extern "C" int nidr_parse(const char*, FILE*);

int NIDRProblemDescDB::nerr = 0;
NIDRProblemDescDB* NIDRProblemDescDB::pDDBInstance(NULL);


NIDRProblemDescDB::NIDRProblemDescDB(ParallelLibrary& parallel_lib):
  ProblemDescDB(BaseConstructor(), parallel_lib)
{}


NIDRProblemDescDB::~NIDRProblemDescDB()
{
#ifndef NO_NIDR_DYNLIB
  nidr_lib_cleanup(); // close any explicitly opened shared libraries
#endif
}

void NIDRProblemDescDB::botch(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "\nError: ");
  std::vfprintf(stderr, fmt, ap);
  std::fputs(".\n", stderr);
  va_end(ap);
  abort_handler(-1);
}

void NIDRProblemDescDB::squawk(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "\nError: ");
  std::vfprintf(stderr, fmt, ap);
  std::fputs(".\n", stderr);
  va_end(ap);
  ++nerr;
}

void NIDRProblemDescDB::warn(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  std::fprintf(stderr, "\nWarning: ");
  std::vfprintf(stderr, fmt, ap);
  std::fputs(".\n", stderr);
  va_end(ap);
}

/** Parse the input file using the Input Deck Reader (IDR) parsing system.
    IDR populates the IDRProblemDescDB object with the input file data. */
void NIDRProblemDescDB::
derived_parse_inputs(const char* dakota_input_file, const char* parser_options)
{
  // set the pDDBInstance
  pDDBInstance = this;

  // Open the dakota input file passed in and "attach" it to stdin
  // (required by nidr_parse)
  if (dakota_input_file) {
    if (dakota_input_file[0] == '-' && !dakota_input_file[1])
      nidrin = stdin;
    else if( !(nidrin = std::fopen(dakota_input_file, "r")) )
      botch("cannot open \"%s\"", dakota_input_file);
  }

  // nidr_parse parses the input file and invokes the keyword handlers
  FILE *dump_file = NULL;
  if (nidr_parse(parser_options, dump_file)) {
    //Cerr << "\nErrors parsing input file." << std::endl;
    abort_handler(-1); // allows proper abort in parallel case
  }
  if (nerr)
    abort_handler(-1);
  if (parallel_library().command_line_run()) {
    const char *s;
#ifdef DAKOTA_DL_SOLVER
    std::list<DataMethod>::iterator
      Dml  = dataMethodList.begin(),
      Dmle = dataMethodList.end();
    DataMethodRep *Dr;
    const char *s0;
    char *s1;
    size_t L;

    for(; Dml != Dmle; ++Dml) {
      Dr = Dml->dataMethodRep;
      if ((s0 = Dr->dlDetails) && *(s = s0) > ' ') {
	while(*s > ' ')
	  ++s;
	s1 = 0;
	if (*s) {
	  L = s - s0;
	  s1 = new char[L+1];
	  memcpy(s1, s0, L);
	  s1[L] = 0;
	  s0 = s1;
	}
	if (!(Dr->dlLib = dlopen(s0, RTLD_NOW))) {
#ifndef _WIN32
	  const char *se;
	  if ((se = dlerror()))
	    squawk("Cannot open dl_solver \"%s\":\n\t%s", s0, se);
	  else
#endif
	    squawk("Cannot open dl_solver \"%s\"", s0);
	}
	if (s1)
	  delete[] s1;
      }
    }
#endif
    if (parallel_library().mpirun_flag()) {
      std::list<DataInterface>::iterator
	Ifl  = dataInterfaceList.begin(),
	Ifle = dataInterfaceList.end();
      DataInterfaceRep *Ir;
      struct stat sb;

      for(; Ifl != Ifle; Ifl++) {
	Ir = Ifl->dataIfaceRep;
	if ((s = (Ir->workDir).c_str()) && *s && stat(s,&sb))
	  DAK_MKDIR(s, 0700);
      }
    }
  }
}


void NIDRProblemDescDB::derived_broadcast()
{ check_variables(&dataVariablesList); check_responses(&dataResponsesList); }
// check_variables() invokes check_variables_node(), either directly or after
// some manip of interval/histogram/correlation data.  check_variables_node
// does label processing (?) followed by additional processing in
// make_variable_defaults() below, which calls BuildLabels().  Need to
// understand the reason for this step.

// Basic flow should be:
// (1) db.parse_inputs(file), db.insert_node(), db.set(), or some combination
//     only on rank 0.
// (2) perform sanity checking (once) on sets of vector inputs across all kw's,
//     but for only those vector inputs that have been provided (see 4).
//     Note: if only sanity checking (no set up), then derived_broadcast() only
//           as a pre-processor on rank 0.
// (3) send (minimal) db buffer from rank 0 to all other ranks
// (4) define (large-scale) vector defaults across all keywords for all ranks,
//     retaining sanity from 2
//
// Q: is 2 really needed, or would it be OK to perform all checks in 4?
//    Evolution: checks/defaults as part of parsing (original)
//           --> checks after parsing/mixed input and defaults after DB
//               send/receive (current)
//           --> checks/defaults after DB send/receive (proposed new)


void NIDRProblemDescDB::derived_post_process()
{
  // finish processing dataVariableList
  make_variable_defaults(&dataVariablesList);
  // finish processing dataResponsesList
  make_response_defaults(&dataResponsesList);
}


#define Squawk NIDRProblemDescDB::squawk
#define Warn NIDRProblemDescDB::warn

typedef unsigned long UL;

struct Iface_Info {
  DataInterfaceRep *di;
  DataInterface *di_handle;
};

struct Iface_mp_Rlit {
  String DataInterfaceRep::* sp;
  RealVector DataInterfaceRep::* rv;
  const char *lit;
};

struct Iface_mp_ilit {
  String DataInterfaceRep::* sp;
  int DataInterfaceRep::* iv;
  const char *lit;
};

struct Iface_mp_lit {
  String DataInterfaceRep::* sp;
  const char *lit;
};

struct Meth_Info {
  DataMethodRep *dme;
  DataMethod *dme0;
};

struct Method_mp_ilit2 {
  String DataMethodRep::* sp;
  int DataMethodRep::* ip;
  const char *lit;
};

struct Method_mp_ilit2z {
  String DataMethodRep::* sp;
  size_t DataMethodRep::* ip;
  const char *lit;
};

struct Method_mp_lit {
  String DataMethodRep::* sp;
  const char *lit;
};

struct Method_mp_lit2 {
  String DataMethodRep::* sp;
  String DataMethodRep::* sp2;
  const char *lit;
  const char *lit2;
};

struct Method_mp_litc {
  String DataMethodRep::* sp;
  Real DataMethodRep::* rp;
  const char *lit;
};

struct Method_mp_litrv {
  String DataMethodRep::* sp;
  RealVector DataMethodRep::* rp;
  const char *lit;
};

struct Method_mp_slit2 {
  String DataMethodRep::* sp;
  String DataMethodRep::* sp2;
  const char *lit;
};

struct Method_mp_type {
  short DataMethodRep::* sp;
  short type;
};

struct Mod_Info {
  DataModelRep *dmo;
  DataModel *dmo0;
};

struct Model_mp_lit {
  String DataModelRep::* sp;
  const char *lit;
};

struct Model_mp_ord {
  short DataModelRep::* sp;
  int ord;
};

struct Model_mp_type {
  short DataModelRep::* sp;
  short type;
};

struct Resp_Info {
  DataResponsesRep *dr;
  DataResponses *dr0;
};

struct
Resp_mp_lit {
  String DataResponsesRep::* sp;
  const char *lit;
};

struct Strategy_mp_lit {
  String DataStrategyRep::* sp;
  const char *lit;
};

enum { // kinds of continuous aleatory uncertain variables
  CAUVar_normal = 0,
  CAUVar_lognormal = 1,
  CAUVar_uniform = 2,
  CAUVar_loguniform = 3,
  CAUVar_triangular = 4,
  CAUVar_exponential = 5,
  CAUVar_beta = 6,
  CAUVar_gamma = 7,
  CAUVar_gumbel = 8,
  CAUVar_frechet = 9,
  CAUVar_weibull = 10,
  CAUVar_histogram_bin = 11,
  CAUVar_Nkinds = 12	// number of kinds of cauv variables
};

enum { // kinds of discrete aleatory uncertain integer variables
  DAUIVar_poisson = 0,
  DAUIVar_binomial = 1,
  DAUIVar_negative_binomial = 2,
  DAUIVar_geometric = 3,
  DAUIVar_hypergeometric = 4,
  DAUIVar_Nkinds = 5	// number of kinds of dauiv variables
};

enum { // kinds of discrete aleatory uncertain real variables
  DAURVar_histogram_point = 0,
  DAURVar_Nkinds = 1	// number of kinds of daurv variables
};

enum { // kinds of continuous epistemic uncertain variables
  CEUVar_interval = 0,
  CEUVar_Nkinds = 1	// number of kinds of cauv variables
};

enum { // kinds of discrete epistemic uncertain integer variables
  DEUIVar_interval = 0,
  DEUIVar_set_int = 1,
  DEUIVar_Nkinds = 2	// number of kinds of deuiv variables
};

enum { // kinds of discrete epistemic uncertain real variables
  DEURVar_set_real = 0,
  DEURVar_Nkinds = 1	// number of kinds of deurv variables
};

enum { // kinds of discrete set variables
  DiscSetVar_design_set_int = 0,
  DiscSetVar_design_set_real = 1,
  DiscSetVar_state_set_int = 2,
  DiscSetVar_state_set_real = 3,
  DiscSetVar_Nkinds = 4	// number of kinds of discrete set variables
};

struct VarLabel {
  size_t n;
  const char **s;
};

struct Var_Info {
  DataVariablesRep *dv;
  DataVariables    *dv_handle;
  VarLabel  CAUv[ CAUVar_Nkinds],  CEUv[ CEUVar_Nkinds];
  VarLabel DAUIv[DAUIVar_Nkinds], DAURv[DAURVar_Nkinds];
  VarLabel DEUIv[DEUIVar_Nkinds], DEURv[DEURVar_Nkinds];
  IntArray   *nddsi, *nddsr, *nCI, *nDI, *nhbp, *nhpp, *ndusi, *ndusr,
             *ndssi, *ndssr;
  RealVector *ddsr, *CIlb, *CIub, *CIp, *DIp, *DSIp, *DSRp, *dusr,
             *hba, *hbo, *hbc, *hpa, *hpc, *ucm, *dssr;
  IntVector  *ddsi, *DIlb, *DIub, *dusi, *dssi;
};

struct Var_check
{
  const char *name;
  size_t DataVariablesRep::* n;
  void (*vgen)(DataVariablesRep*, size_t);
};

struct Var_rcheck
{
  const char *name;
  size_t DataVariablesRep::* n;
  void (*vgen)(DataVariablesRep*, size_t);
  RealVector  DataVariablesRep::* L;   // when static, initialized to NULL
  RealVector  DataVariablesRep::* U;   // when static, initialized to NULL
  RealVector  DataVariablesRep::* V;   // when static, initialized to NULL
  StringArray DataVariablesRep::* Lbl; // when static, initialized to NULL
};

struct Var_icheck
{
  const char *name;
  size_t DataVariablesRep::* n;
  void (*vgen)(DataVariablesRep*, size_t);
  IntVector   DataVariablesRep::* L;   // when static, initialized to NULL
  IntVector   DataVariablesRep::* U;   // when static, initialized to NULL
  IntVector   DataVariablesRep::* V;   // when static, initialized to NULL
  StringArray DataVariablesRep::* Lbl; // when static, initialized to NULL
};

struct Var_uinfo {
  const char *lbl;
  const char *vkind;
  size_t DataVariablesRep::* n;
  void(*vchk)(DataVariablesRep*, size_t, Var_Info*);
};

struct Var_brv {
  RealVector DataVariablesRep::* rv;
  Real b;
};

struct Var_biv {
  IntVector DataVariablesRep::* iv;
  int b;
};

struct Var_mp_type {
  short DataVariablesRep::* sp;
  short type;
};

void NIDRProblemDescDB::
iface_Rlit(const char *keyname, Values *val, void **g, void *v)
{
  DataInterfaceRep *di = (*(Iface_Info**)g)->di;
  Iface_mp_Rlit *R = (Iface_mp_Rlit*)v;
  Real *r;
  RealVector *rv;
  size_t i, n;

  di->*R->sp = R->lit;
  rv = &(di->*R->rv);
  n = val->n;
  rv->sizeUninitialized(n);
  r = val->r;
  for(i = 0; i < n; ++i)
    (*rv)[i] = r[i];
}

void NIDRProblemDescDB::
iface_false(const char *keyname, Values *val, void **g, void *v)
{
  (*(Iface_Info**)g)->di->**(bool DataInterfaceRep::**)v = false;
}

void NIDRProblemDescDB::
iface_ilit(const char *keyname, Values *val, void **g, void *v)
{
  DataInterfaceRep *di = (*(Iface_Info**)g)->di;
  Iface_mp_ilit *I = (Iface_mp_ilit*)v;

  di->*I->sp = I->lit;
  di->*I->iv = *val->i;
}

void NIDRProblemDescDB::
iface_pint(const char *keyname, Values *val, void **g, void *v)
{
  int n = *val->i;
#ifdef REDUNDANT_INT_CHECKS
  if (n <= 0) /* now handled by INTEGER > 0 in the .nspec file */
    botch("%s must be positive", keyname);
#endif
  (*(Iface_Info**)g)->di->**(int DataInterfaceRep::**)v = n;
}

void NIDRProblemDescDB::
iface_lit(const char *keyname, Values *val, void **g, void *v)
{
  (*(Iface_Info**)g)->di->*((Iface_mp_lit*)v)->sp = ((Iface_mp_lit*)v)->lit;
}

void NIDRProblemDescDB::
iface_start(const char *keyname, Values *val, void **g, void *v)
{
  Iface_Info *ii;

  if (!(ii = new Iface_Info))
  Botch:		botch("new failure in iface_start");
  if (!(ii->di_handle = new DataInterface))
    goto Botch;
  ii->di = ii->di_handle->dataIfaceRep;
  *g = (void*)ii;
}

void NIDRProblemDescDB::
iface_true(const char *keyname, Values *val, void **g, void *v)
{
  (*(Iface_Info**)g)->di->**(bool DataInterfaceRep::**)v = true;
}

/*
 *  not_executable(const char *driver_name) checks whether driver_name is an 
 *  executable file appearing somewhere in $PATH and returns 0 if so,
 *  1 if not found, 2 if found but not executable.
 */
int not_executable(const char *driver_name, const char *tdir)
{
  static const char *p0;
  struct stat sb;
  const char *p;
  const char *p1;
  char *b, buf[2048];
  const char *a2[2], **al;
  int rc, sv;
  size_t clen, dlen, plen, tlen;
  void *a0;
  std::string cwd;  // decalre here before any possible "goto"

#ifdef _WIN32
  char dbuf[128];
#else
  static uid_t myuid;
  static gid_t mygid;
#endif

  /* allow shell assignments and quotes around executable names */
  /* that may involve blanks */
  a2[0] = driver_name;
  a2[1] = 0;
  al = arg_list_adjust(a2,&a0);
  driver_name = al[0];

  rc = 0;
  if (!p0) {
    p0 = std::getenv("PATH");
#ifdef _WIN32
    if (!p0)
      p0 = std::getenv("Path");
#else
    myuid = geteuid();
    mygid = getegid();
#endif
    if (p0)
      while(*p0 <= ' ' && *p0)
	++p0;
    else
      p0 = "";
  }
#ifdef _WIN32
  // make sure we have a suitable suffix
  if ((p = strrchr(driver_name, '.'))) {
    if (std::strlen(++p) != 3)
      p = 0;
    else {
      for(b = dbuf; *p; ++b, ++p)
	*b = tolower(*p);
      *b = 0;
      if (std::strcmp(dbuf, "exe") && std::strcmp(dbuf, "bat") &&
	  std::strcmp(dbuf, "cmd"))
	p = 0;
    }
  }
  if (!p) {
    dlen = std::strlen(driver_name);
    if (dlen + 5 > sizeof(dbuf)) {
      rc = 1;
      goto ret;
    }
    std::strcpy(dbuf, driver_name);
    std::strcpy(dbuf+dlen, ".exe");
    driver_name = dbuf;
  }

  // . is always implicitly in $Path under MS Windows; check it now
  if (!stat(driver_name, &sb))
    goto ret;
#endif

  cwd = get_cwd();
  clen = cwd.size();
  dlen = std::strlen(driver_name);
  tlen = std::strlen(tdir);
  rc = 1;
  p = p0;
  if (std::strchr(driver_name, '/')
#ifdef _WIN32
      || std::strchr(driver_name, '\\')
      || (dlen > 2 && driver_name[1] == ':')
#endif
      )
    p = "";

  else if (clen + dlen + 2 < sizeof(buf)) {
    std::memcpy(buf,cwd.c_str(),clen);
    buf[clen] = '/';
    std::strcpy(buf+clen+1, driver_name);
    sv = stat(buf,&sb);
    if (sv == 0)
      goto stat_check;
  }
  else if (tdir && *tdir && tlen + dlen + 2 < sizeof(buf)) {
    std::memcpy(buf,tdir,tlen);
    buf[tlen] = '/';
    std::strcpy(buf+tlen+1, driver_name);
    sv = stat(buf,&sb);
    if (sv == 0)
      goto stat_check;
  }
  for(;;) {
    for(p1 = p;; ++p1) {
      switch(*p1) {
      case 0:
#ifdef _WIN32
      case ';':
#else
      case ':':
#endif
	goto break2;
      }
    }
  break2:
    if (p1 == p || (p1 == p + 1 && *p == '.'))
      sv = stat(driver_name, &sb);
    else {
      plen = p1 - p;
      while(plen && p[plen-1] <= ' ')
	--plen;
      if (plen + dlen + 2 > sizeof(buf))
	sv = 1;
      else {
	std::strncpy(buf,p,plen);
	b = buf + plen;
	*b++ = '/';
	std::strcpy(b, driver_name);
	sv = stat(buf, &sb);
      }
    }
    if (!sv) {
    stat_check:
#ifdef __CYGWIN__
      rc = 2;
      if (sb.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) {
	rc = 0;
	goto ret;
      }
#elif defined(_WIN32) || defined(_WIN64)
      rc = 0;
      goto ret;
#else
      rc = 2;
      if (sb.st_uid == myuid) {
	if (sb.st_mode & S_IXUSR)
	  goto ret0;
      }
      else if (sb.st_gid == mygid) {
	if (sb.st_mode & S_IXGRP)
	  goto ret0;
      }
      else if (sb.st_mode & S_IXOTH) {
      ret0:                          rc = 0;
	goto ret;
      }
#endif
    }
    if (p1 == 0)
      goto ret;
    else if (!*p1)
      break;
    for(p = p1 + 1; *p <= ' '; ++p)
      while(*p <= ' ' && *p)
	if (!*p)
	  goto ret;
  }
 ret:
  if (a0)
    std::free(a0);
  return rc;
}


void NIDRProblemDescDB::
iface_stop(const char *keyname, Values *val, void **g, void *v)
{
  Iface_Info *ii = *(Iface_Info**)g;
  DataInterfaceRep *di = ii->di;
  StringArray *sa;
  const char *s;
  int ac, ec, j, nd, same;
  void (*Complain)(const char *fmt, ...);
  size_t i, n;
  static const char samefmt[] =
    "template_directory and work_directory name are both \"%s\"";
  static const char samefmt2[] =
    "work_directory template file %d are both \"%s\"";
  struct stat sb1;
#ifndef _WIN32
  struct stat sb2;
#endif

  nd = di->analysisDrivers.size();
  ac = di->asynchLocalAnalysisConcurrency;
  ec = di->asynchLocalEvalConcurrency;

  if (di->algebraicMappings == "" && nd == 0)
    squawk("interface specification must provide algebraic_mappings,\n\t"
	   "analysis_drivers, or both");
  if (nd > 0 && di->asynchLocalAnalysisConcurrency > nd) {
    warn("analysis_concurrency specification greater than length of\n\t"
	 "analysis_drivers list.  Truncating analysis_concurrency to %d",nd);
    di->asynchLocalAnalysisConcurrency = nd;
  }
  if (ec && ec < 2 && ac && ac < 2) {
    warn("asynchronous option not required for evaluation and analysis.\n\t"
	 "Concurrency limited to %d and %d.\n\t"
	 "Synchronous operations will be used", ec, ac);
    di->interfaceSynchronization = "synchronous";
  }
  if ((di->interfaceType == "system" || di->interfaceType == "fork")
      && (Complain = Dak_pddb->parallel_library().command_line_check() ? warn
	  : Dak_pddb->parallel_library().command_line_run() ? squawk : 0)) {
    sa = &di->analysisDrivers;
    n = sa->size();
    if (di->templateDir.c_str())
      WorkdirHelper::prepend_preferred_env_path(di->templateDir.c_str());
#ifdef DEBUG_NOT_EXECUTABLE
    for(i = 0; i < n; ++i)
      if ((j = not_executable(s = (*sa)[i], di->templateDir)))
	Complain("analysis driver \"%s\" %s", s,
		 j == 1	? "not found"
		 : "exists but is not executable");
#endif
  }
  same = 0;
  if (di->workDir.length() > 0) {
    if (di->templateDir.length() > 0) {
      same = di->templateDir == di->workDir;
#ifndef _WIN32
      if (!same
	  && !stat(di->workDir.data(), &sb1)
	  && !stat(di->templateDir.data(), &sb2))
	same = sb1.st_dev == sb2.st_dev
	  && sb1.st_rdev == sb2.st_rdev
	  && sb1.st_ino == sb2.st_ino;
#endif
      if (same)
	squawk(samefmt, di->workDir.data());
    }
    else if ((n = di->templateFiles.size() > 0)
	     && !stat(di->workDir.data(), &sb1)) {
      for(i = 0; i < n; ++i) {
	if (di->templateFiles[i] == di->workDir
#ifndef _WIN32
	    || (!stat(di->templateFiles[i].data(), &sb2)
		&& sb1.st_dev == sb2.st_dev
		&& sb1.st_rdev == sb2.st_rdev
		&& sb1.st_ino == sb2.st_ino)
#endif
	    ) {
	  squawk(samefmt2, (int)(i+1), di->workDir.data());
	  break;
	}
      }
    }
  }
  pDDBInstance->dataInterfaceList.push_back(*ii->di_handle);
  delete ii->di_handle;
  delete ii;
}

void NIDRProblemDescDB::
iface_str(const char *keyname, Values *val, void **g, void *v)
{
  (*(Iface_Info**)g)->di->**(String DataInterfaceRep::**)v = *val->s;
}

void NIDRProblemDescDB::
iface_str2D(const char *keyname, Values *val, void **g, void *v)
{
  DataInterfaceRep *di = (*(Iface_Info**)g)->di;
  String2DArray *sa2 = &(di->**(String2DArray DataInterfaceRep::**)v);
  StringArray *sa;
  const char **s = val->s;
  size_t i, j, k, n, nc, nd;

  // This is for analysisComponents -- only String2DArray in a DataInterfaceRep

  nd = di->analysisDrivers.size();
  n = val->n;
  if (nd <= 0)
    botch("num_drivers = %d in iface_str2D", (int)nd);
  if (n % nd) {
    squawk("number of analysis_components not evenly divisible "
	   "by number of analysis_drivers");
    return;
  }
  nc = n / nd;
  sa2->resize(nd);
  for(i = k = 0; i < nd; i++) {
    sa = &((*sa2)[i]);
    sa->resize(nc);
    for(j = 0; j < nc; ++j, ++k)
      (*sa)[j] = s[k];
  }
}

void NIDRProblemDescDB::
iface_strL(const char *keyname, Values *val, void **g, void *v)
{
  StringArray *sa
    = &((*(Iface_Info**)g)->di->**(StringArray DataInterfaceRep::**)v);
  const char **s = val->s;
  size_t i, n = val->n;

  sa->resize(n);
  for(i = 0; i < n; i++)
    (*sa)[i] = s[i];
}

void NIDRProblemDescDB::
method_Real(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->**(Real DataMethodRep::**)v = *val->r;
}

void NIDRProblemDescDB::
method_Real01(const char *keyname, Values *val, void **g, void *v)
{
  Real t = *val->r;
  if (t < 0. || t > 1.)
    botch("%s must be in [0, 1]", keyname);
  (*(Meth_Info**)g)->dme->**(Real DataMethodRep::**)v = t;
}

void NIDRProblemDescDB::
method_RealDL(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  RealVector DataMethodRep::* sa = *(RealVector DataMethodRep::**)v;
  Real *r = val->r;
  size_t i, n = val->n;

  (dm->*sa).sizeUninitialized(n);
  for(i = 0; i < n; i++)
    (dm->*sa)[i] = r[i];
}

void NIDRProblemDescDB::
method_RealLlit(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  Real *r;
  RealVector *rv = &(dm->*((Method_mp_litrv*)v)->rp);
  size_t i, n;
  dm->*((Method_mp_litrv*)v)->sp = ((Method_mp_litrv*)v)->lit;
  r = val->r;
  n = val->n;
  rv->resize(n);
  for(i = 0; i < n; i++)
    (*rv)[i] = r[i];
}

void NIDRProblemDescDB::
method_Realp(const char *keyname, Values *val, void **g, void *v)
{
  Real t = *val->r;
  if (t <= 0.)
    botch("%s must be positive", keyname);
  (*(Meth_Info**)g)->dme->**(Real DataMethodRep::**)v = t;
}

void NIDRProblemDescDB::
method_Realz(const char *keyname, Values *val, void **g, void *v)
{
  Real t = *val->r;
  if (t < 0.)
    botch("%s must be nonnegative", keyname);
  (*(Meth_Info**)g)->dme->**(Real DataMethodRep::**)v = t;
}

// MSE: This function just sets two values for one keyword.
void NIDRProblemDescDB::
method_piecewise(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  dm->expansionType  = STD_UNIFORM_U;
  dm->piecewiseBasis = true;
}

void NIDRProblemDescDB::
method_false(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->**(bool DataMethodRep::**)v = false;
}

void NIDRProblemDescDB::
method_int(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->**(int DataMethodRep::**)v = *val->i;
}

void NIDRProblemDescDB::
method_ivec(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  int *z = val->i;
  IntVector *iv = &(dm->**(IntVector DataMethodRep::**)v);
  size_t i, n = val->n;

  iv->resize(n);
  for(i = 0; i < n; i++)
    (*iv)[i] = z[i];
}

void NIDRProblemDescDB::
method_ilit2(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  dm->*((Method_mp_ilit2*)v)->sp = ((Method_mp_ilit2*)v)->lit;
  dm->*((Method_mp_ilit2*)v)->ip = *val->i;
}

void NIDRProblemDescDB::
method_ilit2p(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  dm->*((Method_mp_ilit2z*)v)->sp = ((Method_mp_ilit2z*)v)->lit;
  if ((dm->*((Method_mp_ilit2z*)v)->ip = *val->i) <= 0)
    botch("%s must be positive", keyname);
}

void NIDRProblemDescDB::
method_lit(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->*((Method_mp_lit*)v)->sp = ((Method_mp_lit*)v)->lit;
}

void NIDRProblemDescDB::
method_lit2(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  dm->*((Method_mp_lit2*)v)->sp  = ((Method_mp_lit2*)v)->lit;
  dm->*((Method_mp_lit2*)v)->sp2 = ((Method_mp_lit2*)v)->lit2;
}

void NIDRProblemDescDB::
method_litc(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  if (dm->*((Method_mp_litc*)v)->rp > 0.)
    dm->*((Method_mp_litc*)v)->sp = ((Method_mp_litc*)v)->lit;
}

void NIDRProblemDescDB::
method_litp(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  Real t = *val->r;
  if (t <= 0.)
    botch("%s must be positive",keyname);
  dm->*((Method_mp_litc*)v)->sp = ((Method_mp_litc*)v)->lit;
  dm->*((Method_mp_litc*)v)->rp = t;
}

void NIDRProblemDescDB::
method_litz(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  Real t = *val->r;
  if (t < 0.)
    botch("%s must be nonnegative",keyname);
  if ((dm->*((Method_mp_litc*)v)->rp = t) == 0.)
    dm->*((Method_mp_litc*)v)->sp = ((Method_mp_litc*)v)->lit;
}

void NIDRProblemDescDB::
method_nnint(const char *keyname, Values *val, void **g, void *v)
{
  int n = *val->i;
#ifdef REDUNDANT_INT_CHECKS
  if (n < 0) /* now handled by INTEGER >= 0 in the .nspec file */
    botch("%s must be non-negative", keyname);
#endif
  (*(Meth_Info**)g)->dme->**(int DataMethodRep::**)v = n;
}

void NIDRProblemDescDB::
method_sizet(const char *keyname, Values *val, void **g, void *v)
{
  int n = *val->i; // test value as int, prior to storage as size_t
#ifdef REDUNDANT_INT_CHECKS
  if (n < 0) /* now handled by INTEGER >= 0 in the .nspec file */
    botch("%s must be non-negative", keyname);
#endif
  (*(Meth_Info**)g)->dme->**(size_t DataMethodRep::**)v = n;
}

void NIDRProblemDescDB::
method_num_resplevs(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  Real *r;
  RealVectorArray *rva = &(dm->**(RealVectorArray DataMethodRep::**)v);
  RealVector *ra = &(*rva)[0];
  int m, *z = val->i;
  size_t i, i1, j, je, k, n;

  n = val->n;
  for(i = k = 0; i < n; ++i)
    k += z[i];
  if (ra->length() != (int)k)
    botch("number of %s = %u does not match\n"
	  "%s specification of %u response levels",
	  keyname+4, (Uint)ra->length(), keyname, (Uint)k);
  r = new Real[k];
  for(i = 0; i < k; i++)
    r[i] = (*ra)[i];
  (*rva).resize(n);
  for(i = j = je = 0; i < n; i++) {
    m = z[i];
    (*rva)[i].resize(m);
    ra = &(*rva)[i];
    for(i1 = 0, je += m; j < je; ++i1, ++j)
      (*ra)[i1] = r[j];
  }
  delete[] r;
}

void NIDRProblemDescDB::
method_pint(const char *keyname, Values *val, void **g, void *v)
{
  int n = *val->i;
#ifdef REDUNDANT_INT_CHECKS
  if (n <= 0) /* now handled by INTEGER > 0 in the .nspec file */
    botch("%s must be positive", keyname);
#endif
  (*(Meth_Info**)g)->dme->**(int DataMethodRep::**)v = n;
}

void NIDRProblemDescDB::
method_pintz(const char *keyname, Values *val, void **g, void *v)
{
  int n = *val->i; // test value as int, prior to storage as size_t
#ifdef REDUNDANT_INT_CHECKS
  if (n <= 0) /* now handled by INTEGER > 0 in the .nspec file */
    botch("%s must be positive", keyname);
#endif
  (*(Meth_Info**)g)->dme->**(size_t DataMethodRep::**)v = n;
}

void NIDRProblemDescDB::
method_resplevs(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  Real *r = val->r;
  RealVector *ra;
  RealVectorArray *rva = &(dm->**(RealVectorArray DataMethodRep::**)v);
  size_t i, n = val->n;

  (*rva).resize(1);
  ra = &(*rva)[0];
  ra->resize(n);
  for(i = 0; i < n; ++i)
    (*ra)[i] = r[i];
}

void NIDRProblemDescDB::
method_resplevs01(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;
  Real *r = val->r, t;
  RealVector *ra;
  RealVectorArray *rva = &(dm->**(RealVectorArray DataMethodRep::**)v);
  size_t i, n = val->n;

  (*rva).resize(1);
  ra = &(*rva)[0];
  ra->resize(n);
  for(i = 0; i < n; ++i) {
    (*ra)[i] = t = r[i];
    if (t < 0. || t > 1.)
      botch("%s must be between 0 and 1", keyname);
  }
}

void NIDRProblemDescDB::
method_shint(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->**(short DataMethodRep::**)v = *val->i;
}

void NIDRProblemDescDB::
method_ushint(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->**(unsigned short DataMethodRep::**)v = *val->i;
}

void NIDRProblemDescDB::
method_usharray(const char *keyname, Values *val, void **g, void *v)
{
  UShortArray *usa
    = &((*(Meth_Info**)g)->dme->**(UShortArray DataMethodRep::**)v);
  int *z = val->i;
  size_t i, n = val->n;

  usa->resize(n);
  for (i=0; i<n; ++i)
    if (z[i] >= 0)
      (*usa)[i] = z[i];
    else
      botch("%s must have non-negative values", keyname);
}

void NIDRProblemDescDB::
method_szarray(const char *keyname, Values *val, void **g, void *v)
{
  SizetArray *sza
    = &((*(Meth_Info**)g)->dme->**(SizetArray DataMethodRep::**)v);
  int *z = val->i;
  size_t i, n = val->n;

  sza->resize(n);
  for (i=0; i<n; ++i)
    if (z[i] >= 0)
      (*sza)[i] = z[i];
    else
      botch("%s must have non-negative values", keyname);
}

void NIDRProblemDescDB::
method_slit2(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep *dm = (*(Meth_Info**)g)->dme;

  dm->*((Method_mp_slit2*)v)->sp  = ((Method_mp_slit2*)v)->lit;
  dm->*((Method_mp_slit2*)v)->sp2 = *val->s;
}

void NIDRProblemDescDB::
method_start(const char *keyname, Values *val, void **g, void *v)
{
  Meth_Info *mi;

  if (!(mi = new Meth_Info))
  Botch:		botch("new failure in method_start");
  if (!(mi->dme0 = new DataMethod))
    goto Botch;
  mi->dme = mi->dme0->dataMethodRep;
  *g = (void*)mi;
}

static void
scale_chk(StringArray &ST, RealVector &S, const char *what, const char **univ)
{
  const char *s, **u;
  size_t i, n, nbad, vseen;

  n = ST.size();
  for(i = nbad = vseen = 0; i < n; ++i) {
    s = ST[i].data();
    if (!strcmp(s,"value")) {
      ++vseen;
      goto break2;
    }
    for(u = univ; *u; ++u)
      if (!strcmp(s,*u)) {
	goto break2;
      }
    NIDRProblemDescDB::squawk("\"%s\" cannot appear in %s_scale_types",
			      s, what);
    ++nbad;
  break2:	;
  }
  if (vseen && S.length() <= 0)
    NIDRProblemDescDB::squawk(
      "\"value\" in %s_scale_types requires at least one value for %s_scales",
      what, what);
}

static const char *aln_scaletypes[] = { "auto", "log", "none", 0 };

void NIDRProblemDescDB::
method_stop(const char *keyname, Values *val, void **g, void *v)
{
  static const char *mr_scaletypes[] = { "auto", "none", 0 };
  Meth_Info *mi = *(Meth_Info**)g;
  DataMethodRep *dm = mi->dme;
  scale_chk(dm->linearIneqScaleTypes, dm->linearIneqScales,
	    "linear_inequality", mr_scaletypes);
  scale_chk(dm->linearEqScaleTypes, dm->linearEqScales,
	    "linear_equality", mr_scaletypes);
  pDDBInstance->dataMethodList.push_back(*mi->dme0);
  delete mi->dme0;
  delete mi;
}

void NIDRProblemDescDB::
method_str(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->**(String DataMethodRep::**)v = *val->s;
}

void NIDRProblemDescDB::
method_strL(const char *keyname, Values *val, void **g, void *v)
{
  StringArray *sa = &((*(Meth_Info**)g)->dme->**(StringArray DataMethodRep::**)v);
  const char **s = val->s;
  size_t i, n = val->n;

  sa->resize(n);
  for(i = 0; i < n; i++)
    (*sa)[i] = s[i];
}

void NIDRProblemDescDB::
method_true(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->**(bool DataMethodRep::**)v = true;
}

void NIDRProblemDescDB::
method_type(const char *keyname, Values *val, void **g, void *v)
{
  (*(Meth_Info**)g)->dme->*((Method_mp_type*)v)->sp = ((Method_mp_type*)v)->type;
}

void NIDRProblemDescDB::
model_Real(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->**(Real DataModelRep::**)v = *val->r;
}

void NIDRProblemDescDB::
model_RealDL(const char *keyname, Values *val, void **g, void *v)
{
  Real *r = val->r;
  RealVector *rdv = &((*(Mod_Info**)g)->dmo->**(RealVector DataModelRep::**)v);
  size_t i, n = val->n;

  rdv->sizeUninitialized(n);
  for(i = 0; i < n; i++)
    (*rdv)[i] = r[i];
}

void NIDRProblemDescDB::
model_false(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->**(bool DataModelRep::**)v = false;
}

void NIDRProblemDescDB::
model_int(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->**(int DataModelRep::**)v = *val->i;
}

void NIDRProblemDescDB::
model_intsetm1(const char *keyname, Values *val, void **g, void *v)
{
  IntSet *is = &((*(Mod_Info**)g)->dmo->**(IntSet DataModelRep::**)v);
  int *z = val->i;
  size_t i, n = val->n;

  for(i = 0; i < n; i++)
    is->insert(z[i] - 1); // model converts ids -> indices
}

void NIDRProblemDescDB::
model_lit(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->*((Model_mp_lit*)v)->sp = ((Model_mp_lit*)v)->lit;
}

void NIDRProblemDescDB::
model_order(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->*((Model_mp_ord*)v)->sp = ((Model_mp_ord*)v)->ord;
}

void NIDRProblemDescDB::
model_type(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->*((Model_mp_type*)v)->sp = ((Model_mp_type*)v)->type;
}

void NIDRProblemDescDB::
model_shint(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->**(short DataModelRep::**)v = (short)*val->i;
}

void NIDRProblemDescDB::
model_start(const char *keyname, Values *val, void **g, void *v)
{
  DataModelRep *dm;
  Mod_Info *mi;

  if (!(mi = new Mod_Info))
  Botch:		botch("new failure in model_start");
  if (!(mi->dmo0 = new DataModel))
    goto Botch;
  dm = mi->dmo = mi->dmo0->dataModelRep;
  *g = (void*)mi;
}

void NIDRProblemDescDB::
model_stop(const char *keyname, Values *val, void **g, void *v)
{
  Mod_Info *mi = *(Mod_Info**)g;
  pDDBInstance->dataModelList.push_back(*mi->dmo0);
  delete mi->dmo0;
  delete mi;
}

void NIDRProblemDescDB::
model_str(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->**(String DataModelRep::**)v = *val->s;
}

void NIDRProblemDescDB::
model_strL(const char *keyname, Values *val, void **g, void *v)
{
  DataModelRep *dm = (*(Mod_Info**)g)->dmo;
  StringArray DataModelRep::* sa = *(StringArray DataModelRep::**)v;
  const char **s = val->s;
  size_t i, n = val->n;

  (dm->*sa).resize(n);
  for(i = 0; i < n; i++)
    (dm->*sa)[i] = s[i];
}

void NIDRProblemDescDB::
model_true(const char *keyname, Values *val, void **g, void *v)
{
  (*(Mod_Info**)g)->dmo->**(bool DataModelRep::**)v = true;
}

void NIDRProblemDescDB::
resp_RealDL(const char *keyname, Values *val, void **g, void *v)
{
  Real *r = val->r;
  RealVector *rv = &((*(Resp_Info**)g)->dr->**(RealVector DataResponsesRep::**)v);
  size_t i, n = val->n;

  rv->sizeUninitialized(n);
  for(i = 0; i < n; i++)
    (*rv)[i] = r[i];
}

void NIDRProblemDescDB::
resp_RealL(const char *keyname, Values *val, void **g, void *v)
{
  NIDRProblemDescDB::resp_RealDL(keyname, val, g, v);
}

void NIDRProblemDescDB::
resp_intset(const char *keyname, Values *val, void **g, void *v)
{
  IntSet *is = &((*(Resp_Info**)g)->dr->**(IntSet DataResponsesRep::**)v);
  int *z = val->i;
  size_t i, n = val->n;

  for (i=0; i<n; ++i)
    is->insert(z[i]);
}

void NIDRProblemDescDB::
resp_ivec(const char *keyname, Values *val, void **g, void *v)
{
  DataResponsesRep *dr = (*(Resp_Info**)g)->dr;
  IntVector *iv = &(dr->**(IntVector DataResponsesRep::**)v);
  size_t i, n = val->n;
  iv->sizeUninitialized(n);

  int *z = val->i;
  for(i = 0; i < n; i++)
    (*iv)[i] = z[i];
}

void NIDRProblemDescDB::
resp_lit(const char *keyname, Values *val, void **g, void *v)
{
  (*(Resp_Info**)g)->dr->*((Resp_mp_lit*)v)->sp = ((Resp_mp_lit*)v)->lit;
}

void NIDRProblemDescDB::
resp_sizet(const char *keyname, Values *val, void **g, void *v)
{
  int n = *val->i; // test value as int, prior to storage as size_t
#ifdef REDUNDANT_INT_CHECKS
  if (n < 0) /* now handled by INTEGER >= 0 in the .nspec file */
    botch("%s must be non-negative", keyname);
#endif
  (*(Resp_Info**)g)->dr->**(size_t DataResponsesRep::**)v = n;
}

void NIDRProblemDescDB::
resp_start(const char *keyname, Values *val, void **g, void *v)
{
  Resp_Info *ri;

  if (!(ri = new Resp_Info))
  Botch:		botch("new failure in resp_start");
  if (!(ri->dr0 = new DataResponses))
    goto Botch;
  ri->dr = ri->dr0->dataRespRep;
  *g = (void*)ri;
}

static void
BuildLabels(StringArray *sa, size_t nsa, size_t n1, size_t n2, const char *stub)
{
  char buf[64];
  size_t i, n0;
  if (nsa)
    sa->resize(nsa);
  i = n0 = n1;
  while(n1 < n2) {
    std::sprintf(buf, "%s%lu", stub, (UL)(++n1 - n0));
    (*sa)[i++] = buf;
  }
}

static int mixed_check(IntSet *S, int n, IntArray *iv, const char *what)
{
  int nbad, j;

  nbad = 0;
  for(ISCIter it = S->begin(), ite = S->end(); it != ite; ++it) {
    j = *it;
    if (j < 1 || j > n) {
      if (!nbad++)
	Squawk("%s values must be between 1 and %d", what, n);
    }
    else {
      ++(iv->operator[](j-1));
    }
  }
  return nbad;
}

static void
mixed_check2(size_t n, IntArray *iv, const char *what)
{
  int j;
  size_t i;

  for(i = 0; i < n; ) {
    j = (*iv)[i++];
    if (j == 0)
      Squawk("Function %lu missing from mixed %s lists", (UL)i);
    else if (j > 1)
      Squawk("Function %lu replicated in mixed %s lists", (UL)i);
  }
}

void NIDRProblemDescDB::
resp_stop(const char *keyname, Values *val, void **g, void *v)
{
  size_t k, n;
  static const char *osc[] = { "log", "none", 0 };
  Resp_Info *ri = *(Resp_Info**)g;
  DataResponsesRep *dr = ri->dr;
  scale_chk(dr->primaryRespFnScaleTypes, dr->primaryRespFnScales,
	    dr->numLeastSqTerms ? "least_squares_term" : "objective_function", osc);
  scale_chk(dr->nonlinearIneqScaleTypes, dr->nonlinearIneqScales,
	    "nonlinear_inequality", aln_scaletypes);
  scale_chk(dr->nonlinearEqScaleTypes, dr->nonlinearEqScales,
	    "nonlinear_equality", aln_scaletypes);
  if ( dr->primaryRespFnWeights.length() > 0 && 
       (dr->expStdDeviations.length() > 0 || dr->numExpStdDeviations > 0) ) {
    squawk("Specify calibration weights or experimental standard deviations, not both.");
  }
  if ((n = dr->responseLabels.size()) > 0) {
    if (!(k = dr->numResponseFunctions)) {
      if (!(k = dr->numObjectiveFunctions))
	k = dr->numLeastSqTerms;
      k += dr->numNonlinearIneqConstraints + dr->numNonlinearEqConstraints;
    }
    if (n != k)
      squawk("Expected %ld response descriptors but found %ld",
	     (long)k, (long) n);
  }
  pDDBInstance->dataResponsesList.push_back(*ri->dr0);
  delete ri->dr0;
  delete ri;
}


void NIDRProblemDescDB::
check_responses(std::list<DataResponses>* drl)
{
  // TO DO: move Schk from below?
}


void NIDRProblemDescDB::
make_response_defaults(std::list<DataResponses>* drl)
{
  IntArray *iv;
  StringArray *rl;
  int ni;
  size_t i, n, n1, nf, no, nrl;
  struct RespStr_chk {
    const char *what;
    size_t DataResponsesRep::* n;
    StringArray DataResponsesRep::* sa;
  } *sc;
  struct RespDVec_chk {
    const char *what;
    size_t DataResponsesRep::* n;
    RealVector DataResponsesRep::* rv;
  } *rdvc;
#define Schk(a,b,c) {#a,&DataResponsesRep::b, &DataResponsesRep::c}
  static RespStr_chk Str_chk[] = {  // for StringArray checking
    Schk(least_squares_term_scale_types,numLeastSqTerms,primaryRespFnScaleTypes),
    Schk(nonlinear_equality_scale_types,numNonlinearEqConstraints,nonlinearEqScaleTypes),
    Schk(nonlinear_inequality_scale_types,numNonlinearIneqConstraints,nonlinearIneqScaleTypes),
    Schk(objective_function_scale_types,numObjectiveFunctions,primaryRespFnScaleTypes)
  };
  static RespDVec_chk RespVec_chk_Bound[] = {// Bounds:  length must be right
    Schk(least_squares_weights,numLeastSqTerms,primaryRespFnWeights),
    Schk(multi_objective_weights,numObjectiveFunctions,primaryRespFnWeights),
    Schk(nonlinear_equality_targets,numNonlinearEqConstraints,nonlinearEqTargets),
    Schk(nonlinear_inequality_lower_bounds,numNonlinearIneqConstraints,nonlinearIneqLowerBnds),
    Schk(nonlinear_inequality_upper_bounds,numNonlinearIneqConstraints,nonlinearIneqUpperBnds)
  };
  static RespDVec_chk RespVec_chk_Scale[] = {// Scales:  length must be right
    Schk(least_squares_term_scales,numLeastSqTerms,primaryRespFnScales),
    Schk(nonlinear_equality_scales,numNonlinearEqConstraints,nonlinearEqScales),
    Schk(nonlinear_inequality_scales,numNonlinearIneqConstraints,nonlinearIneqScales),
    Schk(objective_function_scales,numObjectiveFunctions,primaryRespFnScales)
  };
#undef Schk
#define Numberof(x) sizeof(x)/sizeof(x[0])

  std::list<DataResponses>::iterator It = drl->begin(), Ite = drl->end();
  for(; It != Ite; It++) {

    DataResponsesRep *dr = It->dataRespRep;

    for(sc = Str_chk, i = 0; i < Numberof(Str_chk); ++sc, ++i)
      if ((n1 = dr->*sc->n) && (n = (dr->*sc->sa).size()) > 0
	  && n != n1 && n != 1)
	squawk("%s must have length 1 or %lu, not %lu",
	       sc->what, (UL)n1, (UL)n);
    rl = &dr->responseLabels;
    nrl = rl->size();
    no = dr->numObjectiveFunctions;
    if ((n = dr->numLeastSqTerms)) {
      nf = n + dr->numNonlinearEqConstraints + dr->numNonlinearIneqConstraints;
      if (!nrl) {
	BuildLabels(rl, nf, 0, n, "least_sq_term_");
	n1 = n + dr->numNonlinearIneqConstraints;
	BuildLabels(rl, 0, n, n1, "nln_ineq_con_");
	BuildLabels(rl, 0, n1, nf, "nln_eq_con_");
      }
    }
    else if ((nf = no + dr->numNonlinearEqConstraints + dr->numNonlinearIneqConstraints)) {
      if (!nrl) {
	rl->resize(nf);
	if (no == 1)
	  (*rl)[0] = "obj_fn";
	else
	  BuildLabels(rl, 0, 0, no, "obj_fn_");
	n1 = no + dr->numNonlinearIneqConstraints;
	BuildLabels(rl, 0, no, n1, "nln_ineq_con_");
	BuildLabels(rl, 0, n1, nf, "nln_eq_con_");
      }
    }
    else if ((nf = dr->numResponseFunctions)) {
      if (!nrl)
	BuildLabels(rl, nf, 0, nf, "response_fn_");
    }
    for(rdvc = RespVec_chk_Bound, i = 0; i < Numberof(RespVec_chk_Bound); ++rdvc, ++i)
      if ((n1 = dr->*rdvc->n) && (n = (dr->*rdvc->rv).length()) && n != n1)
	squawk("%s needs %lu elements, not %lu",
	       rdvc->what, (UL)(dr->*rdvc->n), (UL)n);
    for(rdvc = RespVec_chk_Scale, i = 0; i < Numberof(RespVec_chk_Scale); ++rdvc, ++i)
      if ((n1 = dr->*rdvc->n) && (n = (dr->*rdvc->rv).length())
	  && n != n1 && n != 1)
	squawk("%s needs %lu elements (or just one), not %lu",
	       rdvc->what, (UL)n1, (UL)n);
    if (dr->methodSource == "vendor" && dr->fdGradStepSize.length() > 1)
      squawk("vendor numerical gradients only support a single fd_gradient_step_size");

    ni = (int)nf;
    if (dr->gradientType == "mixed") {
      iv = new IntArray;
      //iv->resize(nf);
      iv->assign(nf, 0);
      if (!(mixed_check(&dr->idAnalyticGrads, ni, iv, "id_analytic_gradients")
	    + mixed_check(&dr->idNumericalGrads, ni, iv, "id_numerical_gradients")))
	mixed_check2(nf, iv, "gradient");
      delete iv;
    }
    if ((n = dr->numNonlinearEqConstraints) > 0
	&& dr->nonlinearEqTargets.length() == 0) {
      dr->nonlinearEqTargets.sizeUninitialized(n);
      dr->nonlinearEqTargets = 0.;
    }
    if ((n = dr->numNonlinearIneqConstraints) > 0) {
      if (dr->nonlinearIneqLowerBnds.length() == 0) {
	dr->nonlinearIneqLowerBnds.sizeUninitialized(n);
	dr->nonlinearIneqLowerBnds = -DBL_MAX;
      }
      if (dr->nonlinearIneqUpperBnds.length() == 0) {
	dr->nonlinearIneqUpperBnds.sizeUninitialized(n);
	dr->nonlinearIneqUpperBnds = 0.;
	// default is a one-sided inequality <= 0.0
      }
    }
    if (dr->hessianType == "mixed") {
      iv = new IntArray;
      //iv->resize(nf);
      iv->assign(nf, 0);
      if (!(mixed_check(&dr->idAnalyticHessians, ni, iv, "id_analytic_hessians")
	    + mixed_check(&dr->idNumericalHessians, ni, iv, "id_numerical_hessians")
	    + mixed_check(&dr->idQuasiHessians, ni, iv, "id_quasi_hessians")))
	mixed_check2(nf, iv, "Hessian");
      delete iv;
    }
    if (nerr)
      abort_handler(-1);
  }
}

void NIDRProblemDescDB::
resp_str(const char *keyname, Values *val, void **g, void *v)
{
  (*(Resp_Info**)g)->dr->**(String DataResponsesRep::**)v = *val->s;
}

void NIDRProblemDescDB::
resp_strL(const char *keyname, Values *val, void **g, void *v)
{
  StringArray *sa
    = &((*(Resp_Info**)g)->dr->**(StringArray DataResponsesRep::**)v);
  const char **s = val->s;
  size_t i, n = val->n;

  sa->resize(n);
  for(i = 0; i < n; i++)
    (*sa)[i] = s[i];
}

void NIDRProblemDescDB::
resp_false(const char *keyname, Values *val, void **g, void *v)
{
  (*(Resp_Info**)g)->dr->**(bool DataResponsesRep::**)v = false;
}

void NIDRProblemDescDB::
resp_true(const char *keyname, Values *val, void **g, void *v)
{
  (*(Resp_Info**)g)->dr->**(bool DataResponsesRep::**)v = true;
}

void NIDRProblemDescDB::
strategy_Real(const char *keyname, Values *val, void **g, void *v)
{
  (*(DataStrategyRep**)g)->**(Real DataStrategyRep::**)v = *val->r;
}

void NIDRProblemDescDB::
strategy_RealL(const char *keyname, Values *val, void **g, void *v)
{
  Real *r = val->r;
  RealVector *rdv
    = &((*(DataStrategyRep**)g)->**(RealVector DataStrategyRep::**)v);
  size_t i, n = val->n;

  rdv->sizeUninitialized(n);
  for(i = 0; i < n; i++)
    (*rdv)[i] = r[i];
}

void NIDRProblemDescDB::
strategy_true(const char *keyname, Values *val, void **g, void *v)
{
  (*(DataStrategyRep**)g)->**(bool DataStrategyRep::**)v = true;
}

void NIDRProblemDescDB::
strategy_int(const char *keyname, Values *val, void **g, void *v)
{
  (*(DataStrategyRep**)g)->**(int DataStrategyRep::**)v = *val->i;
}

void NIDRProblemDescDB::
strategy_lit(const char *keyname, Values *val, void **g, void *v)
{
  (*(DataStrategyRep**)g)->*((Strategy_mp_lit*)v)->sp
    = ((Strategy_mp_lit*)v)->lit;
}

void NIDRProblemDescDB::
strategy_start(const char *keyname, Values *val, void **g, void *v)
{
  *g = (void*)pDDBInstance->strategySpec.dataStratRep;
}

void NIDRProblemDescDB::
strategy_str(const char *keyname, Values *val, void **g, void *v)
{
  (*(DataStrategyRep**)g)->**(String DataStrategyRep::**)v = *val->s;
}

void NIDRProblemDescDB::
strategy_strL(const char *keyname, Values *val, void **g, void *v)
{
  StringArray *sa
    = &((*(DataStrategyRep**)g)->**(StringArray DataStrategyRep::**)v);
  const char **s = val->s;
  size_t i, n = val->n;

  sa->resize(n);
  for(i = 0; i < n; i++)
    (*sa)[i] = s[i];
}

void NIDRProblemDescDB::
method_tr_final(const char *keyname, Values *val, void **g, void *v)
{
  DataMethodRep &data_method = *(*(Meth_Info**)g)->dme;

  // sanity checks on trust region user-defined values
  if ( data_method.surrBasedLocalTRInitSize <= 0.0 ||
       data_method.surrBasedLocalTRInitSize >  1.0 )
    botch("initial_size must be in (0,1]");
  if ( data_method.surrBasedLocalTRMinSize <= 0.0 ||
       data_method.surrBasedLocalTRMinSize >  1.0 ||
       data_method.surrBasedLocalTRMinSize >
       data_method.surrBasedLocalTRInitSize )
    botch("minimum_size must be in (0,1]");
  if( data_method.surrBasedLocalTRContractTrigger <= 0.0 ||
      data_method.surrBasedLocalTRContractTrigger >
      data_method.surrBasedLocalTRExpandTrigger        ||
      data_method.surrBasedLocalTRExpandTrigger   >  1.0 )
    botch("expand/contract threshold values must satisfy\n\t"
	  "0 < contract_threshold <= expand_threshold < 1");
  if ( data_method.surrBasedLocalTRContract == 1.0 )
    warn("contraction_factor = 1.0 is valid, but should be < 1\n\t"
	 "to assure convergence of the surrrogate_based_opt method");
  if ( data_method.surrBasedLocalTRContract <= 0.0 ||
       data_method.surrBasedLocalTRContract >  1.0 )
    botch("contraction_factor must be in (0,1]");
  if (data_method.surrBasedLocalTRExpand < 1.0)
    botch("expansion_factor must be >= 1");
}


void NIDRProblemDescDB::
var_RealLb(const char *keyname, Values *val, void **g, void *v)
{
  Real b, *r;
  RealVector *rv;
  Var_brv *V;
  size_t i, n;

  V = (Var_brv*)v;
  rv = &((*(Var_Info**)g)->dv->*V->rv);
  b = V->b;
  r = val->r;
  n = val->n;
  for(i = 0; i < n; i++)
    if (r[i] <= b) {
      squawk("%s values must be > %g", keyname, b);
      break;
    }
  rv->sizeUninitialized(n);
  for(i = 0; i < n; i++)
    (*rv)[i] = r[i];
}

void NIDRProblemDescDB::
var_RealUb(const char *keyname, Values *val, void **g, void *v)
{
  Real b, *r;
  RealVector *rv;
  Var_brv *V;
  size_t i, n;

  V = (Var_brv*)v;
  rv = &((*(Var_Info**)g)->dv->*V->rv);
  b = V->b;
  r = val->r;
  n = val->n;
  for(i = 0; i < n; i++)
    if (r[i] >= b) {
      squawk("%s values must be < %g", keyname, b);
      break;
    }
  rv->sizeUninitialized(n);
  for(i = 0; i < n; i++)
    (*rv)[i] = r[i];
}

void NIDRProblemDescDB::
var_IntLb(const char *keyname, Values *val, void **g, void *v)
{
  Var_biv *V = (Var_biv*)v;
  IntVector *iv = &((*(Var_Info**)g)->dv->*V->iv);
  int b = V->b;
  int *z = val->i;
  size_t i, n = val->n;
  for(i = 0; i < n; i++)
    if (z[i] <= b) {
      squawk("%s values must be > %g", keyname, b);
      break;
    }
  iv->sizeUninitialized(n);
  for(i = 0; i < n; i++)
    (*iv)[i] = z[i];
}

void NIDRProblemDescDB::
var_newrvec(const char *keyname, Values *val, void **g, void *v)
{
  Var_Info *vi = *(Var_Info**)g;
  RealVector *rv;
  size_t i, n = val->n;
  Real *r = val->r;

  if (!(rv = new RealVector(n, false)))
    botch("new failure in var_newrvec");
  vi->**(RealVector *Var_Info::**)v = rv;
  for(i = 0; i < n; i++)
    (*rv)[i] = r[i];
}

void NIDRProblemDescDB::
var_newivec(const char *keyname, Values *val, void **g, void *v)
{
  Var_Info *vi = *(Var_Info**)g;
  IntVector *iv;
  size_t i, n = val->n;
  int *z = val->i;

  if (!(iv = new IntVector(n, false)))
    botch("new failure in var_newivec");
  vi->**(IntVector *Var_Info::**)v = iv;
  for(i = 0; i < n; i++)
    (*iv)[i] = z[i];
}

void NIDRProblemDescDB::
var_newiarray(const char *keyname, Values *val, void **g, void *v)
{
  Var_Info *vi = *(Var_Info**)g;
  IntArray *iv;
  size_t i, n = val->n;
  int *z = val->i;

  if (!(iv = new IntArray(n)))
    botch("new failure in var_intarray");
  vi->**(IntArray *Var_Info::**)v = iv;
  for(i = 0; i < n; i++)
    (*iv)[i] = z[i];
}

void NIDRProblemDescDB::
var_rvec(const char *keyname, Values *val, void **g, void *v)
{
  RealVector *rv
    = &((*(Var_Info**)g)->dv->**(RealVector DataVariablesRep::**)v);
  size_t i, n = val->n;
  rv->sizeUninitialized(n);

  Real *r = val->r;
  for(i = 0; i < n; i++)
    (*rv)[i] = r[i];
}

void NIDRProblemDescDB::
var_ivec(const char *keyname, Values *val, void **g, void *v)
{
  IntVector *iv = &((*(Var_Info**)g)->dv->**(IntVector DataVariablesRep::**)v);
  size_t i, n = val->n;
  iv->sizeUninitialized(n);

  int *z = val->i;
  for(i = 0; i < n; i++)
    (*iv)[i] = z[i];
}

void NIDRProblemDescDB::
var_pintz(const char *keyname, Values *val, void **g, void *v)
{
  int n = *val->i; // test value as int, prior to storage as size_t
#ifdef REDUNDANT_INT_CHECKS
  if (n <= 0) /* now handled by INTEGER > 0 in the .nspec file */
    botch("%s must be positive", keyname);
#endif
  (*(Var_Info**)g)->dv->**(size_t DataVariablesRep::**)v = n;
}

void NIDRProblemDescDB::
var_type(const char *keyname, Values *val, void **g, void *v)
{
  (*(Var_Info**)g)->dv->*((Var_mp_type*)v)->sp = ((Var_mp_type*)v)->type;
}

void NIDRProblemDescDB::
var_start(const char *keyname, Values *val, void **g, void *v)
{
  Var_Info *vi;

  if (!(vi = new Var_Info))
  Botch:		botch("new failure in var_start");
  memset(vi, 0, sizeof(Var_Info));
  if (!(vi->dv_handle = new DataVariables))
    goto Botch;
  vi->dv = vi->dv_handle->dataVarsRep;
  *g = (void*)vi;
}

void NIDRProblemDescDB::
var_true(const char *keyname, Values *val, void **g, void *v)
{
  (*(Var_Info**)g)->dv->**(bool DataVariablesRep::**)v = true;
}

static int wronglen(size_t n, RealVector *V, const char *what)
{
  size_t n1 = V->length();
  if (n != n1) {
    Squawk("Expected %d numbers for %s, but got %d", (int)n, what, (int)n1);
    return 1;
  }
  return 0;
}

static int wronglen(size_t n, IntVector *V, const char *what)
{
  size_t n1 = V->length();
  if (n != n1) {
    Squawk("Expected %d numbers for %s, but got %d", (int)n, what, (int)n1);
    return 1;
  }
  return 0;
}

static void Vcopyup(RealVector *V, RealVector *M, size_t i, size_t n)
{
  size_t j;
  for(j = 0; j < n; ++i, ++j)
    (*V)[i] = (*M)[j];
}

static void Set_rv(RealVector *V, double d, size_t n)
{
  size_t i;
  V->sizeUninitialized(n);
  for(i = 0; i < n; ++i)
    (*V)[i] = d;
}

static void Set_iv(IntVector *V, int d, size_t n)
{
  size_t i;
  V->sizeUninitialized(n);
  for(i = 0; i < n; ++i)
    (*V)[i] = d;
}

static void
wrong_number(const char *what, const char *kind, size_t nsv, size_t m)
{
  Squawk("Expected %d %s for %s, not %d", (int)nsv, what, kind, (int)m);
}

static void too_small(const char *kind)
{
  Squawk("num_set_values values for %s must be >= 1", kind);
}

static void not_div(const char *kind, size_t nsv, size_t m)
{
  Squawk("Number of %s set_values (%d) not evenly divisible by number of variables (%d); use num_set_values for unequal apportionment",
	 kind, (int)nsv, (int)m);
}

static void suppressed(const char *kind, int ndup, int *ip, Real *rp)
{
  const char *s;
  int i, nother;

  nother = 0;
  if (ndup > 2) {
    nother = ndup - 1;
    ndup = 1;
  }
  for(i = 0; i < ndup; ++i)
    if (ip)
      Squawk("Duplicate %s value %d", kind, ip[i]);
    else
      Squawk("Duplicate %s value %.17g", kind, rp[i]);
  if (nother) {
    s = "s" + (nother == 1);
    Squawk("Warning%s of %d other duplicate %s value%s suppressed",
	   s, nother, kind, s);
  }
}

static void bad_initial_ivalue(const char *kind, int val)
{
  Squawk("invalid initial value %d for %s", val, kind);
}

static void bad_initial_rvalue(const char *kind, Real val)
{
  Squawk("invalid initial value %.17g for %s", val, kind);
}

// *****************************************************************************
//  Vchk functions called earlier from within check_variables_node(), which
//   immediately {precedes,follows} DB buffer {send,receive} in broadcast().
// Vgen functions called later from within make_variable_defaults()
//   (from within post_process() following broadcast()).
// As documented in ProblemDescDB::manage_inputs(), Vchk applies to minimal
//   spec data, whereas Vgen constructs any large inferred vectors.
// *****************************************************************************
static void Vgen_ContinuousDes(DataVariablesRep *dv, size_t offset)
{
  RealVector *L, *U, *V;
  size_t i, n = dv->numContinuousDesVars;

  L = &dv->continuousDesignLowerBnds;
  U = &dv->continuousDesignUpperBnds;
  V = &dv->continuousDesignVars;
  if (L->length() == 0)
    Set_rv(L, -DBL_MAX, n);
  if (U->length() == 0)
    Set_rv(U, DBL_MAX, n);
  if (V->length() == 0) {
    V->sizeUninitialized(n);
    for(i = 0; i < n; i++) { // init to 0, repairing to bounds if needed
      if      ((*L)[i] > 0.) (*V)[i] = (*L)[i];
      else if ((*U)[i] < 0.) (*V)[i] = (*U)[i];
      else                   (*V)[i] = 0;
    }
  }
}

static void Vgen_DiscreteDesRange(DataVariablesRep *dv, size_t offset)
{
  IntVector *L, *U, *V;
  size_t i, n = dv->numDiscreteDesRangeVars;

  L = &dv->discreteDesignRangeLowerBnds;
  U = &dv->discreteDesignRangeUpperBnds;
  V = &dv->discreteDesignRangeVars;
  if (L->length() == 0)
    Set_iv(L, INT_MIN, n);
  if (U->length() == 0)
    Set_iv(U, INT_MAX, n);
  if (V->length() == 0) {
    V->sizeUninitialized(n);
    for(i = 0; i < n; ++i) { // init to 0, repairing to bounds if needed
      if      ((*L)[i] > 0) (*V)[i] = (*L)[i];
      else if ((*U)[i] < 0) (*V)[i] = (*U)[i];
      else                  (*V)[i] = 0;
    }
  }
}

static void Vgen_ContinuousState(DataVariablesRep *dv, size_t offset)
{
  RealVector *L, *U, *V;
  size_t i, n = dv->numContinuousStateVars;

  L = &dv->continuousStateLowerBnds;
  U = &dv->continuousStateUpperBnds;
  V = &dv->continuousStateVars;
  if (L->length() == 0)
    Set_rv(L, -DBL_MAX, n);
  if (U->length() == 0)
    Set_rv(U, DBL_MAX, n);
  if (V->length() == 0) {
    V->sizeUninitialized(n);
    for(i = 0; i < n; i++) { // init to 0, repairing to bounds if needed
      if      ((*L)[i] > 0.) (*V)[i] = (*L)[i];
      else if ((*U)[i] < 0.) (*V)[i] = (*U)[i];
      else                   (*V)[i] = 0;
    }
  }
}

static void Vgen_DiscreteStateRange(DataVariablesRep *dv, size_t offset)
{
  IntVector *L, *U, *V;
  size_t i, n = dv->numDiscreteStateRangeVars;

  L = &dv->discreteStateRangeLowerBnds;
  U = &dv->discreteStateRangeUpperBnds;
  V = &dv->discreteStateRangeVars;
  if (L->length() == 0)
    Set_iv(L, INT_MIN, n);
  if (U->length() == 0)
    Set_iv(U, INT_MAX, n);
  if (V->length() == 0) {
    V->sizeUninitialized(n);
    for(i = 0; i < n; ++i) { // init to 0, repairing to bounds if needed
      if      ((*L)[i] > 0) (*V)[i] = (*L)[i];
      else if ((*U)[i] < 0) (*V)[i] = (*U)[i];
      else                  (*V)[i] = 0;
    }
  }
}

static void Vchk_NormalUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t n;
  RealVector *B, *M, *Sd;

  n = dv->numNormalUncVars;
  if (wronglen(n,  M = &dv->normalUncMeans,   "nuv_means") ||
      wronglen(n, Sd = &dv->normalUncStdDevs, "nuv_std_deviations"))
    return;
  B = &dv->normalUncLowerBnds;
  if (B->length() && wronglen(n, B, "nuv_lower_bounds"))
    return;
  B = &dv->normalUncUpperBnds;
  if (B->length())
    wronglen(n, B, "nuv_upper_bounds");
}

static void Vgen_NormalUnc(DataVariablesRep *dv, size_t offset)
{
  short bds;
  size_t i, j, n;
  Real mean, stdev, nudge, lower, upper;
  RealVector *B, *L, *M, *Sd, *U, *V, *IP;

  n  =  dv->numNormalUncVars;
  M  = &dv->normalUncMeans;      Sd = &dv->normalUncStdDevs;
  L  = &dv->normalUncLowerBnds;  U  = &dv->normalUncUpperBnds;
  IP = &dv->normalUncVars;       V  = &dv->continuousAleatoryUncVars;

  // process lower bounds
  B = &dv->continuousAleatoryUncLowerBnds;
  if (L->length()) {
    Vcopyup(B, L, offset, n); // global = distribution
    bds = 1;
  }
  else {
    Set_rv(L, -DBL_MAX, n); // distribution
    for(j = 0; j < n; ++j)
      (*B)[offset+j] = (*M)[j] - 3.*(*Sd)[j]; // inferred global
    bds = 0;
  }

  // process upper bounds
  B = &dv->continuousAleatoryUncUpperBnds;
  if (U->length()) {
    Vcopyup(B, U, offset, n); // global = distribution
    bds |= 2;
  }
  else {
    Set_rv(U, DBL_MAX, n); // distribution
    for(j = 0; j < n; ++j)
      (*B)[offset+j] = (*M)[j] + 3.*(*Sd)[j]; // inferred global
  }

  // Set initial values and repair to bounds, if needed
  if (IP->length()) { // in this case, don't nudge since not a default value

    dv->uncertainVarsInitPt = true;

    for (i = offset, j = 0; j < n; ++i, ++j)
      if      ((*IP)[j] < (*L)[j]) (*V)[i] =  (*L)[j];
      else if ((*IP)[j] > (*U)[j]) (*V)[i] =  (*U)[j];
      else                         (*V)[i] = (*IP)[j];
  }
  else { // > bds is 0, 1, 2, or 3 (0 ==> no bounds given, nothing more to do)

    //Vcopyup(V, M, offset, n);
    switch(bds) {
    case 1: // only lower bounds given
      for (i = offset, j = 0; j < n; ++i, ++j)
	if ((*M)[j] <= (*L)[j]) (*V)[i] = (*L)[j] + 0.5*(*Sd)[j];
	else                    (*V)[i] = (*M)[j];
      break;

    case 2: // only upper bounds given
      for (i = offset, j = 0; j < n; ++i, ++j)
	if ((*M)[j] >= (*U)[j]) (*V)[i] = (*U)[j] - 0.5*(*Sd)[j];
	else                    (*V)[i] = (*M)[j];
      break;

    case 3: // both lower and upper bounds given
      for (i = offset, j = 0; j < n; ++i, ++j) {
	lower  = (*L)[j]; upper = (*U)[j];
	nudge  = 0.5 * std::min((*Sd)[j], upper - lower);
	lower += nudge;   upper -= nudge;  mean = (*M)[j];
 	if      (mean < lower) (*V)[i] = lower;
	else if (mean > upper) (*V)[i] = upper;
	else                   (*V)[i] = mean;
      }
      break;
    }
  }
}

static void Vchk_LognormalUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t n;
  RealVector *B, *L, *M, *Sd, *Z;

  // lambda/zeta, mean/std_deviation, or mean/error_factor
  n = dv->numLognormalUncVars;
  L = &dv->lognormalUncLambdas;
  M = &dv->lognormalUncMeans;
  if (L->length()) {
    if (wronglen(n, L, "lnuv_lambdas"))
      return;
    if (wronglen(n, Z = &dv->lognormalUncZetas, "lnuv_zetas"))
      return;
  }
  else if (M->length()) {
    if (wronglen(n, M, "lnuv_means"))
      return;
    Sd = &dv->lognormalUncStdDevs;
    if (Sd->length() && wronglen(n, Sd, "lnuv_std_deviations"))
      return;
  }
  else if (wronglen(n, &dv->lognormalUncErrFacts, "lnuv_error_factors"))
    return;

  // lower bounds
  B = &dv->lognormalUncLowerBnds;
  if (B->length() && wronglen(n, B, "lnuv_lower_bounds"))
    return;

  // upper bounds
  B = &dv->lognormalUncUpperBnds;
  if (B->length())
    wronglen(n, B, "lnuv_upper_bounds");
}

static void Vgen_LognormalUnc(DataVariablesRep *dv, size_t offset)
{
  size_t i, j, n;
  Real mean, stdev, nudge, lower, upper;
  RealVector *B, *Ef, *Lam, *L, *M, *Sd, *U, *V, *Z, *IP;

  // lambda/zeta, mean/std_deviation, or mean/error_factor
  n  = dv->numLognormalUncVars;    Lam = &dv->lognormalUncLambdas;
  Z  = &dv->lognormalUncZetas;     Ef  = &dv->lognormalUncErrFacts;
  M  = &dv->lognormalUncMeans;     Sd  = &dv->lognormalUncStdDevs;
  L  = &dv->lognormalUncLowerBnds; U   = &dv->lognormalUncUpperBnds; 
  IP = &dv->lognormalUncVars;      V   = &dv->continuousAleatoryUncVars;

  size_t num_Sd = Sd->length(), num_Lam = Lam->length(), num_IP = IP->length(),
    num_L = L->length(), num_U = U->length();

  if (num_IP) dv->uncertainVarsInitPt = true;

  // manage distribution and global bounds.  Global are inferred if
  // distribution are not specified.
  if (!num_L) L->size(n); // inits L to zeros --> default {dist,global}
  Vcopyup(&dv->continuousAleatoryUncLowerBnds, L, offset, n); // global = dist
  B = &dv->continuousAleatoryUncUpperBnds;
  if (num_U) Vcopyup(B, U, offset, n); // global = dist
  else       Set_rv(U, DBL_MAX, n);    // default dist; global inferred below

  for (i = offset, j = 0; j < n; ++i, ++j) {

    // extract mean & stdev, if needed
    if (!num_IP || !num_U) {
      if (num_Lam)  // lambda/zeta
	Pecos::moments_from_lognormal_params((*Lam)[j], (*Z)[j], mean, stdev);
      else {
	mean = (*M)[j];
	if (num_Sd) // mean/std_deviation
	  stdev = (*Sd)[j];
	else        // mean/error_factor
	  Pecos::lognormal_std_deviation_from_err_factor(mean, (*Ef)[j], stdev);
      }
    }

    // Repair initial values to bounds, if needed
    if (num_IP) { // in this case, don't nudge since not a default value

      // TO DO: set user spec flag

      if      ((*IP)[j] < (*L)[j]) (*V)[i] =  (*L)[j];
      else if ((*IP)[j] > (*U)[j]) (*V)[i] =  (*U)[j];
      else                         (*V)[i] = (*IP)[j];
    }
    else {
      // repair bounds exceedance in default value, if needed
      lower  = (*L)[j]; upper = (*U)[j];
      nudge  = 0.5 * std::min(stdev, upper - lower);
      lower += nudge;   upper -= nudge;
      if      (mean < lower) (*V)[i] = lower;
      else if (mean > upper) (*V)[i] = upper;
      else                   (*V)[i] = mean;
    }

    // infer global bounds if no distribution bounds spec
    if (!num_U)
      (*B)[i] = mean + 3.*stdev;
  }
}

static void Vchk_UniformUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t n;
  RealVector *L, *U;

  n = dv->numUniformUncVars;
  L = &dv->uniformUncLowerBnds;
  U = &dv->uniformUncUpperBnds;
  if (wronglen(n, L, "uuv_lower_bounds") || wronglen(n, U, "uuv_upper_bounds"))
    return;
}

static void Vgen_UniformUnc(DataVariablesRep *dv, size_t offset)
{
  size_t i, j, n;
  Real stdev;
  RealVector *L, *U, *V, *IP;

  n = dv->numUniformUncVars;   IP = &dv->uniformUncVars;
  L = &dv->uniformUncLowerBnds; U = &dv->uniformUncUpperBnds;
  Vcopyup(&dv->continuousAleatoryUncLowerBnds, L, offset, n);
  Vcopyup(&dv->continuousAleatoryUncUpperBnds, U, offset, n);
  V = &dv->continuousAleatoryUncVars;
  if (IP->length()) {
    dv->uncertainVarsInitPt = true;
    for (i = offset, j = 0; j < n; ++i, ++j)
      if      ((*IP)[j] < (*L)[j]) (*V)[i] =  (*L)[j];
      else if ((*IP)[j] > (*U)[j]) (*V)[i] =  (*U)[j];
      else                         (*V)[i] = (*IP)[j];
  }
  else
    for(i = offset, j = 0; j < n; ++i, ++j)
      Pecos::moments_from_uniform_params((*L)[j], (*U)[j], (*V)[i], stdev);
}

static void 
Vchk_LoguniformUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t j, n;
  Real Lj, Uj;
  RealVector *L, *U;

  n = dv->numLoguniformUncVars;
  L = &dv->loguniformUncLowerBnds;
  U = &dv->loguniformUncUpperBnds;
  if (wronglen(n, L, "luuv_lower_bounds") ||
      wronglen(n, U, "luuv_upper_bounds"))
    return;
  for(j = 0; j < n; ++j) {
    Lj = (*L)[j];
    Uj = (*U)[j];
    if (Lj <= 0. || Uj <= 0.) {
      Squawk("loguniform bounds must be positive");
      return;
    }
    if (Lj >= DBL_MAX || Uj >= DBL_MAX) {
      Squawk("loguniform bounds must be finite");
      return;
    }
    if (Lj > Uj) {
      Squawk("loguniform lower bound greater than upper bound");
      return;
    }
  }
}

static void Vgen_LoguniformUnc(DataVariablesRep *dv, size_t offset)
{
  size_t i, j, n;
  Real stdev;
  RealVector *L, *U, *V, *IP;

  n = dv->numLoguniformUncVars;   IP = &dv->loguniformUncVars;
  L = &dv->loguniformUncLowerBnds; U = &dv->loguniformUncUpperBnds;
  Vcopyup(&dv->continuousAleatoryUncLowerBnds, L, offset, n);
  Vcopyup(&dv->continuousAleatoryUncUpperBnds, U, offset, n);
  V = &dv->continuousAleatoryUncVars;
  if (IP->length()) {
    dv->uncertainVarsInitPt = true;
    for (i = offset, j = 0; j < n; ++i, ++j)
      if      ((*IP)[j] < (*L)[j]) (*V)[i] =  (*L)[j];
      else if ((*IP)[j] > (*U)[j]) (*V)[i] =  (*U)[j];
      else                         (*V)[i] = (*IP)[j];
  }
  else
    for(i = offset, j = 0; j < n; ++i, ++j)
      Pecos::moments_from_loguniform_params((*L)[j], (*U)[j], (*V)[i], stdev);
}

static void Vchk_TriangularUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t j, n;
  Real Lj, Mj, Uj;
  RealVector *L, *M, *U;

  n = dv->numTriangularUncVars;    M = &dv->triangularUncModes;
  L = &dv->triangularUncLowerBnds; U = &dv->triangularUncUpperBnds;
  if (wronglen(n, L, "tuv_lower_bounds") || wronglen(n, M, "tuv_modes") ||
      wronglen(n, U, "tuv_upper_bounds"))
    return;
  for(j = 0; j < n; ++j) {
    Lj = (*L)[j]; Mj = (*M)[j]; Uj = (*U)[j];
    if (Lj > Mj || Mj > Uj) {
      Squawk("triangular uncertain variables must have\n\t"
	     "tuv_lower_bounds <= tuv_modes <= tuv_upper_bounds");
      return;
    }
  }
}

static void Vgen_TriangularUnc(DataVariablesRep *dv, size_t offset)
{
  size_t i, j, n;
  Real stdev;
  RealVector *L, *M, *U, *V, *IP;

  n = dv->numTriangularUncVars;   IP = &dv->triangularUncVars;
  L = &dv->triangularUncLowerBnds; U = &dv->triangularUncUpperBnds;
  Vcopyup(&dv->continuousAleatoryUncLowerBnds, L, offset, n);
  Vcopyup(&dv->continuousAleatoryUncUpperBnds, U, offset, n);
  V = &dv->continuousAleatoryUncVars;
  if (IP->length()) {
    dv->uncertainVarsInitPt = true;
    for (i = offset, j = 0; j < n; ++i, ++j)
      if      ((*IP)[j] < (*L)[j]) (*V)[i] =  (*L)[j];
      else if ((*IP)[j] > (*U)[j]) (*V)[i] =  (*U)[j];
      else                         (*V)[i] = (*IP)[j];
  }
  else {
    M = &dv->triangularUncModes;
    for(i = offset, j = 0; j < n; ++i, ++j)
      Pecos::moments_from_triangular_params((*L)[j], (*U)[j], (*M)[j],
					    (*V)[i], stdev);
  }
}

static void 
Vchk_ExponentialUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t n = dv->numExponentialUncVars;
  RealVector *B = &dv->exponentialUncBetas;
  if (wronglen(n, B, "euv_betas"))
    return;
}

static void Vgen_ExponentialUnc(DataVariablesRep *dv, size_t offset)
{
  Real mean, stdev;
  RealVector *B, *L, *U, *V, *IP;

  B = &dv->exponentialUncBetas; IP = &dv->exponentialUncVars;
  V = &dv->continuousAleatoryUncVars;
  L = &dv->continuousAleatoryUncLowerBnds;
  U = &dv->continuousAleatoryUncUpperBnds;
  size_t i, j, n = dv->numExponentialUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    Pecos::moments_from_exponential_params((*B)[j], mean, stdev);
    (*L)[i] = 0.;
    (*U)[i] = mean + 3.*stdev;
    if (num_IP) (*V)[i] = (*IP)[j];
    else        (*V)[i] = mean;
  }
}

static void Vchk_BetaUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  RealVector *A, *B, *L, *U;

  A = &dv->betaUncAlphas;    B = &dv->betaUncBetas;
  L = &dv->betaUncLowerBnds; U = &dv->betaUncUpperBnds;

  size_t n = dv->numBetaUncVars;
  if (wronglen(n, A, "buv_alphas")       || wronglen(n, B, "buv_betas") ||
      wronglen(n, L, "buv_lower_bounds") || wronglen(n, U, "buv_upper_bounds"))
    return;
}

static void Vgen_BetaUnc(DataVariablesRep *dv, size_t offset)
{
  Real stdev;
  RealVector *A, *B, *L, *U, *V, *IP;

  size_t i, j, n = dv->numBetaUncVars;
  A = &dv->betaUncAlphas;    B = &dv->betaUncBetas;
  L = &dv->betaUncLowerBnds; U = &dv->betaUncUpperBnds;
  V = &dv->continuousAleatoryUncVars; IP = &dv->betaUncVars;
  Vcopyup(&dv->continuousAleatoryUncLowerBnds, L, offset, n);
  Vcopyup(&dv->continuousAleatoryUncUpperBnds, U, offset, n);
  if (IP->length()) {
    dv->uncertainVarsInitPt = true;
    for (i = offset, j = 0; j < n; ++i, ++j)
      if      ((*IP)[j] < (*L)[j]) (*V)[i] =  (*L)[j];
      else if ((*IP)[j] > (*U)[j]) (*V)[i] =  (*U)[j];
      else                         (*V)[i] = (*IP)[j];
  }
  else
    for(i = offset, j = 0; j < n; ++i, ++j)
      Pecos::moments_from_beta_params((*L)[j], (*U)[j], (*A)[j],
				      (*B)[j], (*V)[i], stdev);
}

static void Vchk_GammaUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  RealVector *A, *B;

  size_t n = dv->numGammaUncVars;
  A = &dv->gammaUncAlphas; B = &dv->gammaUncBetas;
  if (wronglen(n, A, "gauv_alphas") || wronglen(n, B, "gauv_betas"))
    return;
}

static void Vgen_GammaUnc(DataVariablesRep *dv, size_t offset)
{
  Real mean, stdev;
  RealVector *A, *B, *L, *U, *V, *IP;

  A = &dv->gammaUncAlphas; B = &dv->gammaUncBetas;
  L = &dv->continuousAleatoryUncLowerBnds;
  U = &dv->continuousAleatoryUncUpperBnds;
  V = &dv->continuousAleatoryUncVars; IP = &dv->gammaUncVars;
  size_t i, j, n = dv->numGammaUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    Pecos::moments_from_gamma_params((*A)[j], (*B)[j], mean, stdev);
    (*L)[i] = 0.;
    (*U)[i] = mean + 3.*stdev;
    if (num_IP) (*V)[i] = (*IP)[j];
    else        (*V)[i] = mean;
  }
}

static void Vchk_GumbelUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  RealVector *A, *B;

  size_t n = dv->numGumbelUncVars;
  A = &dv->gumbelUncAlphas; B = &dv->gumbelUncBetas;
  if (wronglen(n, A, "guuv_alphas") || wronglen(n, B, "guuv_betas"))
    return;
}

static void Vgen_GumbelUnc(DataVariablesRep *dv, size_t offset)
{
  Real mean, stdev;
  RealVector *A, *B, *L, *U, *V, *IP;

  A = &dv->gumbelUncAlphas; B = &dv->gumbelUncBetas;
  L = &dv->continuousAleatoryUncLowerBnds;
  U = &dv->continuousAleatoryUncUpperBnds;
  V = &dv->continuousAleatoryUncVars; IP = &dv->gumbelUncVars;
  size_t i, j, n = dv->numGumbelUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    Pecos::moments_from_gumbel_params((*A)[j], (*B)[j], mean, stdev);
    (*L)[i] = mean - 3.*stdev;
    (*U)[i] = mean + 3.*stdev;
    if (num_IP) (*V)[i] = (*IP)[j];
    else        (*V)[i] = mean;
  }
}

static void Vchk_FrechetUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  RealVector *A, *B;

  size_t n = dv->numFrechetUncVars;
  A = &dv->frechetUncAlphas; B = &dv->frechetUncBetas;
  if (wronglen(n, A, "fuv_alphas") || wronglen(n, B, "fuv_betas"))
    return;
}

static void Vgen_FrechetUnc(DataVariablesRep *dv, size_t offset)
{
  Real mean, stdev;
  RealVector *A, *B, *L, *U, *V, *IP;

  A = &dv->frechetUncAlphas; B = &dv->frechetUncBetas;
  L = &dv->continuousAleatoryUncLowerBnds;
  U = &dv->continuousAleatoryUncUpperBnds;
  V = &dv->continuousAleatoryUncVars; IP = &dv->frechetUncVars;
  size_t i, j, n = dv->numFrechetUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    Pecos::moments_from_frechet_params((*A)[j], (*B)[j], mean, stdev);
    (*L)[i] = 0.;
    (*U)[i] = mean + 3.*stdev;
    if (num_IP) (*V)[i] = (*IP)[j];
    else        (*V)[i] = mean;
  }
}

static void Vchk_WeibullUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  RealVector *A, *B;

  size_t n = dv->numWeibullUncVars;
  A = &dv->weibullUncAlphas; B = &dv->weibullUncBetas;
  if (wronglen(n, A, "wuv_alphas") || wronglen(n, B, "wuv_betas"))
    return;
}

static void Vgen_WeibullUnc(DataVariablesRep *dv, size_t offset)
{
  Real mean, stdev;
  RealVector *A, *B, *L, *U, *V, *IP;

  A = &dv->weibullUncAlphas; B = &dv->weibullUncBetas;
  L = &dv->continuousAleatoryUncLowerBnds;
  U = &dv->continuousAleatoryUncUpperBnds;
  V = &dv->continuousAleatoryUncVars; IP = &dv->weibullUncVars;
  size_t i, j, n = dv->numWeibullUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    Pecos::moments_from_weibull_params((*A)[j], (*B)[j], mean, stdev);
    (*L)[i] = 0.;
    (*U)[i] = mean + 3.*stdev;
    if (num_IP) (*V)[i] = (*IP)[j];
    else        (*V)[i] = mean;
  }
}

static void 
Vchk_HistogramBinUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  IntArray *nhbp;
  RealVector *hba, *hbo, *hbc, *hbpi;
  RealVectorArray *hbp;
  int nhbpi, avg_nhbpi;
  size_t i, j, num_a, num_o, num_c, m, n, tothbp, cntr;
  Real x, y, bin_width, count_sum;

  if (hba = vi->hba) { // abscissas are required
    num_a = hba->length();                         // abscissas
    hbo = vi->hbo; num_o = (hbo) ? hbo->length() : 0; // ordinates
    hbc = vi->hbc; num_c = (hbc) ? hbc->length() : 0; // counts
    if (num_o && num_o != num_a) {
      Squawk("Expected %d ordinates, not %d", num_a, num_o);
      return;
    }
    if (num_c && num_c != num_a) {
      Squawk("Expected %d counts, not %d", num_a, num_c);
      return;
    }
    bool key;
    if (nhbp = vi->nhbp) {
      key = true;
      m = nhbp->size();
      //dv->numHistogramBinUncVars = m;
      for(i=tothbp=0; i<m; ++i) {
	tothbp += nhbpi = (*nhbp)[i];
	if (nhbpi < 2) {
	  Squawk("num_pairs must be >= 2");
	  return;
	}
      }
      if (num_a != tothbp) {
	Squawk("Expected %d abscissas, not %d", tothbp, num_a);
	return;
      }
    }
    else {
      key = false;
      m = dv->numHistogramBinUncVars;
      if (num_a % m) {
	Squawk("Number of abscissas (%d) not evenly divisible by number of variables (%d); Use num_pairs for unequal apportionment", num_a, m);
	return;
      }
      else
	avg_nhbpi = num_a / m;
    }
    hbp = &dv->histogramUncBinPairs;
    hbp->resize(m);
    for (i=cntr=0; i<m; ++i) {
      nhbpi = (key) ? (*nhbp)[i] : avg_nhbpi;
      hbpi  = &((*hbp)[i]);
      hbpi->sizeUninitialized(2*nhbpi);
      count_sum = 0.;
      for (j=0; j<nhbpi; ++j, ++cntr) {
	Real x = (*hba)[cntr];                          // abscissas
	Real y = (num_o) ? (*hbo)[cntr] : (*hbc)[cntr]; // ordinates/counts
	if (j<nhbpi-1) {
	  Real bin_width = (*hba)[cntr+1] - x;
	  if (bin_width <= 0.) {
	    Squawk("histogram bin x values must increase");
	    return;
	  }
	  if (y <= 0.) {
	    Squawk("nonpositive intermediate histogram bin y value");
	    return;
	  }
	  if (num_o) // convert from ordinates (probability density) to counts
	    y *= bin_width;
	  count_sum += y;
	}
	else if (y != 0) {
	  Squawk("histogram bin y values must end with 0");
	  return;
	}
	(*hbpi)[2*j]   = x; // abscissa
	(*hbpi)[2*j+1] = y; // count
      }
      // normalize counts to sum to 1
      for (j=0; j<nhbpi-1; ++j)
	(*hbpi)[2*j+1] /= count_sum;
    }
  }
}

static void Vgen_HistogramBinUnc(DataVariablesRep *dv, size_t offset)
{
  RealVector *L, *U, *V, *r, *IP;
  Real mean, stdev;

  L = &dv->continuousAleatoryUncLowerBnds;
  U = &dv->continuousAleatoryUncUpperBnds;
  V = &dv->continuousAleatoryUncVars; IP = &dv->histogramBinUncVars;
  RealVectorArray *A = &dv->histogramUncBinPairs;
  size_t i, j, n = dv->numHistogramBinUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    r = &((*A)[j]);
    (*L)[i] = (*r)[0];
    (*U)[i] = (*r)[r->length() - 2];
    if (num_IP) {
      if      ((*IP)[j] < (*L)[i]) (*V)[i] =  (*L)[i];
      else if ((*IP)[j] > (*U)[i]) (*V)[i] =  (*U)[i];
      else                         (*V)[i] = (*IP)[j];
    }
    else Pecos::moments_from_histogram_bin_params(*r, (*V)[i], stdev);
  }
}

static void Vchk_PoissonUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t n = dv->numPoissonUncVars;
  RealVector *A = &dv->poissonUncLambdas;
  if (wronglen(n, A, "lambdas"))
    return;
}

static void Vgen_PoissonUnc(DataVariablesRep *dv, size_t offset)
{
  IntVector *L, *U, *V, *IP;
  Real mean, std_dev;
  RealVector *Lam;

  L = &dv->discreteIntAleatoryUncLowerBnds;
  U = &dv->discreteIntAleatoryUncUpperBnds;
  V = &dv->discreteIntAleatoryUncVars;
  IP = &dv->poissonUncVars; Lam = &dv->poissonUncLambdas;
  size_t i, j, n = dv->numPoissonUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    Pecos::moments_from_poisson_params((*Lam)[j], mean, std_dev);
    (*L)[i] = 0;
    (*U)[i] = (int)std::ceil(mean + 3.*std_dev);
    if (num_IP) (*V)[i] = (*IP)[j];
    else        (*V)[i] = (int)mean;
  }
}

static void Vchk_BinomialUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t n = dv->numBinomialUncVars;
  RealVector *A = &dv->binomialUncProbPerTrial;
  IntVector  *B = &dv->binomialUncNumTrials;
  if (wronglen(n, A, "prob_per_trial") ||  wronglen(n, B, "num_trials"))
    return;
}

static void Vgen_BinomialUnc(DataVariablesRep *dv, size_t offset)
{
  IntVector *L, *NT, *U, *V, *IP;
  Real mean, std_dev;
  RealVector *Pr;

  L = &dv->discreteIntAleatoryUncLowerBnds;
  U = &dv->discreteIntAleatoryUncUpperBnds;
  V = &dv->discreteIntAleatoryUncVars; IP = &dv->binomialUncVars;
  NT = &dv->binomialUncNumTrials;      Pr = &dv->binomialUncProbPerTrial;
  size_t i, j, n = dv->numBinomialUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    (*L)[i] = 0;
    (*U)[i] = (*NT)[j];
    if (num_IP) {
      if ((*IP)[j] > (*U)[i]) (*V)[i] =  (*U)[i];
      else                    (*V)[i] = (*IP)[j];
    }
    else {
      Pecos::moments_from_binomial_params((*Pr)[j], (*NT)[j], mean, std_dev);
      (*V)[i] = (int)mean;
    }
  }
}

static void 
Vchk_NegBinomialUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t n = dv->numNegBinomialUncVars;
  RealVector *A = &dv->negBinomialUncProbPerTrial;
  IntVector  *B = &dv->negBinomialUncNumTrials;
  if (wronglen(n, A, "prob_per_trial") || wronglen(n, B, "num_trials"))
    return;
}

static void Vgen_NegBinomialUnc(DataVariablesRep *dv, size_t offset)
{
  IntVector *L, *NT, *U, *V, *IP;
  Real mean, std_dev;
  RealVector *Pr;

  L = &dv->discreteIntAleatoryUncLowerBnds;
  U = &dv->discreteIntAleatoryUncUpperBnds;
  V = &dv->discreteIntAleatoryUncVars; IP = &dv->negBinomialUncVars;
  NT = &dv->negBinomialUncNumTrials;   Pr = &dv->negBinomialUncProbPerTrial;
  size_t i, j, n = dv->numNegBinomialUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    Pecos::moments_from_negative_binomial_params((*Pr)[j], (*NT)[j],
						 mean, std_dev);
    (*L)[i] = (*NT)[j];
    (*U)[i] = (int)std::ceil(mean + 3.*std_dev);
    if (num_IP) {
      if ((*IP)[j] < (*L)[i]) (*V)[i] =  (*L)[i];
      else                    (*V)[i] = (*IP)[j];
    }
    else                      (*V)[i] = (int)mean;
  }
}

static void Vchk_GeometricUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t n = dv->numGeometricUncVars;
  RealVector *A = &dv->geometricUncProbPerTrial;
  if (wronglen(n, A, "prob_per_trial"))
    return;
}

static void Vgen_GeometricUnc(DataVariablesRep *dv, size_t offset)
{
  IntVector *L, *U, *V, *IP;
  Real mean, std_dev;
  RealVector *Pr;

  L = &dv->discreteIntAleatoryUncLowerBnds;
  U = &dv->discreteIntAleatoryUncUpperBnds;
  V = &dv->discreteIntAleatoryUncVars; IP = &dv->geometricUncVars;
  Pr = &dv->geometricUncProbPerTrial;
  size_t i, j, n = dv->numGeometricUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    (*L)[i] = 0;
    Pecos::moments_from_geometric_params((*Pr)[j], mean, std_dev);
    (*U)[i] = (int)std::ceil(mean + 3.*std_dev);
    if (num_IP) (*V)[i] = (*IP)[j];
    else        (*V)[i] = (int)mean;
  }
}

static void Vchk_HyperGeomUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  IntVector *A, *B, *C;

  size_t n = dv->numHyperGeomUncVars;
  A = &dv->hyperGeomUncTotalPop;
  B = &dv->hyperGeomUncSelectedPop;
  C = &dv->hyperGeomUncNumDrawn;
  if (wronglen(n, A, "total_population")    ||
      wronglen(n, B, "selected_population") || wronglen(n, C, "num_drawn"))
    return;
}

static void Vgen_HyperGeomUnc(DataVariablesRep *dv, size_t offset)
{
  IntVector *L, *ND, *NS, *TP, *U, *V, *IP;
  Real mean, std_dev;

  L = &dv->discreteIntAleatoryUncLowerBnds;
  U = &dv->discreteIntAleatoryUncUpperBnds;
  V = &dv->discreteIntAleatoryUncVars;
  ND = &dv->hyperGeomUncNumDrawn; NS = &dv->hyperGeomUncSelectedPop;
  TP = &dv->hyperGeomUncTotalPop; IP = &dv->hyperGeomUncVars;
  size_t i, j, n = dv->numHyperGeomUncVars, num_IP = IP->length();
  int d, s;
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    (*L)[i] = 0;
    d = (*ND)[j]; s = (*NS)[j];
    (*U)[i] = std::min(d, s);
    if (num_IP) {
      if ((*IP)[j] > (*U)[i]) (*V)[i] =  (*U)[i];
      else                    (*V)[i] = (*IP)[j];
    }
    else {
      Pecos::moments_from_hypergeometric_params((*TP)[j], s, d, mean, std_dev);
      (*V)[i] = (int)mean;
    }
  }
}

static void 
Vchk_HistogramPtUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  IntArray *nhpp;
  RealVector *hpa, *hpc, *hppi;
  RealVectorArray *hpp;
  int nhppi, avg_nhppi;
  size_t i, j, num_a, num_c, m, n, tothpp, cntr;
  Real x, y, bin_width, count_sum;

  if (hpa = vi->hpa) {
    num_a = hpa->length();              // abscissas
    hpc = vi->hpc; num_c = hpc->length(); // counts
    if (num_c != num_a) {
      Squawk("Expected %d point counts, not %d", num_a, num_c);
      return;
    }
    bool key;
    if (nhpp = vi->nhpp) {
      key = true;
      m = nhpp->size();
      //dv->numHistogramPtUncVars = m;
      for(i=tothpp=0; i<m; ++i) {
	tothpp += nhppi = (*nhpp)[i];
	if (nhppi < 1) {
	  Squawk("num_pairs must be >= 1");
	  return;
	}
      }
      if (num_a != tothpp) {
	Squawk("Expected %d point abscissas, not %d", tothpp, num_a);
	return;
      }
    }
    else {
      key = false;
      m = dv->numHistogramPtUncVars;
      if (num_a % m) {
	Squawk("Number of abscissas (%d) not evenly divisible by number of variables (%d); Use num_pairs for unequal apportionment", num_a, m);
	return;
      }
      else
	avg_nhppi = num_a / m;
    }
    hpp = &dv->histogramUncPointPairs;
    hpp->resize(m);
    for (i=cntr=0; i<m; ++i) {
      nhppi = (key) ? (*nhpp)[i] : avg_nhppi;
      hppi  = &((*hpp)[i]);
      hppi->sizeUninitialized(2*nhppi);
      count_sum = 0.;
      for (j=0; j<nhppi; ++j, ++cntr) {
	Real x = (*hpa)[cntr]; // abscissas
	Real y = (*hpc)[cntr]; // counts
	if (j<nhppi-1 && x >= (*hpa)[cntr+1]) {
	  Squawk("histogram point x values must increase");
	  return;
	}
	if (y <= 0.) {
	  Squawk("nonpositive intermediate histogram point y value");
	  return;
	}
	(*hppi)[2*j]   = x; // abscissa
	(*hppi)[2*j+1] = y; // count
	count_sum += y;
      }
      // normalize counts to sum to 1
      for (j=0; j<nhppi; ++j)
	(*hppi)[2*j+1] /= count_sum;
    }
  }
}

static void Vgen_HistogramPtUnc(DataVariablesRep *dv, size_t offset)
{
  RealVector *L, *U, *V, *r, *IP;
  RealVectorArray *A;
  Real mean, stdev;

  L = &dv->discreteRealAleatoryUncLowerBnds;
  U = &dv->discreteRealAleatoryUncUpperBnds;
  V = &dv->discreteRealAleatoryUncVars;
  A = &dv->histogramUncPointPairs; IP = &dv->histogramPointUncVars;
  size_t i, j, k, last, n = dv->numHistogramPtUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    r = &((*A)[j]);
    last = r->length() - 2;
    (*L)[i] = (*r)[0];
    (*U)[i] = (*r)[last];
    if (num_IP) {
      if      ((*IP)[j] < (*L)[i]) (*V)[i] =  (*L)[i];
      else if ((*IP)[j] > (*U)[i]) (*V)[i] =  (*U)[i];
      else                         (*V)[i] = (*IP)[j];
    }
    else {
      Pecos::moments_from_histogram_pt_params(*r, mean, stdev);
      for(k = 0; k < last && (*r)[k+2] <= mean; k += 2); // bracket mean
      if (k < last && mean - (*r)[k] > (*r)[k+2] - mean) // find closest
	k += 2;
      (*V)[i] = (*r)[k];
    }
  }
}

static void 
Vchk_ContinuousIntervalUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t i, j, k, m, num_p = 0, num_lb, num_ub;
  IntArray *nI;
  int tot_nI, nIi, avg_nI;
  Real lb, lbj, ub, ubj, default_p;
  RealVector *LBi, *UBi, *Ilb, *Iub, *Ip, *Pi;
  RealVectorArray *LB, *UB, *P;

  if ((Ilb = vi->CIlb) && (Iub = vi->CIub)) {
    num_lb = Ilb->length(); // interval lower_bounds
    num_ub = Iub->length(); // interval upper_bounds

    // error check on array lengths; bounds are reqd, probs are optional
    if ((Ip = vi->CIp)) {
      num_p = Ip->length(); // interval_probs
      if (num_lb != num_p || num_ub != num_p) {
	Squawk("Expected as many lower bounds (%d) and upper bounds (%d) as probabilities (%d)", num_lb, num_ub, num_p);
	return;
      }
    }
    else if (num_lb != num_ub) {
      Squawk("Expected as many lower bounds (%d) as upper bounds (%d)", num_lb, num_ub);
      return;
    }

    // define apportionment
    bool key;
    if (nI = vi->nCI) {
      key = true;
      m = nI->size();
      if (m != dv->numContinuousIntervalUncVars) {
	Squawk("Expected %d numbers for num_intervals, but got %d",
	       dv->numContinuousIntervalUncVars, m);
	return;
      }
      for(i=tot_nI=0; i<m; ++i) {
	tot_nI += nIi = (*nI)[i];
	if (nIi < 1) {
	  Squawk("num_intervals values should be positive");
	  return;
	}
      }
      if ( (num_p && wronglen(tot_nI,  Ip, "interval_probs") ) ||
	   wronglen(tot_nI, Ilb, "interval lower_bounds") ||
	   wronglen(tot_nI, Iub, "interval upper_bounds"))
	return;
    }
    else {
      key = false;
      m = dv->numContinuousIntervalUncVars;
      if (num_lb % m) {
	Squawk("Number of bounds (%d) not evenly divisible by number of variables (%d); Use num_intervals for unequal apportionment", num_lb, m);
	return;
      }
      else
	avg_nI = num_lb / m;
    }
    LB = &dv->continuousIntervalUncLowerBounds; LB->resize(m);
    UB = &dv->continuousIntervalUncUpperBounds; UB->resize(m);
    P  = &dv->continuousIntervalUncBasicProbs;   P->resize(m);
    for(i = k = 0; i < m; ++i) {
      nIi = (key) ? (*nI)[i] : avg_nI;
      LBi = &((*LB)[i]); LBi->sizeUninitialized(nIi);
      UBi = &((*UB)[i]); UBi->sizeUninitialized(nIi);
      Pi  = &((*P)[i]);   Pi->sizeUninitialized(nIi);
      ub = -(lb = DBL_MAX);
      if (!num_p) default_p = 1./nIi; // default = equal p per cell
      for(j=0; j<nIi; ++j, ++k) {
	(*Pi)[j]  = (num_p) ? (*Ip)[k] : default_p;
	(*LBi)[j] = lbj = (*Ilb)[k];
	(*UBi)[j] = ubj = (*Iub)[k];
	if (lb > lbj) lb = lbj;
	if (ub < ubj) ub = ubj;
      }
      if (lb > ub)
	Squawk("Inconsistent interval uncertain bounds: %g > %g", lb, ub);
    }
  }
}

static void Vgen_ContinuousIntervalUnc(DataVariablesRep *dv, size_t offset)
{
  Real lb, lbk, ub, ubk, stdev;
  RealVector *ciLBj, *ciUBj, *ceuLB, *Pj, *ceuUB, *V, *IP;
  RealVectorArray *ciLB, *ciUB, *P;

  ceuLB = &dv->continuousEpistemicUncLowerBnds;
  ceuUB = &dv->continuousEpistemicUncUpperBnds;
  V     = &dv->continuousEpistemicUncVars;
  ciLB  = &dv->continuousIntervalUncLowerBounds;
  ciUB  = &dv->continuousIntervalUncUpperBounds;
  P     = &dv->continuousIntervalUncBasicProbs;
  IP    = &dv->continuousIntervalUncVars;
  size_t i, j, k, m, n = dv->numContinuousIntervalUncVars,
    num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    ciLBj = &((*ciLB)[j]); ciUBj = &((*ciUB)[j]); Pj = &((*P)[j]);
    m = Pj->length();
    lb = DBL_MAX; ub = -DBL_MAX;
    for (k=0; k<m; ++k) {
      lbk = (*ciLBj)[k]; ubk = (*ciUBj)[k];
      if (lb > lbk) lb = lbk;
      if (ub < ubk) ub = ubk;
    }
    (*ceuLB)[i] = lb; (*ceuUB)[i] = ub;
    if (num_IP) {
      if      ((*IP)[j] < lb) (*V)[i] = lb;
      else if ((*IP)[j] > ub) (*V)[i] = ub;
      else                    (*V)[i] = (*IP)[j];
    }
    else
      Pecos::moments_from_uniform_params(lb, ub, (*V)[i], stdev);
    // TO DO: if disjoint cells, repair V[i] to lie inside nearest cell
  }
}

static void 
Vchk_DiscreteIntervalUnc(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  size_t i, j, k, m, num_p = 0, num_lb, num_ub;
  IntArray *nI;
  int tot_nI, nIi, avg_nI, lb, lbj, ub, ubj;
  Real default_p;
  RealVector *Ip, *Pi;
  RealVectorArray *P;
  IntVector *LBi, *UBi, *Ilb, *Iub;
  IntVectorArray *LB, *UB;

  if ((Ilb = vi->DIlb) && (Iub = vi->DIub)) {
    num_lb = Ilb->length(); // interval lower_bounds
    num_ub = Iub->length(); // interval upper_bounds

    // error check on array lengths; bounds are reqd, probs are optional
    if ((Ip = vi->DIp)) {
      num_p = Ip->length(); // interval_probs
      if (num_lb != num_p || num_ub != num_p) {
	Squawk("Expected as many lower bounds (%d) and upper bounds (%d) as probabilities (%d)", num_lb, num_ub, num_p);
	return;
      }
    }
    else if (num_lb != num_ub) {
      Squawk("Expected as many lower bounds (%d) as upper bounds (%d)", num_lb, num_ub);
      return;
    }

    // define apportionment
    bool key;
    if (nI = vi->nDI) {
      key = true;
      m = nI->size();
      if (m != dv->numDiscreteIntervalUncVars) {
	Squawk("Expected %d numbers for num_intervals, but got %d",
	       dv->numDiscreteIntervalUncVars, m);
	return;
      }
      for(i=tot_nI=0; i<m; ++i) {
	tot_nI += nIi = (*nI)[i];
	if (nIi < 1) {
	  Squawk("num_intervals values should be positive");
	  return;
	}
      }
      if ( (num_p && wronglen(tot_nI,  Ip, "interval_probs") ) ||
	   wronglen(tot_nI, Ilb, "interval lower_bounds") ||
	   wronglen(tot_nI, Iub, "interval upper_bounds"))
	return;
    }
    else {
      key = false;
      m = dv->numDiscreteIntervalUncVars;
      if (num_lb % m) {
	Squawk("Number of bounds (%d) not evenly divisible by number of variables (%d); Use num_intervals for unequal apportionment", num_lb, m);
	return;
      }
      else
	avg_nI = num_lb / m;
    }
    LB = &dv->discreteIntervalUncLowerBounds; LB->resize(m);
    UB = &dv->discreteIntervalUncUpperBounds; UB->resize(m);
    P  = &dv->discreteIntervalUncBasicProbs;   P->resize(m);
    for(i = k = 0; i < m; ++i) {
      nIi = (key) ? (*nI)[i] : avg_nI;
      LBi = &((*LB)[i]); LBi->sizeUninitialized(nIi);
      UBi = &((*UB)[i]); UBi->sizeUninitialized(nIi);
      Pi  = &((*P)[i]);   Pi->sizeUninitialized(nIi);
      lb = INT_MAX; ub = INT_MIN;
      if (!num_p) default_p = 1./nIi; // default = equal p per cell
      for(j=0; j<nIi; ++j, ++k) {
	(*Pi)[j]  = (num_p) ? (*Ip)[k] : default_p;
	(*LBi)[j] = lbj = (*Ilb)[k];
	(*UBi)[j] = ubj = (*Iub)[k];
	if (lb > lbj) lb = lbj;
	if (ub < ubj) ub = ubj;
      }
      if (lb > ub)
	Squawk("Inconsistent interval uncertain bounds: %g > %g", lb, ub);
    }
  }
}

static void Vgen_DiscreteIntervalUnc(DataVariablesRep *dv, size_t offset)
{
  int lb, lbk, ub, ubk, stdev;
  RealVector *Pj;
  RealVectorArray *P;
  IntVector *diLBj, *diUBj, *deuLB, *deuUB, *V, *IP;
  IntVectorArray *diLB, *diUB;

  deuLB = &dv->discreteIntEpistemicUncLowerBnds;
  deuUB = &dv->discreteIntEpistemicUncUpperBnds;
  V     = &dv->discreteIntEpistemicUncVars;
  diLB  = &dv->discreteIntervalUncLowerBounds;
  diUB  = &dv->discreteIntervalUncUpperBounds;
  P     = &dv->discreteIntervalUncBasicProbs;
  IP    = &dv->discreteIntervalUncVars;
  size_t i, j, k, m, n = dv->numDiscreteIntervalUncVars, num_IP = IP->length();
  if (num_IP) dv->uncertainVarsInitPt = true;;

  for(i = offset, j = 0; j < n; ++i, ++j) {
    diLBj = &((*diLB)[j]); diUBj = &((*diUB)[j]); Pj = &((*P)[j]);
    m = Pj->length();
    ub = INT_MIN; lb = INT_MAX;
    for (k=0; k<m; ++k) {
      lbk = (*diLBj)[k]; ubk = (*diUBj)[k];
      if (lb > lbk) lb = lbk;
      if (ub < ubk) ub = ubk;
    }
    (*deuLB)[i] = lb; (*deuUB)[i] = ub;
    if (num_IP) {
      if      ((*IP)[j] < lb) (*V)[i] = lb;
      else if ((*IP)[j] > ub) (*V)[i] = ub;
      else                    (*V)[i] = (*IP)[j];
    }
    else
      (*V)[i] = (lb + ub) / 2; // int truncation if odd sum
    // TO DO: if disjoint cells, repair V[offset] to lie inside nearest cell
  }
}

static bool 
check_set_keys(size_t num_v, size_t ds_len, const char *kind,
	       IntArray *input_nds, int& avg_num_ds)
{
  // Process num_set_values key or define default allocation
  bool key = (input_nds);
  if (key) {
    if (input_nds->size() != num_v) {
      wrong_number("num_set_values value(s)", kind, num_v, input_nds->size());
      return key;
    }
    int num_ds_i, total_ds = 0;
    for (size_t i=0; i<num_v; ++i) {
      total_ds += num_ds_i = (*input_nds)[i];
      if (num_ds_i < 1)
	{ too_small(kind); return key; }
    }
    if (ds_len != total_ds)
      { wrong_number("set_values", kind, total_ds, ds_len); return key; }
  }
  else { // num_set_values is optional; use average len if no spec
    if (ds_len % num_v)
      { not_div(kind, ds_len, num_v); return key; }
    else
      avg_num_ds = ds_len / num_v;
  }
  return key;
}

static void 
Vchk_DIset(size_t num_v, const char *kind, IntArray *input_ndsi,
	   IntVector *input_dsi, IntSetArray& dsi_all, IntVector& dsi_init_pt)
{
  if (!input_dsi)
    return;

  int avg_num_dsi, ndup, dupval[2], num_dsi_i, val;
  size_t i, j, cntr, dsi_len = input_dsi->length();

  // Process num_set_values key or define default allocation
  bool key = check_set_keys(num_v, dsi_len, kind, input_ndsi, avg_num_dsi);

  // Insert values into the IntSetArray
  dsi_all.resize(num_v);
  for (i=cntr=ndup=0; i<num_v; ++i) {
    num_dsi_i = (key) ? (*input_ndsi)[i] : avg_num_dsi;
    IntSet& dsi_all_i = dsi_all[i];
    for (j=0; j<num_dsi_i; ++j, ++cntr) {
      val = (*input_dsi)[cntr];
      if (!dsi_all_i.insert(val).second) // insert returns pair<iterator,bool>
	if (++ndup <= 2) // warnings suppressed beyond two duplicates
	  dupval[ndup-1] = val;
    }
  }
  if (ndup)
    suppressed(kind, ndup, dupval, 0);

  // Checks on user-specified initial pt array
  if (!dsi_init_pt.empty()) {
    if (dsi_init_pt.length() != num_v)
      wrong_number("initial_point value(s)", kind, num_v, dsi_init_pt.length());
    else // check within admissible set for specified initial pt
      for (i=0; i<num_v; ++i) {
	IntSet& dsi_all_i = dsi_all[i]; val = dsi_init_pt[i];
	if (dsi_all_i.find(val) == dsi_all_i.end())
	  bad_initial_ivalue(kind, val);
      }
  }
  //else: default initialization performed in Vgen_DIset
}

static void 
Vchk_DIset(size_t num_v, const char *kind, IntArray *input_ndsi,
	   IntVector *input_dsi, RealVector *input_dsip,
	   IntRealMapArray& dsi_vals_probs, IntVector& dsi_init_pt)
{
  if (!input_dsi)
    return;

  int avg_num_dsi, ndup, dupval[2], num_dsi_i, val;
  size_t i, j, cntr, dsi_len = input_dsi->length(),
    num_p = (input_dsip) ? input_dsip->length() : 0;
  if (num_p && num_p != dsi_len)
    wrong_number("set_probabilities", kind, dsi_len, num_p);
  Real prob, default_p;

  // Process num_set_values key or define default allocation
  bool key = check_set_keys(num_v, dsi_len, kind, input_ndsi, avg_num_dsi);

  // Insert values into the IntRealMapArray
  dsi_vals_probs.resize(num_v);
  for (i=cntr=ndup=0; i<num_v; ++i) {
    IntRealMap& dsi_v_p_i = dsi_vals_probs[i];
    num_dsi_i = (key) ? (*input_ndsi)[i] : avg_num_dsi;
    if (!num_p) default_p = 1./num_dsi_i;
    for (j=0; j<num_dsi_i; ++j, ++cntr) {
      val  = (*input_dsi)[cntr];
      prob = (num_p) ? (*input_dsip)[cntr] : default_p;
      if (dsi_v_p_i.find(val) == dsi_v_p_i.end())
	dsi_v_p_i[val]  = prob; // insert new
      else {
	dsi_v_p_i[val] += prob; // add to existing
	if (++ndup <= 2) // warnings suppressed beyond two duplicates
	  dupval[ndup-1] = val;
      }
    }
  }
  if (ndup)
    suppressed(kind, ndup, dupval, 0);

  // Checks on user-specified initial pt array
  if (!dsi_init_pt.empty()) {
    if (dsi_init_pt.length() != num_v)
      wrong_number("initial_point value(s)", kind, num_v, dsi_init_pt.length());
    else // check within admissible set for specified initial pt
      for (i=0; i<num_v; ++i) {
	IntRealMap& dsi_v_p_i = dsi_vals_probs[i]; val = dsi_init_pt[i];
	if (dsi_v_p_i.find(val) == dsi_v_p_i.end())
	  bad_initial_ivalue(kind, val);
      }
  }
  //else: default initialization performed in Vgen_DIset
}

static void 
Vchk_DRset(size_t num_v, const char *kind, IntArray  *input_ndsr,
	   RealVector *input_dsr, RealSetArray& dsr_all,
	   RealVector& dsr_init_pt)
{
  if (!input_dsr)
    return;

  int avg_num_dsr, ndup, num_dsr_i;
  Real dupval[2], val;
  size_t i, j, cntr, dsr_len = input_dsr->length();

  // Process num_set_values key or define default allocation
  bool key = check_set_keys(num_v, dsr_len, kind, input_ndsr, avg_num_dsr);

  // Insert values into the RealSetArray
  dsr_all.resize(num_v);
  for (i=cntr=ndup=0; i<num_v; ++i) {
    num_dsr_i = (key) ? (*input_ndsr)[i] : avg_num_dsr;
    RealSet& dsr_all_i = dsr_all[i];
    for (j=0; j<num_dsr_i; ++j, ++cntr) {
      val = (*input_dsr)[cntr];
      if (!dsr_all_i.insert(val).second) // insert returns pair<iterator,bool>
	if (++ndup <= 2) // warnings suppressed beyond two duplicates
	  dupval[ndup-1] = val;
    }
  }
  if (ndup)
    suppressed(kind, ndup, 0, dupval);

  // Checks on user-specified initial pt array
  if (!dsr_init_pt.empty()) {
    if (dsr_init_pt.length() != num_v)
      wrong_number("initial_point value(s)", kind, num_v, dsr_init_pt.length());
    else
      for (i=0; i<num_v; ++i) {
	RealSet& dsr_all_i = dsr_all[i]; val = dsr_init_pt[i];
	if (dsr_all_i.find(val) == dsr_all_i.end())
	  bad_initial_rvalue(kind, val);
      }
  }
  //else: default initialization performed in Vgen_DRset
}

static void 
Vchk_DRset(size_t num_v, const char *kind, IntArray  *input_ndsr,
	   RealVector *input_dsr, RealVector* input_dsrp,
	   RealRealMapArray& dsr_vals_probs, RealVector& dsr_init_pt)
{
  if (!input_dsr)
    return;

  size_t i, j, cntr, dsr_len = input_dsr->length();
  int avg_num_dsr, ndup, num_dsr_i,
    num_p = (input_dsrp) ? input_dsrp->length() : 0;
  if (num_p && num_p != dsr_len)
    wrong_number("set_probabilities", kind, dsr_len, num_p);
  Real prob, default_p, dupval[2], val;

  // Process num_set_values key or define default allocation
  bool key = check_set_keys(num_v, dsr_len, kind, input_ndsr, avg_num_dsr);

  // Insert values into the RealRealMapArray
  dsr_vals_probs.resize(num_v);
  for (i=cntr=ndup=0; i<num_v; ++i) {
    RealRealMap& dsr_v_p_i = dsr_vals_probs[i];
    num_dsr_i = (key) ? (*input_ndsr)[i] : avg_num_dsr;
    if (!num_p) default_p = 1./num_dsr_i;
    for (j=0; j<num_dsr_i; ++j, ++cntr) {
      val  = (*input_dsr)[cntr];
      prob = (num_p) ? (*input_dsrp)[cntr] : default_p;
      if (dsr_v_p_i.find(val) == dsr_v_p_i.end())
	dsr_v_p_i[val]  = prob; // insert new
      else {
	dsr_v_p_i[val] += prob; // add to existing
	if (++ndup <= 2) // warnings suppressed beyond two duplicates
	  dupval[ndup-1] = val;
      }
    }
  }
  if (ndup)
    suppressed(kind, ndup, 0, dupval);

  // Checks on user-specified initial pt array
  if (!dsr_init_pt.empty()) {
    if (dsr_init_pt.length() != num_v)
      wrong_number("initial_point value(s)", kind, num_v, dsr_init_pt.length());
    else // check within admissible set for specified initial pt
      for (i=0; i<num_v; ++i) {
	RealRealMap& dsr_v_p_i = dsr_vals_probs[i]; val = dsr_init_pt[i];
	if (dsr_v_p_i.find(val) == dsr_v_p_i.end())
	  bad_initial_rvalue(kind, val);
      }
  }
  //else: default initialization performed in Vgen_DIset
}

static bool 
check_LUV_size(size_t num_v, IntVector& L, IntVector& U, IntVector& V,
	       bool aggregate_LUV, size_t offset)
{
  bool init_V = true;
  if (aggregate_LUV) {
    int max_index = offset + num_v - 1;
    if (max_index >= L.length() || max_index >= U.length() ||
	max_index >= V.length())
      Squawk("max index %d out of range for aggregate updates in Vgen_DIset",
	     max_index);
  }
  else {
    if (offset)
      Squawk("unexpected offset (%d) for non-aggregate mode in Vgen_DIset",
	     (int)offset);
    L.sizeUninitialized(num_v);
    U.sizeUninitialized(num_v);
    if (V.length() == num_v) // user spec --> already assigned by var_ivec()
      init_V = false;
    else
      V.sizeUninitialized(num_v);
  }
  return init_V;
}

static bool 
check_LUV_size(size_t num_v, RealVector& L, RealVector& U, RealVector& V,
	       bool aggregate_LUV, size_t offset)
{
  bool init_V = true;
  if (aggregate_LUV) {
    int max_index = offset + num_v - 1;
    if (max_index >= L.length() || max_index >= U.length() ||
	max_index >= V.length())
      Squawk("max index %d out of range for aggregate updates in Vgen_DRset",
	     max_index);
  }
  else {
    if (offset)
      Squawk("unexpected offset (%d) for non-aggregate mode in Vgen_DRset",
	     (int)offset);
    L.sizeUninitialized(num_v);
    U.sizeUninitialized(num_v);
    if (V.length() == num_v) // user spec --> already assigned by var_rvec()
      init_V = false;
    else
      V.sizeUninitialized(num_v);
  }
  return init_V;
}

static void 
Vgen_DIset(size_t num_v, IntSetArray& sets, IntVector& L, IntVector& U,
	   IntVector& V, bool aggregate_LUV = false, size_t offset = 0)
{
  ISCIter ie, it;
  Real avg_val, r_val;
  int i, i_val, i_left, i_right;
  size_t num_set_i;

  bool init_V = check_LUV_size(num_v, L, U, V, aggregate_LUV, offset);

  for(i = 0; i < num_v; ++i, ++offset) {
    IntSet& set_i = sets[i];
    it = set_i.begin(); ie = set_i.end(); num_set_i = set_i.size();
    if (num_set_i == 0) // should not occur
      L[offset] = U[offset] = V[offset] = 0;
    else if (num_set_i == 1)
      L[offset] = U[offset] = V[offset] = *it;
    else {
      L[offset] = *it;     // lower bound is first value
      U[offset] = *(--ie); // upper bound is final value
      if (init_V) {
	// select the initial value to be closest set value to avg_val
	for(avg_val = 0., ++ie; it != ie; ++it)
	  avg_val += *it;
	avg_val /= num_set_i;
	// bracket avg_val between [i_left,i_right]
	i_right = INT_MAX; i_left = INT_MIN;
	for(it = set_i.begin(); it != ie; ++it) {
	  r_val = i_val = *it;
	  if (r_val > avg_val) {      // update nearest neighbor to right
	    if (i_val < i_right)
	      i_right = i_val;
	  }
	  else if (r_val < avg_val) { // update nearest neighbor to left
	    if (i_val > i_left)
	      i_left = i_val;
	  }
	  else { // r_val equals avg_val
	    i_left = i_right = i_val;
	    break;
	  }
	}
	V[offset] = (i_right - avg_val < avg_val - i_left) ? i_right : i_left;
      }
    }
  }
}

static void 
Vgen_DIset(size_t num_v, IntRealMapArray& vals_probs, IntVector& IP,
	   IntVector& L, IntVector& U, IntVector& V,
	   bool aggregate_LUV = false, size_t offset = 0)
{
  IRMCIter ite, it;
  Real avg_val, r_val;
  int i_val, i_left, i_right;
  size_t i, j, num_vp_j, num_IP = IP.length();

  bool init_V = check_LUV_size(num_v, L, U, V, aggregate_LUV, offset);

  for(i = offset, j = 0; j < num_v; ++i, ++j) {
    IntRealMap& vp_j = vals_probs[j];
    it = vp_j.begin(); ite = vp_j.end(); num_vp_j = vp_j.size();
    if (num_vp_j == 0) { // should not occur
      L[i] = U[i] = 0;
      V[i] = (num_IP) ? IP[j] : 0;
    }
    else if (num_vp_j == 1) {
      L[i] = U[i] = it->first;
      V[i] = (num_IP) ? IP[j] : it->first;
    }
    else {
      L[i] = it->first;      // lower bound is first value
      U[i] = (--ite)->first; // upper bound is final value
      if (num_IP) V[i] = IP[j]; // presence of value w/i set already checked
      else if (init_V) {
	// select the initial value to be closest set value to avg_val
	for(avg_val = 0., ++ite; it != ite; ++it)
	  avg_val += it->first;
	avg_val /= num_vp_j;
	// bracket avg_val between [i_left,i_right]
	i_right = INT_MAX; i_left = INT_MIN;
	for(it = vp_j.begin(); it != ite; ++it) {
	  r_val = i_val = it->first;
	  if (r_val > avg_val) {      // update nearest neighbor to right
	    if (i_val < i_right)
	      i_right = i_val;
	  }
	  else if (r_val < avg_val) { // update nearest neighbor to left
	    if (i_val > i_left)
	      i_left = i_val;
	  }
	  else { // r_val equals avg_val
	    i_left = i_right = i_val;
	    break;
	  }
	}
	V[i] = (i_right - avg_val < avg_val - i_left) ? i_right : i_left;
      }
    }
  }
}

static void 
Vgen_DRset(size_t num_v, RealSetArray& sets, RealVector& L, RealVector& U,
	   RealVector& V, bool aggregate_LUV = false, size_t offset = 0)
{
  Real avg_val, set_val, s_left, s_right;
  RSCIter ie, it;
  int i;
  size_t num_set_i;

  bool init_V = check_LUV_size(num_v, L, U, V, aggregate_LUV, offset);

  for(i = 0; i < num_v; ++i, ++offset) {
    RealSet& set_i = sets[i];
    it = set_i.begin(); ie = set_i.end(); num_set_i = set_i.size();
    if (num_set_i == 0) // should not occur
      L[offset] = U[offset] = V[offset] = 0.;
    else if (num_set_i == 1)
      L[offset] = U[offset] = V[offset] = *it;
    else {
      L[offset] = *it;     // lower bound is first value
      U[offset] = *(--ie); // upper bound is final value
      if (init_V) {
	// select the initial value to be closest set value to avg_val
	for(avg_val = 0., ++ie; it != ie; ++it)
	  avg_val += *it;
	avg_val /= num_set_i;
	// bracket avg_val between [s_left,s_right]
	s_right = DBL_MAX; s_left = -DBL_MAX;
	for(it = set_i.begin(); it != ie; ++it) {
	  set_val = *it;
	  if (set_val > avg_val) {      // update nearest neighbor to right
	    if (set_val < s_right)
	      s_right = set_val;
	  }
	  else if (set_val < avg_val) { // update nearest neighbor to left
	    if (set_val > s_left)
	      s_left = set_val;
	  }
	  else { // set_val equals avg_val
	    s_left = s_right = set_val;
	    break;
	  }
	}
	V[offset] = (s_right - avg_val < avg_val - s_left) ? s_right : s_left;
      }
    }
  }
}

static void 
Vgen_DRset(size_t num_v, RealRealMapArray& vals_probs, RealVector& IP,
	   RealVector& L, RealVector& U, RealVector& V,
	   bool aggregate_LUV = false, size_t offset = 0)
{
  Real avg_val, set_val, s_left, s_right;
  RRMCIter ite, it;
  size_t i, j, num_vp_j, num_IP = IP.length();

  bool init_V = check_LUV_size(num_v, L, U, V, aggregate_LUV, offset);

  for(i = offset, j = 0; j < num_v; ++i, ++j) {
    RealRealMap& vp_j = vals_probs[j];
    it = vp_j.begin(); ite = vp_j.end(); num_vp_j = vp_j.size();
    if (num_vp_j == 0) { // should not occur
      L[i] = U[i] = V[i] = 0.;
      V[i] = (num_IP) ? IP[j] : 0.;
    }
    else if (num_vp_j == 1) {
      L[i] = U[i] = it->first;
      V[i] = (num_IP) ? IP[j] : it->first;
    }
    else {
      L[i] = it->first;     // lower bound is first value
      U[i] = (--ite)->first; // upper bound is final value
      if (num_IP) V[i] = IP[j];
      else if (init_V) {
	// select the initial value to be closest set value to avg_val
	for(avg_val = 0., ++ite; it != ite; ++it)
	  avg_val += it->first;
	avg_val /= num_vp_j;
	// bracket avg_val between [s_left,s_right]
	s_right = DBL_MAX; s_left = -DBL_MAX;
	for(it = vp_j.begin(); it != ite; ++it) {
	  set_val = it->first;
	  if (set_val > avg_val) {      // update nearest neighbor to right
	    if (set_val < s_right)
	      s_right = set_val;
	  }
	  else if (set_val < avg_val) { // update nearest neighbor to left
	    if (set_val > s_left)
	      s_left = set_val;
	  }
	  else { // set_val equals avg_val
	    s_left = s_right = set_val;
	    break;
	  }
	}
	V[i] = (s_right - avg_val < avg_val - s_left) ? s_right : s_left;
      }
    }
  }
}

static void 
Vchk_DiscreteDesSetInt(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  static char kind[] = "discrete_design_set_integer";
  Vchk_DIset(dv->numDiscreteDesSetIntVars, kind, vi->nddsi, vi->ddsi,
	     dv->discreteDesignSetInt, dv->discreteDesignSetIntVars);
}

static void Vgen_DiscreteDesSetInt(DataVariablesRep *dv, size_t offset)
{
  Vgen_DIset(dv->numDiscreteDesSetIntVars, dv->discreteDesignSetInt,
	     dv->discreteDesignSetIntLowerBnds,
	     dv->discreteDesignSetIntUpperBnds,
	     dv->discreteDesignSetIntVars); // no offset, not aggregate L/U/V
}

static void 
Vchk_DiscreteDesSetReal(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  static char kind[] = "discrete_design_set_real";
  Vchk_DRset(dv->numDiscreteDesSetRealVars, kind, vi->nddsr, vi->ddsr,
	     dv->discreteDesignSetReal, dv->discreteDesignSetRealVars);
}

static void Vgen_DiscreteDesSetReal(DataVariablesRep *dv, size_t offset)
{
  Vgen_DRset(dv->numDiscreteDesSetRealVars, dv->discreteDesignSetReal,
	     dv->discreteDesignSetRealLowerBnds,
	     dv->discreteDesignSetRealUpperBnds,
	     dv->discreteDesignSetRealVars); // no offset, not aggregate L/U/V
}

static void 
Vchk_DiscreteUncSetInt(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  static char kind[] = "discrete_uncertain_set_integer";
  Vchk_DIset(dv->numDiscreteUncSetIntVars, kind, vi->ndusi, vi->dusi, vi->DSIp,
	     dv->discreteUncSetIntValuesProbs, dv->discreteUncSetIntVars);
}

static void Vgen_DiscreteUncSetInt(DataVariablesRep *dv, size_t offset)
{
  Vgen_DIset(dv->numDiscreteUncSetIntVars, dv->discreteUncSetIntValuesProbs,
	     dv->discreteUncSetIntVars, dv->discreteIntEpistemicUncLowerBnds,
	     dv->discreteIntEpistemicUncUpperBnds,
	     dv->discreteIntEpistemicUncVars, true, offset);
  if (dv->discreteUncSetIntVars.length()) dv->uncertainVarsInitPt = true;
}

static void 
Vchk_DiscreteUncSetReal(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  static char kind[] = "discrete_uncertain_set_real";
  Vchk_DRset(dv->numDiscreteUncSetRealVars, kind, vi->ndusr, vi->dusr, vi->DSRp,
	     dv->discreteUncSetRealValuesProbs, dv->discreteUncSetRealVars);
}

static void Vgen_DiscreteUncSetReal(DataVariablesRep *dv, size_t offset)
{
  Vgen_DRset(dv->numDiscreteUncSetRealVars, dv->discreteUncSetRealValuesProbs,
	     dv->discreteUncSetRealVars, dv->discreteRealEpistemicUncLowerBnds,
	     dv->discreteRealEpistemicUncUpperBnds,
	     dv->discreteRealEpistemicUncVars, true, offset);
  if (dv->discreteUncSetRealVars.length()) dv->uncertainVarsInitPt = true;
}

static void 
Vchk_DiscreteStateSetInt(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  static char kind[] = "discrete_state_set_integer";
  Vchk_DIset(dv->numDiscreteStateSetIntVars, kind, vi->ndssi, vi->dssi, 
	     dv->discreteStateSetInt, dv->discreteStateSetIntVars);
}

static void Vgen_DiscreteStateSetInt(DataVariablesRep *dv, size_t offset)
{
  Vgen_DIset(dv->numDiscreteStateSetIntVars, dv->discreteStateSetInt,
	     dv->discreteStateSetIntLowerBnds,
	     dv->discreteStateSetIntUpperBnds,
	     dv->discreteStateSetIntVars); // no offset, not aggregate L/U/V
}

static void 
Vchk_DiscreteStateSetReal(DataVariablesRep *dv, size_t offset, Var_Info *vi)
{
  static char kind[] = "discrete_state_set_real";
  Vchk_DRset(dv->numDiscreteStateSetRealVars, kind, vi->ndssr, vi->dssr,
	     dv->discreteStateSetReal, dv->discreteStateSetRealVars);
}

static void Vgen_DiscreteStateSetReal(DataVariablesRep *dv, size_t offset)
{
  Vgen_DRset(dv->numDiscreteStateSetRealVars, dv->discreteStateSetReal,
	     dv->discreteStateSetRealLowerBnds,
	     dv->discreteStateSetRealUpperBnds,
	     dv->discreteStateSetRealVars); // no offset, not aggregate L/U/V
}

static const char *
Var_Name(StringArray *sa, char *buf, size_t i)
{
  if (sa)
    return (*sa)[i].data();
  std::sprintf(buf,"%lu", (UL)(i+1));
  return (const char*)buf;
}

static void Var_boundchk(DataVariablesRep *dv, Var_rcheck *b)
{
  RealVector *L, *U, *V;
  StringArray *sa;
  char namebuf[32];
  int i, n, ndflt; // length() values here are int rather than size_t

  if ((n = dv->*b->n) == 0)
    return;
  ndflt = -1;
  if (b->L) {
    ndflt = 0;
    L = &(dv->*b->L);
    if (L->length() == 0)
      ++ndflt;
    else if (L->length() != n) {
      Squawk("%s_lower_bounds needs %lu elements, not %lu",
	     b->name, (UL)n, (UL)L->length());
      return;
    }
    U = &(dv->*b->U);
    if (U->length() == 0)
      ++ndflt;
    else if (U->length() != n) {
      Squawk("%s_upper_bounds needs %lu elements, not %lu",
	     b->name, (UL)n, (UL)L->length());
      return;
    }
  }
  sa = 0;
  if (b->Lbl) {
    sa = &(dv->*b->Lbl);
    if (sa->size() == 0)
      sa = 0;
  }
  if (ndflt == 0)
    for(i = 0; i < n; i++) {
      if ((*L)[i] > (*U)[i])
	Squawk("lower bound for %s variable %s exceeds upper bound",
	       b->name, Var_Name(sa,namebuf,i));
    }
  if (b->V == 0)
    return;
  V = &(dv->*b->V);
  if (V->length() == 0)
    return;
  if (V->length() != n) {
    Squawk("initial point for %s needs %lu elements, not %lu",
	   b->name, (UL)n, (UL)V->length());
    return;
  }
  if (L->length() > 0) {
    for(i = 0; i < n; i++)
      if ((*V)[i] < (*L)[i]) {
	Warn("Setting initial_value for %s variable %s to its lower bound",
	     b->name, Var_Name(sa,namebuf,i));
	(*V)[i] = (*L)[i];
      }
  }
  if (U->length() > 0) {
    for(i = 0; i < n; i++)
      if ((*V)[i] > (*U)[i]) {
	Warn("Setting initial_value for %s variable %s to its upper bound",
	     b->name, Var_Name(sa,namebuf,i));
	(*V)[i] = (*U)[i];
      }
  }
}

static void Var_iboundchk(DataVariablesRep *dv, Var_icheck *ib)
{
  IntVector *L, *U, *V;
  StringArray *sa;
  char namebuf[32];
  int i, n, ndflt; // length() values here are int rather than size_t

  if ((n = dv->*ib->n) == 0)
    return;
  L = &(dv->*ib->L);
  ndflt = 0;
  if (L->length() == 0)
    ++ndflt;
  else if (L->length() != n) {
    Squawk("%s_lower_bounds needs %lu elements, not %lu",
	   ib->name, (UL)n, (UL)L->length());
    return;
  }
  U = &(dv->*ib->U);
  if (U->length() == 0)
    ++ndflt;
  else if (U->length() != n) {
    Squawk("%s_upper_bounds needs %lu elements, not %lu",
	   ib->name, (UL)n, (UL)L->length());
    return;
  }
  sa = 0;
  if (ib->Lbl) {
    sa = &(dv->*ib->Lbl);
    if (sa->size() == 0)
      sa = 0;
  }
  if (ndflt == 0)
    for(i = 0; i < n; i++) {
      if ((*L)[i] > (*U)[i])
	Squawk("lower bound for %s variable %s exceeds upper bound",
	       ib->name, Var_Name(sa,namebuf,i));
    }
  if (ib->V == 0)
    return;	// won't happen for discrete variables
  V = &(dv->*ib->V);
  if (V->length() == 0)
    return;
  if (V->length() != n) {
    Squawk("initial point for %s needs %lu elements, not %lu",
	   ib->name, (UL)n, (UL)V->length());
    return;
  }
  if (L->length() > 0) {
    for(i = 0; i < n; i++)
      if ((*V)[i] < (*L)[i]) {
	Warn("Setting initial_value for %s variable %s to its lower bound",
	     ib->name, Var_Name(sa,namebuf,i));
	(*V)[i] = (*L)[i];
      }
  }
  if (U->length() > 0) {
    for(i = 0; i < n; i++)
      if ((*V)[i] > (*U)[i]) {
	Warn("Setting initial_value for %s variable %s to its upper bound",
	     ib->name, Var_Name(sa,namebuf,i));
	(*V)[i] = (*U)[i];
      }
  }
}

#define VarLabelInfo(a,b)     { #a, #b, &DataVariablesRep::num##b##Vars, Vchk_##b }
static Var_uinfo CAUVLbl[CAUVar_Nkinds] = {
  VarLabelInfo(nuv_, NormalUnc),
  VarLabelInfo(lnuv_, LognormalUnc),
  VarLabelInfo(uuv_, UniformUnc),
  VarLabelInfo(luuv_, LoguniformUnc),
  VarLabelInfo(tuv_, TriangularUnc),
  VarLabelInfo(euv_, ExponentialUnc),
  VarLabelInfo(beuv_, BetaUnc),
  VarLabelInfo(gauv_, GammaUnc),
  VarLabelInfo(guuv_, GumbelUnc),
  VarLabelInfo(fuv_, FrechetUnc),
  VarLabelInfo(wuv_, WeibullUnc),
  VarLabelInfo(hbuv_, HistogramBinUnc)
};
static Var_uinfo DAUIVLbl[DAUIVar_Nkinds] = {
  VarLabelInfo(puv_, PoissonUnc),
  VarLabelInfo(biuv_, BinomialUnc),
  VarLabelInfo(nbuv_, NegBinomialUnc),
  VarLabelInfo(geuv_, GeometricUnc),
  VarLabelInfo(hguv_, HyperGeomUnc)
};
static Var_uinfo DAURVLbl[DAURVar_Nkinds] = {
  VarLabelInfo(hpuv_, HistogramPtUnc)
};
static Var_uinfo CEUVLbl[CEUVar_Nkinds] = {
  VarLabelInfo(ciuv_, ContinuousIntervalUnc)
};
static Var_uinfo DEUIVLbl[DEUIVar_Nkinds] = {
  VarLabelInfo(diuv_, DiscreteIntervalUnc),
  VarLabelInfo(dusiv_, DiscreteUncSetInt)
};
static Var_uinfo DEURVLbl[DEURVar_Nkinds] = {
  VarLabelInfo(dusrv_, DiscreteUncSetReal)
};
static Var_uinfo DiscSetLbl[DiscSetVar_Nkinds] = {
  VarLabelInfo(ddsiv_, DiscreteDesSetInt),
  VarLabelInfo(ddsrv_, DiscreteDesSetReal),
  VarLabelInfo(dssiv_, DiscreteStateSetInt),
  VarLabelInfo(dssrv_, DiscreteStateSetReal)
};
#undef VarLabelInfo

void NIDRProblemDescDB::
var_stop(const char *keyname, Values *val, void **g, void *v)
{
  Var_Info *vi = *(Var_Info**)g;
  DataVariablesRep *dv = vi->dv;
  scale_chk(dv->continuousDesignScaleTypes, dv->continuousDesignScales,
	    "cdv", aln_scaletypes);
  pDDBInstance->VIL.push_back(vi);
  pDDBInstance->dataVariablesList.push_back(*vi->dv_handle);
  delete vi->dv_handle;
}

struct VarLabelChk {
  size_t DataVariablesRep::* n;
  StringArray DataVariablesRep::* sa;
  const char *stub;
  const char *name;
};

#define AVI &DataVariablesRep::
static VarLabelChk Vlch[] = {
  { AVI numContinuousDesVars, AVI continuousDesignLabels, "cdv_", "cdv_descriptors" },
  { AVI numDiscreteDesRangeVars, AVI discreteDesignRangeLabels, "ddriv_", "ddriv_descriptors" },
  { AVI numDiscreteDesSetIntVars, AVI discreteDesignSetIntLabels, "ddsiv_", "ddsiv_descriptors" },
  { AVI numDiscreteDesSetRealVars, AVI discreteDesignSetRealLabels, "ddsrv_", "ddsrv_descriptors" },
  { AVI numContinuousStateVars, AVI continuousStateLabels, "csv_", "csv_descriptors" },
  { AVI numDiscreteStateRangeVars, AVI discreteStateRangeLabels, "dsriv_", "dsriv_descriptors" },
  { AVI numDiscreteStateSetIntVars, AVI discreteStateSetIntLabels, "dssiv_", "dssiv_descriptors" },
  { AVI numDiscreteStateSetRealVars, AVI discreteStateSetRealLabels, "dssrv_", "dssrv_descriptors" },
  { AVI numContinuousDesVars, AVI continuousDesignScaleTypes, 0, "cdv_scale_types" }
};
#undef  AVI

struct VLreal {
  int n;
  VarLabel Var_Info::* VL; // should be "VarLabel *Var_Info::* VL"
  // but g++ is buggy (versions 4.3.1, 4.4.2 anyway)
  Var_uinfo *vui;
  StringArray DataVariablesRep::* Labels;
  RealVector DataVariablesRep::* LowerBnds;
  RealVector DataVariablesRep::* UpperBnds;
  RealVector DataVariablesRep::* UncVars;
};

struct VLint {
  int n;
  VarLabel Var_Info::* VL; // should be "VarLabel *Var_Info::* VL"
  // but g++ is buggy (versions 4.3.1, 4.4.2 anyway)
  Var_uinfo *vui;
  StringArray DataVariablesRep::* Labels;
  IntVector DataVariablesRep::* LowerBnds;
  IntVector DataVariablesRep::* UpperBnds;
  IntVector DataVariablesRep::* UncVars;
};

enum { N_VLR = 4 };
enum { N_VLI = 2 };

#define AVI (VarLabel Var_Info::*) &Var_Info::	// cast to bypass g++ bug
#define DVR &DataVariablesRep::

static VLreal VLR[N_VLR] = {
 {CAUVar_Nkinds,  AVI CAUv,  CAUVLbl,
	DVR continuousAleatoryUncLabels,
	DVR continuousAleatoryUncLowerBnds,
	DVR continuousAleatoryUncUpperBnds,
	DVR continuousAleatoryUncVars},
 {CEUVar_Nkinds,  AVI CEUv,  CEUVLbl,
	DVR continuousEpistemicUncLabels,
	DVR continuousEpistemicUncLowerBnds,
	DVR continuousEpistemicUncUpperBnds,
	DVR continuousEpistemicUncVars},
 {DAURVar_Nkinds, AVI DAURv, DAURVLbl,
	DVR discreteRealAleatoryUncLabels,
	DVR discreteRealAleatoryUncLowerBnds,
	DVR discreteRealAleatoryUncUpperBnds,
        DVR discreteRealAleatoryUncVars},
 {DEURVar_Nkinds, AVI DEURv, DEURVLbl,
	DVR discreteRealEpistemicUncLabels,
	DVR discreteRealEpistemicUncLowerBnds,
	DVR discreteRealEpistemicUncUpperBnds,
	DVR discreteRealEpistemicUncVars}};

static VLint VLI[N_VLI] = {
 {DAUIVar_Nkinds, AVI DAUIv, DAUIVLbl,
	DVR discreteIntAleatoryUncLabels,
        DVR discreteIntAleatoryUncLowerBnds,
        DVR discreteIntAleatoryUncUpperBnds,
        DVR discreteIntAleatoryUncVars},
 {DEUIVar_Nkinds, AVI DEUIv, DEUIVLbl,
	DVR discreteIntEpistemicUncLabels,
	DVR discreteIntEpistemicUncLowerBnds,
	DVR discreteIntEpistemicUncUpperBnds,
	DVR discreteIntEpistemicUncVars}};

//#undef RDVR
#undef	DVR
#undef  AVI

static int VLR_aleatory[N_VLR] = { 1, 0, 1, 0 };
static int VLI_aleatory[N_VLI] = { 1, 0 };

#define Vchk_3(x,y) {#x,&DataVariablesRep::num##y##Vars,Vgen_##y}
// Some compilers in debug mode (MSVC) don't initialize the trailing
// two entries to NULL, so be explicit:
#define Vchk_5(x,y,z) {#x,&DataVariablesRep::num##y##Vars,Vgen_##y,&DataVariablesRep::z##LowerBnds,&DataVariablesRep::z##UpperBnds,NULL,NULL}
#define Vchk_7(x,y,z) {#x,&DataVariablesRep::num##y##Vars,Vgen_##y,&DataVariablesRep::z##LowerBnds,&DataVariablesRep::z##UpperBnds,&DataVariablesRep::z##Vars,&DataVariablesRep::z##Labels}

// Trailing pointers in these initialization lists will be NULL. From C++ 2003:
//   "Objects with static storage shall be zero-initialized before any other
//    initialization takes place."

// These are used within make_variable_defaults(): Vgen_##y is applied
static Var_check
  var_mp_check_cv[] = {
	Vchk_3(continuous_design,ContinuousDes),
	Vchk_3(continuous_state,ContinuousState) },
  var_mp_check_dset[] = {
	Vchk_3(discrete_design_set_integer,DiscreteDesSetInt),
	Vchk_3(discrete_design_set_real,DiscreteDesSetReal),
	Vchk_3(discrete_state_set_integer,DiscreteStateSetInt),
	Vchk_3(discrete_state_set_real,DiscreteStateSetReal) },
  var_mp_check_cau[] = {
	Vchk_3(normal_uncertain,NormalUnc),
	Vchk_3(lognormal_uncertain,LognormalUnc),
	Vchk_3(uniform_uncertain,UniformUnc),
	Vchk_3(loguniform_uncertain,LoguniformUnc),
	Vchk_3(triangular_uncertain,TriangularUnc),
	Vchk_3(exponential_uncertain,ExponentialUnc),
	Vchk_3(beta_uncertain,BetaUnc),
	Vchk_3(gamma_uncertain,GammaUnc),
	Vchk_3(gumbel_uncertain,GumbelUnc),
	Vchk_3(frechet_uncertain,FrechetUnc),
	Vchk_3(weibull_uncertain,WeibullUnc),
	Vchk_3(histogram_bin_uncertain,HistogramBinUnc) },
  var_mp_check_daui[] = {
	Vchk_3(poisson_uncertain,PoissonUnc),
	Vchk_3(binomial_uncertain,BinomialUnc),
	Vchk_3(negative_binomial_uncertain,NegBinomialUnc),
	Vchk_3(geometric_uncertain,GeometricUnc),
	Vchk_3(hypergeometric_uncertain,HyperGeomUnc) },
  var_mp_check_daur[] = {
	Vchk_3(histogram_point_uncertain,HistogramPtUnc) },
  var_mp_check_ceu[] = {
	Vchk_3(continuous_interval_uncertain,ContinuousIntervalUnc) },
  var_mp_check_deui[] = {
	Vchk_3(discrete_interval_uncertain,DiscreteIntervalUnc),
	Vchk_3(discrete_uncertain_set_integer,DiscreteUncSetInt) },
  var_mp_check_deur[] = {
	Vchk_3(discrete_uncertain_set_real,DiscreteUncSetReal) };

// This is used within check_variables_node(): Var_boundchk() is applied
static Var_rcheck
  var_mp_cbound[] = {
	Vchk_7(continuous_design,ContinuousDes,continuousDesign),
	Vchk_7(continuous_state,ContinuousState,continuousState),
	Vchk_5(normal_uncertain,NormalUnc,normalUnc),
	Vchk_5(lognormal_uncertain,LognormalUnc,lognormalUnc),
	Vchk_5(uniform_uncertain,UniformUnc,uniformUnc),
	Vchk_5(loguniform_uncertain,LoguniformUnc,loguniformUnc),
	Vchk_5(triangular_uncertain,TriangularUnc,triangularUnc),
	Vchk_5(beta_uncertain,BetaUnc,betaUnc) };

// This is used within check_variables_node(): Var_iboundchk() is applied
static Var_icheck
  var_mp_drange[] = {
	Vchk_7(discrete_design_range,DiscreteDesRange,discreteDesignRange),
	Vchk_7(discrete_state_range,DiscreteStateRange,discreteStateRange) };

#undef Vchk_7
#undef Vchk_5
#undef Vchk_3

void NIDRProblemDescDB::
make_variable_defaults(std::list<DataVariables>* dvl)
{
  DataVariablesRep *dv;
  IntVector *IL, *IU, *IV;
  RealVector *L, *U, *V;
  StringArray *sa;
  VLreal *vlr;
  VLint  *vli;
  VarLabel *vl;
  VarLabelChk *vlc, *vlce;
  Var_uinfo *vui, *vuie;
  char buf[32];
  size_t i, j, k, n, nu, nursave[N_VLR], nuisave[N_VLI];
  static const char Inconsistent_bounds[] =
    "Inconsistent bounds on %s uncertain variables";

  std::list<DataVariables>::iterator It = dvl->begin(), Ite = dvl->end();
  for(; It != Ite; ++It) {
    dv = It->dataVarsRep;
    for(k = 0; k < N_VLR; ++k) {
      vlr = &VLR[k];
      vui = vlr->vui;
      vuie = vui + vlr->n;
      for(nu = 0; vui < vuie; ++vui)
	nu += dv->*vui->n;
      nursave[k] = nu;
      if (!nu)
	continue;
      L = &(dv->*vlr->LowerBnds);
      U = &(dv->*vlr->UpperBnds);
      V = &(dv->*vlr->UncVars);
      L->sizeUninitialized(nu);
      U->sizeUninitialized(nu);
      V->sizeUninitialized(nu);
    }
    for(k = 0; k < N_VLI; ++k) {
      vli = &VLI[k];
      vui = vli->vui;
      vuie = vui + vli->n;
      for(nu = 0; vui < vuie; ++vui)
	nu += dv->*vui->n;
      nuisave[k] = nu;
      if (!nu)
	continue;
      IL = &(dv->*vli->LowerBnds);
      IU = &(dv->*vli->UpperBnds);
      IV = &(dv->*vli->UncVars);
      IL->sizeUninitialized(nu);
      IU->sizeUninitialized(nu);
      IV->sizeUninitialized(nu);
    }

    // inferred bound generation for continuous variable types

    // loop over cdv/csv
    Var_check *c, *ce;
    size_t offset = 0;
    for(c=var_mp_check_cv, ce = c + Numberof(var_mp_check_cv); c < ce; ++c)
      if ((n = dv->*c->n) > 0)
	(*c->vgen)(dv, offset); // offset not used
    // loop over nuv/lnuv/uuv/luuv/truv/euv/buv/gauv/guuv/fuv/wuv/hbuv
    for(c=var_mp_check_cau, ce=c + Numberof(var_mp_check_cau); c < ce; ++c)
      if ((n = dv->*c->n) > 0)
	{ (*c->vgen)(dv, offset); offset += n; }
    // continuous epistemic uncertain
    offset = 0; // reset for combined epistemic arrays
    for(c=var_mp_check_ceu, ce=c + Numberof(var_mp_check_ceu); c < ce; ++c)
      if ((n = dv->*c->n) > 0)
	{ (*c->vgen)(dv, offset); offset += n; }

    // inferred bound generation for discrete variable types

    // discrete design,state ranges
    Var_icheck *ic, *ice;
    offset = 0;
    for(ic=var_mp_drange, ice=ic + Numberof(var_mp_drange); ic<ice; ++ic)
      if ((n = dv->*ic->n) > 0)
	{ (*ic->vgen)(dv, offset); } // offset not used
    // discrete int aleatory uncertain use offset passed into Vgen_*Unc
    for(c=var_mp_check_daui, ce=c + Numberof(var_mp_check_daui); c<ce; ++c)
      if ((n = dv->*c->n) > 0)
	{ (*c->vgen)(dv, offset); offset += n; }
    // discrete real aleatory uncertain use offset passed into Vgen_*Unc
    offset = 0;
    for(c=var_mp_check_daur, ce=c + Numberof(var_mp_check_daur); c<ce; ++c)
      if ((n = dv->*c->n) > 0)
	{ (*c->vgen)(dv, offset); offset += n; }
    // discrete int epistemic uncertain use offset passed into Vgen_*Unc
    offset = 0;
    for(c=var_mp_check_deui, ce=c + Numberof(var_mp_check_deui); c<ce; ++c)
      if ((n = dv->*c->n) > 0)
	{ (*c->vgen)(dv, offset); offset += n; }
    // discrete real epistemic uncertain use offset passed into Vgen_*Unc
    offset = 0;
    for(c=var_mp_check_deur, ce=c + Numberof(var_mp_check_deur); c<ce; ++c)
      if ((n = dv->*c->n) > 0)
	{ (*c->vgen)(dv, offset); offset += n; }
    // these don't use an offset passed into Vgen_Discrete*Set*
    offset = 0;
    for(c=var_mp_check_dset, ce=c + Numberof(var_mp_check_dset); c<ce; ++c)
      if ((n = dv->*c->n) > 0)
	(*c->vgen)(dv, offset); // offset not used


    for(k = 0; k < N_VLR; ++k) {
      nu = nursave[k];
      if (!nu)
	continue;
      vlr = &VLR[k];
      vui = vlr->vui;
      vuie = vui + vlr->n;
      L = &(dv->*vlr->LowerBnds);
      U = &(dv->*vlr->UpperBnds);
      V = &(dv->*vlr->UncVars);
      sa = &(dv->*vlr->Labels);
      if (!sa->size())
	sa->resize(nu);
      i = 0;
      for(vui = vlr->vui; vui < vuie; ++vui) {
	if ((n = dv->*vui->n) == 0)
	  continue;
	for(j = 0; j < n; ++j)
	  if ((*L)[i+j] > (*U)[i+j]) {
	    squawk(Inconsistent_bounds, vui->vkind);
	    break;
	  }
	if ((*sa)[i] == "")
	  for(j = 1; j <= n; ++j) {
	    std::sprintf(buf, "%s%d", vui->lbl, (int)j);
	    (*sa)[i++] = buf;
	  }
	else
	  i += n;
      }
    }
    for(k = 0; k < N_VLI; ++k) {
      nu = nuisave[k];
      if (!nu)
	continue;
      vli = &VLI[k];
      vui = vli->vui;
      vuie = vui + vli->n;
      IL = &(dv->*vli->LowerBnds);
      IU = &(dv->*vli->UpperBnds);
      IV = &(dv->*vli->UncVars);
      sa = &(dv->*vli->Labels);
      if (!sa->size())
	sa->resize(nu);
      i = 0;
      for(vui = vli->vui; vui < vuie; ++vui) {
	if ((n = dv->*vui->n) == 0)
	  continue;
	for(j = 0; j < n; ++j)
	  if ((*IL)[i+j] > (*IU)[i+j]) {
	    squawk(Inconsistent_bounds, vui->vkind);
	    break;
	  }
	if ((*sa)[i] == "")
	  for(j = 1; j <= n; ++j) {
	    std::sprintf(buf, "%s%d", vui->lbl, (int)j);
	    (*sa)[i++] = buf;
	  }
	else
	  i += n;
      }
    }
    for(vlc = Vlch, vlce = vlc + Numberof(Vlch); vlc < vlce; ++vlc)
      if (vlc->stub && (n = dv->*vlc->n)) {
	sa = &(dv->*vlc->sa);
	if (sa->size() == 0)
	  BuildLabels(sa, n, 0, n, vlc->stub);
      }
  }
}

void NIDRProblemDescDB::check_variables_node(void *v)
{
  IntArray *Ia; IntVector *Iv; RealVector *Rv; RealSymMatrix *Rm;
  StringArray *sa;
  VLreal *vlr; VLint *vli;
  VarLabel *vl;
  VarLabelChk *vlc, *vlce;
  Var_uinfo *vui, *vuie;
  const char **sp;
  int havelabels;
  size_t i, j, k, n, nd, nu, nuk, nutot, nv;

#define AVI &Var_Info::
  // Used for deallocation of Var_Info temporary data
  static IntArray   *Var_Info::* Ia_delete[]
    = { AVI nddsi, AVI nddsr, AVI nCI, AVI nDI, AVI nhbp, AVI nhpp, AVI ndusi,
	AVI ndusr, AVI ndssi, AVI ndssr };
  static RealVector *Var_Info::* Rv_delete[]
    = { AVI ddsr, AVI CIlb, AVI CIub, AVI CIp, AVI DIp, AVI DSIp, AVI DSRp,
	AVI dusr, AVI hba, AVI hbo, AVI hbc, AVI hpa, AVI hpc, AVI ucm,
	AVI dssr };
  static IntVector *Var_Info::* Iv_delete[]
    = { AVI ddsi, AVI DIlb, AVI DIub, AVI dusi, AVI dssi };
#undef AVI
  static char ucmerr[]
    = "Got %lu entries for the uncertain_correlation_matrix\n\
	but needed %lu for %lu uncertain variables";

  Var_Info *vi = (Var_Info*)v;
  DataVariablesRep *dv = vi->dv;

  /* check and generate labels for design and state variables */

  nv = 0;
  for(vlc = Vlch, vlce = vlc + Numberof(Vlch); vlc < vlce; ++vlc)
    if ((n = dv->*vlc->n)) {
      if (vlc->stub)
	nv += n;
      sa = &(dv->*vlc->sa);
      if ((nu = sa->size())) {
	if (nu != n) {
	  if (nu == 1 && !vlc->stub)
	    continue;
	  squawk("Found %lu rather than %lu %s values",
		 (UL)nu, (UL)n, vlc->name);
	}
      }
    }

  /* now deal with uncertain variables */

  for(k = nu = nutot = 0; k < N_VLR; ++k) {
    vlr = &VLR[k];
    havelabels = 0;
    nuk = 0;
    vl = &(vi->*vlr->VL);	// "&(...)" to bypass a g++ bug
    vui = vlr->vui;
    for(vuie = vui + vlr->n; vui < vuie; ++vl, ++vui) {
      nuk += dv->*vui->n;
      if (vl->s)
	++havelabels;
    }
    if (nuk > 0) {
      nutot += nuk;
      if (VLR_aleatory[k])
	nu += nuk;
      if (havelabels)
	(sa = &(dv->*vlr->Labels))->resize(nuk);
      i = 0;
      vl = &(vi->*vlr->VL);
      for(vui = vlr->vui; vui < vuie; ++vl, ++vui) {
	if ((n = dv->*vui->n) == 0)
	  continue;
	vui->vchk(dv,i,vi);
	if ((sp = vl->s)) {
	  if (vl->n != n)
	    squawk("Expected %d %s_descriptors, but got %d",
		   n, vui->lbl, vl->n);
	  else
	    for(j = 0; j < n; ++i, ++j)
	      (*sa)[i] = sp[j];
	  free(sp);
	}
	else
	  i += n;
      }
    }
  }
  for(k = 0; k < N_VLI; ++k) {
    vli = &VLI[k];
    havelabels = 0;
    nuk = 0;
    vl = &(vi->*vli->VL);	// "&(...)" to bypass a g++ bug
    vui = vli->vui;
    for(vuie = vui + vli->n; vui < vuie; ++vl, ++vui) {
      nuk += dv->*vui->n;
      if (vl->s)
	++havelabels;
    }
    if (nuk > 0) {
      nutot += nuk;
      if (VLI_aleatory[k])
	nu += nuk;
      if (havelabels)
	(sa = &(dv->*vli->Labels))->resize(nuk);
      i = 0;
      vl = &(vi->*vli->VL);
      for(vui = vli->vui; vui < vuie; ++vl, ++vui) {
	if ((n = dv->*vui->n) == 0)
	  continue;
	vui->vchk(dv,i,vi);
	if ((sp = vl->s)) {
	  if (vl->n != n)
	    squawk("Expected %d %s_descriptors, but got %d",
		   n, vui->lbl, vl->n);
	  else
	    for(j = 0; j < n; ++i, ++j)
	      (*sa)[i] = sp[j];
	  free(sp);
	}
	else
	  i += n;
      }
    }
  }

  if ((Rv = vi->ucm)) { // uncertain_correlation_matrix
    n = Rv->length();
    if (n != nu*nu)
      squawk(ucmerr, (UL)n, (UL)nu*nu, (UL)nu);
    else {
      Rm = &dv->uncertainCorrelations;
      Rm->reshape(nu);
      for(i = k = 0; i < nu; ++i) {
	for(j = 0; j < nu;)
	  (*Rm)(i,j++) = (*Rv)[k++];
      }
    }
  }

  /* check discrete variables */

  nd = 0;
  for(vui=DiscSetLbl, vuie=DiscSetLbl+DiscSetVar_Nkinds; vui<vuie; ++vui) {
    if ((n = dv->*vui->n) > 0) {
      vui->vchk(dv,0,vi);
      nd += n;
    }
  }

  if (nd + nv + nutot == 0)
    squawk("at least one variable must be specified");

  /* check bounds */

  // loop over continuous bound specs: cdv/csv/nuv/lnuv/uuv/luuv/truv/buv
  Var_rcheck *rc, *rce;
  for(rc = var_mp_cbound, rce = rc + Numberof(var_mp_cbound); rc < rce; ++rc)
    Var_boundchk(dv, rc);
  // loop over discrete bound specs: design,state ranges
  Var_icheck *ic, *ice;
  for(ic = var_mp_drange, ice = ic + Numberof(var_mp_drange); ic < ice; ++ic)
    Var_iboundchk(dv, ic);

  /* finish up and deallocate temporary Var_Info data */

  n = Numberof(Ia_delete);
  for(i = 0; i < n; i++)
    if ((Ia = vi->*Ia_delete[i]))
      delete Ia;
  n = Numberof(Rv_delete);
  for(i = 0; i < n; i++)
    if ((Rv = vi->*Rv_delete[i]))
      delete Rv;
  n = Numberof(Iv_delete);
  for(i = 0; i < n; i++)
    if ((Iv = vi->*Iv_delete[i]))
      delete Iv;
  delete vi;
  if (nerr)
    abort_handler(-1);
}

static void flatten_num_rva(RealVectorArray *rva, IntArray **pia)
{
  size_t i, m;
  IntArray *ia;

  m = rva->size();
  *pia = ia = new IntArray(m);
  for(i = 0; i < m; ++i)
    (*ia)[i] = (*rva)[i].length();
}

static void flatten_num_rsa(RealSetArray *rsa, IntArray **pia)
{
  size_t i, m;
  IntArray *ia;

  m = rsa->size();
  *pia = ia = new IntArray(m);
  for(i = 0; i < m; ++i)
    (*ia)[i] = (*rsa)[i].size();
}

static void flatten_num_isa(IntSetArray *isa, IntArray **pia)
{
  size_t i, m;
  IntArray *ia;

  m = isa->size();
  *pia = ia = new IntArray(m);
  for(i = 0; i < m; ++i)
    (*ia)[i] = (*isa)[i].size();
}

static void flatten_num_rrma(RealRealMapArray *rrma, IntArray **pia)
{
  size_t i, m;
  IntArray *ia;

  m = rrma->size();
  *pia = ia = new IntArray(m);
  for(i = 0; i < m; ++i)
    (*ia)[i] = (*rrma)[i].size();
}

static void flatten_num_irma(IntRealMapArray *irma, IntArray **pia)
{
  size_t i, m;
  IntArray *ia;

  m = irma->size();
  *pia = ia = new IntArray(m);
  for(i = 0; i < m; ++i)
    (*ia)[i] = (*irma)[i].size();
}

static void flatten_rva(RealVectorArray *rva, RealVector **prv)
{
  size_t i, j, k, m, n;
  RealVector *rv, *rv_i;

  m = rva->size();
  for(i = n = 0; i < m; ++i)
    n += (*rva)[i].length();
  *prv = rv = new RealVector(n, false);
  for(i = k = 0; i < m; ++i) {
    rv_i = &(*rva)[i];
    n = rv_i->length();
    for(j = 0; j < n; ++j, ++k)
      (*rv)[k] = (*rv_i)[j];
  }
}

static void flatten_iva(IntVectorArray *iva, IntVector **piv)
{
  size_t i, j, k, m, n;
  IntVector *iv, *iv_i;

  m = iva->size();
  for(i = n = 0; i < m; ++i)
    n += (*iva)[i].length();
  *piv = iv = new IntVector(n, false);
  for(i = k = 0; i < m; ++i) {
    iv_i = &(*iva)[i];
    n = iv_i->length();
    for(j = 0; j < n; ++j, ++k)
      (*iv)[k] = (*iv_i)[j];
  }
}

static void flatten_rsm(RealSymMatrix *rsm, RealVector **prv)
{
  size_t i, j, m, n;
  RealVector *rv;

  m = rsm->numRows();
  *prv = rv = new RealVector(m*m, false);
  for(i = n = 0; i < m; ++i)
    for(j = 0; j < m; ++j, ++n)
      (*rv)[n] = (*rsm)(i,j);
}

static void flatten_rsa(RealSetArray *rsa, RealVector **prv)
{
  size_t i, j, k, m, n;
  RealVector *rv;
  RealSet *rs_i;
  RealSet::iterator rs_it, rs_ite;

  m = rsa->size();
  for(i = n = 0; i < m; ++i)
    n += (*rsa)[i].size();
  *prv = rv = new RealVector(n, false);
  for(i = k = 0; i < m; ++i) {
    rs_i = &(*rsa)[i];
    for(rs_it=rs_i->begin(), rs_ite=rs_i->end(); rs_it!=rs_ite; ++rs_it, ++k)
      (*rv)[k] = *rs_it;
  }
}

static void flatten_isa(IntSetArray *isa, IntVector **piv)
{
  size_t i, j, k, m, n;
  IntVector *iv;
  IntSet *is_i;
  IntSet::iterator is_it, is_ite;

  m = isa->size();
  for(i = n = 0; i < m; ++i)
    n += (*isa)[i].size();
  *piv = iv = new IntVector(n, false);
  for(i = k = 0; i < m; ++i) {
    is_i = &(*isa)[i];
    for(is_it=is_i->begin(), is_ite=is_i->end(); is_it!=is_ite; ++is_it, ++k)
      (*iv)[k] = *is_it;
  }
}

static void flatten_rrma_keys(RealRealMapArray *rrma, RealVector **prv)
{
  size_t i, j, k, m, n;
  RealVector *rv;
  RealRealMap *rrm_i;
  RealRealMap::iterator rrm_it, rrm_ite;

  m = rrma->size();
  for(i = n = 0; i < m; ++i)
    n += (*rrma)[i].size();
  *prv = rv = new RealVector(n, false);
  for(i = k = 0; i < m; ++i) {
    rrm_i = &(*rrma)[i];
    for (rrm_it=rrm_i->begin(), rrm_ite=rrm_i->end();
	 rrm_it!=rrm_ite; ++rrm_it, ++k)
      (*rv)[k] = rrm_it->first;
  }
}

static void flatten_rrma_values(RealRealMapArray *rrma, RealVector **prv)
{
  size_t i, j, k, m, n;
  RealVector *rv;
  RealRealMap *rrm_i;
  RealRealMap::iterator rrm_it, rrm_ite;

  m = rrma->size();
  for(i = n = 0; i < m; ++i)
    n += (*rrma)[i].size();
  *prv = rv = new RealVector(n, false);
  for(i = k = 0; i < m; ++i) {
    rrm_i = &(*rrma)[i];
    for (rrm_it=rrm_i->begin(), rrm_ite=rrm_i->end();
	 rrm_it!=rrm_ite; ++rrm_it, ++k)
      (*rv)[k] = rrm_it->second;
  }
}

static void flatten_irma_keys(IntRealMapArray *irma, IntVector **piv)
{
  size_t i, j, k, m, n;
  IntVector *iv;
  IntRealMap *irm_i;
  IntRealMap::iterator irm_it, irm_ite;

  m = irma->size();
  for(i = n = 0; i < m; ++i)
    n += (*irma)[i].size();
  *piv = iv = new IntVector(n, false);
  for(i = k = 0; i < m; ++i) {
    irm_i = &(*irma)[i];
    for (irm_it=irm_i->begin(), irm_ite=irm_i->end();
	 irm_it!=irm_ite; ++irm_it, ++k)
      (*iv)[k] = irm_it->first;
  }
}

static void flatten_irma_values(IntRealMapArray *irma, RealVector **prv)
{
  size_t i, j, k, m, n;
  RealVector *rv;
  IntRealMap *irm_i;
  IntRealMap::iterator irm_it, irm_ite;

  m = irma->size();
  for(i = n = 0; i < m; ++i)
    n += (*irma)[i].size();
  *prv = rv = new RealVector(n, false);
  for(i = k = 0; i < m; ++i) {
    irm_i = &(*irma)[i];
    for (irm_it=irm_i->begin(), irm_ite=irm_i->end();
	 irm_it!=irm_ite; ++irm_it, ++k)
      (*rv)[k] = irm_it->second;
  }
}

void NIDRProblemDescDB::
check_variables(std::list<DataVariables>* dvl)
{
  if (pDDBInstance) {
    std::list<void*>::iterator It, Ite = pDDBInstance->VIL.end();
    for(It = pDDBInstance->VIL.begin(); It != Ite; ++It)
      check_variables_node(*It);
    pDDBInstance->VIL.clear();
  }
  else {
    // library mode with manual provision of everything
    DataVariablesRep *dv;
    IntArray *ia;
    RealSymMatrix *rsm;
    RealVector *rv, *rv_a, *rv_c;
    RealVectorArray *rva;
    Var_Info *vi;
    size_t i, j, m, n, cntr;
    int num_prs_i, total_prs;

    // copy from DataVariables into Var_Info so that check_variables_node() can
    // go the other direction.  TO DO: can we eliminate this circular update?
    std::list<DataVariables>::iterator It = dvl->begin(), Ite = dvl->end();
    for(; It != Ite; ++It) {

      // create new Var_Info instance to hold DataVariables data

      vi = new Var_Info;
      memset(vi, 0, sizeof(Var_Info));
      vi->dv_handle = &*It;
      vi->dv = dv = It->dataVarsRep;

      // flatten 2D {Real,Int}{Vector,Set}Arrays back into Var_Info 1D arrays

      // discrete design set int vars
      if ((n = dv->numDiscreteDesSetIntVars)) {
	// Note: set consolidation and/or reordering cannot be undone
	flatten_num_isa(&dv->discreteDesignSetInt, &vi->nddsi);
	flatten_isa(&dv->discreteDesignSetInt,     &vi->ddsi);
      }
      // discrete design set real vars
      if ((n = dv->numDiscreteDesSetRealVars)) {
	// Note: set consolidation and/or reordering cannot be undone
	flatten_num_rsa(&dv->discreteDesignSetReal, &vi->nddsr);
	flatten_rsa(&dv->discreteDesignSetReal,     &vi->ddsr);
      }
      // histogram bin uncertain vars
      rva = &dv->histogramUncBinPairs;
      if ((m = rva->size())) {
	vi->nhbp = ia = new IntArray(m);
	for(i = 0; i < m; ++i)
	  total_prs += (*ia)[i] = (*rva)[i].length() / 2;
	vi->hba = rv_a = new RealVector(total_prs); // abscissas
	vi->hbc = rv_c = new RealVector(total_prs); // counts
	vi->hbo = NULL;                            // no ordinates
	for(i = cntr = 0; i < m; ++i) {
	  num_prs_i = (*ia)[i];
	  for(j = 0; j < num_prs_i; ++j, ++cntr) {
	    (*rv_a)[cntr] = (*rva)[i][2*j];   // abscissas
	    (*rv_c)[cntr] = (*rva)[i][2*j+1]; // counts only (no ordinates)
	  }
	  // normalization occurs in Vchk_HistogramBinUnc going other direction
	}
      }
      // histogram point uncertain vars
      rva = &dv->histogramUncPointPairs;
      if ((m = rva->size())) {
	vi->nhpp = ia = new IntArray(m);
	for(i = 0; i < m; ++i)
	  total_prs += (*ia)[i] = (*rva)[i].length() / 2;
	vi->hpa = rv_a = new RealVector(total_prs); // abscissas
	vi->hpc = rv_c = new RealVector(total_prs); // counts
	for(i = cntr = 0; i < m; ++i) {
	  num_prs_i = (*ia)[i];
	  for(j = 0; j < num_prs_i; ++j, ++cntr) {
	    (*rv_a)[cntr] = (*rva)[i][2*j];   // abscissas
	    (*rv_c)[cntr] = (*rva)[i][2*j+1]; // counts
	  }
	  // normalization occurs in Vchk_HistogramPtUnc going other direction
	}
      }
      // uncertain correlation matrix
      if (dv->uncertainCorrelations.numRows())
	flatten_rsm(&dv->uncertainCorrelations, &vi->ucm);
      // continuous interval uncertain vars
      if ((n = dv->numContinuousIntervalUncVars)) {
	flatten_num_rva(&dv->continuousIntervalUncBasicProbs, &vi->nCI);
	flatten_rva(&dv->continuousIntervalUncBasicProbs,     &vi->CIp);
	flatten_rva(&dv->continuousIntervalUncLowerBounds,    &vi->CIlb);
	flatten_rva(&dv->continuousIntervalUncUpperBounds,    &vi->CIub);
      }
      // discrete interval uncertain vars
      if ((n = dv->numDiscreteIntervalUncVars)) {
	flatten_num_rva(&dv->discreteIntervalUncBasicProbs, &vi->nDI);
	flatten_rva(&dv->discreteIntervalUncBasicProbs,     &vi->DIp);
	flatten_iva(&dv->discreteIntervalUncLowerBounds,    &vi->DIlb);
	flatten_iva(&dv->discreteIntervalUncUpperBounds,    &vi->DIub);
      }
      // discrete uncertain set int vars
      if ((n = dv->numDiscreteUncSetIntVars)) {
	// Note: map consolidation and/or reordering cannot be undone
	flatten_num_irma(&dv->discreteUncSetIntValuesProbs,    &vi->ndusi);
	flatten_irma_keys(&dv->discreteUncSetIntValuesProbs,   &vi->dusi);
	flatten_irma_values(&dv->discreteUncSetIntValuesProbs, &vi->DSIp);
      }
      // discrete uncertain set real vars
      if ((n = dv->numDiscreteUncSetRealVars)) {
	// Note: map consolidation and/or reordering cannot be undone
	flatten_num_rrma(&dv->discreteUncSetRealValuesProbs,    &vi->ndusr);
	flatten_rrma_keys(&dv->discreteUncSetRealValuesProbs,   &vi->dusr);
	flatten_rrma_values(&dv->discreteUncSetRealValuesProbs, &vi->DSRp);
      }
      // discrete state set int vars
      if ((n = dv->numDiscreteStateSetIntVars)) {
	// Note: set consolidation and/or reordering cannot be undone
	flatten_num_isa(&dv->discreteStateSetInt, &vi->ndssi);
	flatten_isa(&dv->discreteStateSetInt,     &vi->dssi);
      }
      // discrete state set real vars
      if ((n = dv->numDiscreteStateSetRealVars)) {
	// Note: set consolidation and/or reordering cannot be undone
	flatten_num_rsa(&dv->discreteStateSetReal, &vi->ndssr);
	flatten_rsa(&dv->discreteStateSetReal,     &vi->dssr);
      }

      check_variables_node((void*)vi);
    }
  }
}

void NIDRProblemDescDB::
var_str(const char *keyname, Values *val, void **g, void *v)
{
  (*(Var_Info**)g)->dv->**(String DataVariablesRep::**)v = *val->s;
}

void NIDRProblemDescDB::
var_strL(const char *keyname, Values *val, void **g, void *v)
{
  StringArray *sa = &((*(Var_Info**)g)->dv->**(StringArray DataVariablesRep::**)v);
  const char **s = val->s;
  size_t i, n = val->n;

  sa->resize(n);
  for(i = 0; i < n; i++)
    (*sa)[i] = s[i];
}

static void
var_iulbl(const char *keyname, Values *val, VarLabel *vl)
{
  char *t;
  const char **s, **sl;
  size_t i, L, n;

  L = n = val->n;	// n for null
  s = val->s;
  for(i = 0; i < n; i++)
    L += strlen(s[i]);
  vl->s = sl = (const char **)malloc(n*sizeof(char*) + L);
  if (!sl)
    NIDRProblemDescDB::botch("malloc failure in var_ulbl");
  vl->n = n;
  t = (char*)(sl + n);
  for(i = 0; i < n; i++) {
    strcpy(t, s[i]);
    sl[i] = (const char*)t;
    t += strlen(t) + 1;
  }
}

void NIDRProblemDescDB::
var_caulbl(const char *keyname, Values *val, void **g, void *v)
{
  // IRIX disallows (int)v, so we use the circumlocution (char*)v - (char*)0.
  VarLabel *vl = &(*(Var_Info**)g)->CAUv[(char*)v - (char*)0];
  var_iulbl(keyname, val, vl);
}

void NIDRProblemDescDB::
var_ceulbl(const char *keyname, Values *val, void **g, void *v)
{
  VarLabel *vl = &(*(Var_Info**)g)->CEUv[(char*)v - (char*)0];
  var_iulbl(keyname, val, vl);
}

void NIDRProblemDescDB::
var_dauilbl(const char *keyname, Values *val, void **g, void *v)
{
  VarLabel *vl = &(*(Var_Info**)g)->DAUIv[(char*)v - (char*)0];
  var_iulbl(keyname, val, vl);
}

void NIDRProblemDescDB::
var_deuilbl(const char *keyname, Values *val, void **g, void *v)
{
  VarLabel *vl = &(*(Var_Info**)g)->DEUIv[(char*)v - (char*)0];
  var_iulbl(keyname, val, vl);
}

void NIDRProblemDescDB::
var_daurlbl(const char *keyname, Values *val, void **g, void *v)
{
  VarLabel *vl = &(*(Var_Info**)g)->DAURv[(char*)v - (char*)0];
  var_iulbl(keyname, val, vl);
}

void NIDRProblemDescDB::
var_deurlbl(const char *keyname, Values *val, void **g, void *v)
{
  VarLabel *vl = &(*(Var_Info**)g)->DEURv[(char*)v - (char*)0];
  var_iulbl(keyname, val, vl);
}

#define MP_(x) DataInterfaceRep::* iface_mp_##x = &DataInterfaceRep::x
#define MP2(x,y) iface_mp_##x##_##y = {&DataInterfaceRep::x,#y}
#define MP3(x,y,z) iface_mp_3##x##_##z = {&DataInterfaceRep::x,&DataInterfaceRep::y,#z}

static Iface_mp_Rlit
	MP3(failAction,recoveryFnVals,recover);

static Iface_mp_ilit
	MP3(failAction,retryLimit,retry);

static Iface_mp_lit
	MP2(analysisScheduling,master),
	MP2(analysisScheduling,peer),
      //MP2(analysisScheduling,peer_dynamic),
      //MP2(analysisScheduling,peer_static),
	MP2(evalScheduling,master),
	MP2(evalScheduling,peer_dynamic),
	MP2(evalScheduling,peer_static),
	MP2(failAction,abort),
	MP2(failAction,continuation),
	MP2(interfaceSynchronization,asynchronous),
	MP2(interfaceType,direct),
	MP2(interfaceType,fork),
	MP2(interfaceType,grid),
	MP2(interfaceType,matlab),
	MP2(interfaceType,python),
	MP2(interfaceType,scilab),
	MP2(interfaceType,system),
	MP2(asynchLocalEvalScheduling,dynamic),
	MP2(asynchLocalEvalScheduling,static);

static String
	MP_(algebraicMappings),
	MP_(idInterface),
	MP_(inputFilter),
	MP_(outputFilter),
	MP_(parametersFile),
	MP_(resultsFile),
	MP_(templateDir),
	MP_(workDir);

static String2DArray
	MP_(analysisComponents);

static StringArray
	MP_(analysisDrivers),
	MP_(templateFiles);

static bool
	MP_(activeSetVectorFlag),
	MP_(allowExistingResultsFlag),
	MP_(apreproFlag),
	MP_(dirSave),
	MP_(dirTag),
	MP_(evalCacheFlag),
	MP_(fileSaveFlag),
	MP_(fileTagFlag),
	MP_(numpyFlag),
	MP_(restartFileFlag),
	MP_(templateCopy),
	MP_(templateReplace),
	MP_(useWorkdir),
	MP_(verbatimFlag);

static int
	MP_(analysisServers),
	MP_(asynchLocalAnalysisConcurrency),
	MP_(asynchLocalEvalConcurrency),
	MP_(evalServers),
	MP_(procsPerAnalysis);

#undef MP3
#undef MP2
#undef MP_


// Macros for Method

#define MP_(x) DataMethodRep::* method_mp_##x = &DataMethodRep::x
#define MP2(x,y) method_mp_##x##_##y = {&DataMethodRep::x,#y}
#define MP2s(x,y) method_mp_##x##_##y = {&DataMethodRep::x,y}
#if HAVE_OPTPP
#  define MP2o(x,y) method_mp_##x##_##y = {&DataMethodRep::x,OPTPP::y}
#else
#  define MP2o(x,y) method_mp_##x##_##y = {NULL, NULL}
#endif
#define MP2p(x,y) method_mp_##x##_##y = {&DataMethodRep::x,Pecos::y}
#define MP3(x,y,z) method_mp_3##x##_##z = {&DataMethodRep::x,&DataMethodRep::y,#z}
#define MP4(w,x,y,z) method_mp_##w##_##y = {&DataMethodRep::w,&DataMethodRep::x,#y,z}

static IntVector
	MP_(primeBase),
	MP_(sequenceLeap),
	MP_(sequenceStart),
	MP_(stepsPerVariable);

static Method_mp_ilit2
	MP3(replacementType,numberRetained,chc),
	MP3(replacementType,numberRetained,elitist),
	MP3(replacementType,numberRetained,random);

static Method_mp_ilit2z
	MP3(crossoverType,numCrossPoints,multi_point_binary),
	MP3(crossoverType,numCrossPoints,multi_point_parameterized_binary),
	MP3(crossoverType,numCrossPoints,multi_point_real);

static Method_mp_lit
	MP2(batchSelectionType,naive),
	MP2(batchSelectionType,distance_penalty),
	MP2(batchSelectionType,topology),
	MP2(batchSelectionType,constant_liar),
	MP2(boxDivision,all_dimensions),
	MP2(boxDivision,major_dimension),
	MP2(convergenceType,average_fitness_tracker),
	MP2(convergenceType,best_fitness_tracker),
	MP2(convergenceType,metric_tracker),
	MP2(crossoverType,blend),
	MP2(crossoverType,two_point),
	MP2(crossoverType,uniform),
	MP2(evalSynchronize,blocking),
	MP2(evalSynchronize,nonblocking),
	MP2(expansionSampleType,incremental_lhs),
	MP2(exploratoryMoves,adaptive),
	MP2(exploratoryMoves,multi_step),
	MP2(exploratoryMoves,simple),
	MP2(fitnessType,domination_count),
	MP2(fitnessType,layer_rank),
	MP2(fitnessType,linear_rank),
	MP2(fitnessType,merit_function),
	MP2(fitnessType,proportional),
	MP2(fitnessMetricType,predicted_variance),
	MP2(fitnessMetricType,distance),
	MP2(fitnessMetricType,gradient),
	MP2(initializationType,random),
	MP2(initializationType,unique_random),
	MP2(integrationRefine,ais),
	MP2(integrationRefine,is),
	MP2(integrationRefine,mmais),
        MP2(meritFunction,merit_max),
        MP2(meritFunction,merit_max_smooth),
        MP2(meritFunction,merit1),
        MP2(meritFunction,merit1_smooth),
        MP2(meritFunction,merit2),
        MP2(meritFunction,merit2_smooth),
        MP2(meritFunction,merit2_squared),
	MP2(methodName,asynch_pattern_search),
	MP2(methodName,coliny_beta),
	MP2(methodName,coliny_cobyla),
	MP2(methodName,coliny_direct),
	MP2(methodName,coliny_ea),
	MP2(methodName,coliny_pattern_search),
	MP2(methodName,coliny_solis_wets),
	MP2(methodName,conmin_frcg),
	MP2(methodName,conmin_mfd),
	MP2(methodName,dace),
	MP2(methodName,dot_bfgs),
	MP2(methodName,dot_frcg),
	MP2(methodName,dot_mmfd),
	MP2(methodName,dot_slp),
	MP2(methodName,dot_sqp),
	MP2(methodName,efficient_global),
	MP2(methodName,fsu_cvt),
	MP2(methodName,fsu_halton),
	MP2(methodName,fsu_hammersley),
	MP2(methodName,mesh_adaptive_search),
	MP2(methodName,moga),
	MP2(methodName,ncsu_direct),
	MP2(methodName,nl2sol),
	MP2(methodName,nlpql_sqp),
	MP2(methodName,nlssol_sqp),
        MP2(methodName,nond_adaptive_sampling),
	MP2(methodName,nond_bayes_calibration),
	MP2(methodName,nond_efficient_subspace),
        MP2(methodName,nond_gpais),
	MP2(methodName,nond_global_evidence),
        MP2(methodName,nond_global_interval_est),
	MP2(methodName,nond_global_reliability),
        MP2(methodName,nond_importance_sampling),
 	MP2(methodName,nond_local_evidence),
        MP2(methodName,nond_local_interval_est),
        MP2(methodName,nond_pof_darts),
	MP2(methodName,nond_polynomial_chaos),
	MP2(methodName,nond_sampling),
	MP2(methodName,nond_stoch_collocation),
	MP2(methodName,nonlinear_cg),
	MP2(methodName,npsol_sqp),
	MP2(methodName,optdarts),
	MP2(methodName,optpp_cg),
	MP2(methodName,optpp_fd_newton),
	MP2(methodName,optpp_g_newton),
	MP2(methodName,optpp_newton),
	MP2(methodName,optpp_pds),
	MP2(methodName,optpp_q_newton),
	MP2(methodName,psuade_moat),
	MP2(methodName,richardson_extrap),
	MP2(methodName,soga),
	MP2(methodName,surrogate_based_global),
	MP2(methodName,surrogate_based_local),
	MP2(methodName,vector_parameter_study),
	MP2(methodName,list_parameter_study),
	MP2(methodName,centered_parameter_study),
	MP2(methodName,multidim_parameter_study),
	MP2(metropolisType,adaptive),
	MP2(metropolisType,hastings),
	MP2(mutationType,bit_random),
	MP2(mutationType,offset_cauchy),
	MP2(mutationType,offset_normal),
	MP2(mutationType,offset_uniform),
	MP2(mutationType,replace_uniform),
	MP2(nondOptAlgorithm,nip),
	MP2(nondOptAlgorithm,sqp),
	MP2(nondOptAlgorithm,ea),
	MP2(nondOptAlgorithm,ego),
	MP2(nondOptAlgorithm,lhs),
	MP2(nondOptAlgorithm,sbo),
	MP2(patternBasis,coordinate),
	MP2(patternBasis,simplex),
	MP2(pointReuse,all),
	MP2(rejectionType,standard),
	MP2(rejectionType,delayed),
	MP2(reliabilityIntegration,first_order),
	MP2(reliabilityIntegration,second_order),
	MP2(reliabilitySearchType,amv_plus_u),
	MP2(reliabilitySearchType,amv_plus_x),
	MP2(reliabilitySearchType,amv_u),
	MP2(reliabilitySearchType,amv_x),
	MP2(reliabilitySearchType,egra_u),
	MP2(reliabilitySearchType,egra_x),
	MP2(reliabilitySearchType,no_approx),
	MP2(reliabilitySearchType,tana_u),
	MP2(reliabilitySearchType,tana_x),
	MP2(replacementType,elitist),
	MP2(replacementType,favor_feasible),
	MP2(replacementType,roulette_wheel),
	MP2(replacementType,unique_roulette_wheel),
	MP2(rngName,mt19937),
	MP2(rngName,rnum2),
	MP2(sampleType,incremental_lhs),
	MP2(sampleType,incremental_random),
	MP2(sampleType,lhs),
	MP2(sampleType,random),
	MP2(searchMethod,gradient_based_line_search),
	MP2(searchMethod,tr_pds),
	MP2(searchMethod,trust_region),
	MP2(searchMethod,value_based_line_search),
	MP2(subMethodName,box_behnken),
	MP2(subMethodName,central_composite),
	MP2(subMethodName,dream),
	MP2(subMethodName,gpmsa),
	MP2(subMethodName,grid),
	MP2(subMethodName,lhs),
	MP2(subMethodName,oa_lhs),
	MP2(subMethodName,oas),
	MP2(subMethodName,queso),
	MP2(subMethodName,random),
	MP2(subMethodName,converge_order),
	MP2(subMethodName,converge_qoi),
	MP2(subMethodName,estimate_order),
	MP2(trialType,grid),
	MP2(trialType,halton),
	MP2(trialType,random);

static Method_mp_lit2
	MP4(methodName,reliabilitySearchType,nond_local_reliability,"mv");

static Method_mp_litc
	MP3(crossoverType,crossoverRate,shuffle_random),
	MP3(crossoverType,crossoverRate,null_crossover),
	MP3(mutationType,mutationRate,null_mutation),
	MP3(mutationType,mutationRate,offset_cauchy),
	MP3(mutationType,mutationRate,offset_normal),
	MP3(mutationType,mutationRate,offset_uniform),
	MP3(replacementType,fitnessLimit,below_limit);

static Method_mp_litrv
	MP3(nichingType,nicheVector,distance),
	MP3(nichingType,nicheVector,max_designs),
	MP3(nichingType,nicheVector,radial),
	MP3(postProcessorType,distanceVector,distance_postprocessor);

static Method_mp_slit2
	MP3(initializationType,flatFile,flat_file),
	MP3(methodName,dlDetails,dl_solver);

static Real
	MP_(absConvTol),
	MP_(centeringParam),
	MP_(collocationRatio),
	MP_(collocRatioTermsOrder),
	MP_(constraintPenalty),
	MP_(constrPenalty),
	MP_(constraintTolerance),
	MP_(contractFactor),
	MP_(contractStepLength),
	MP_(convergenceTolerance),
	MP_(crossoverRate),
	MP_(falseConvTol),
	MP_(functionPrecision),
	MP_(globalBalanceParam),
	MP_(gradientTolerance),
	MP_(grThreshold),
	MP_(initDelta),
	MP_(initStepLength),
	MP_(initTRRadius),
	MP_(likelihoodScale),
	MP_(lineSearchTolerance),
	MP_(localBalanceParam),
	MP_(maxBoxSize),
	MP_(maxStep),
	MP_(minBoxSize),
	MP_(mutationRate),
	MP_(mutationScale),
	MP_(refinementRate),
	MP_(regressionL2Penalty),
	MP_(shrinkagePercent),	// should be called shrinkageFraction
	MP_(singConvTol),
	MP_(singRadius),
        MP_(smoothFactor),
 	MP_(solnTarget),
	MP_(stepLenToBoundary),
	MP_(surrBasedLocalTRContract),
	MP_(surrBasedLocalTRContractTrigger),
	MP_(surrBasedLocalTRExpand),
	MP_(surrBasedLocalTRExpandTrigger),
	MP_(surrBasedLocalTRInitSize),
	MP_(surrBasedLocalTRMinSize),
	MP_(threshDelta),
	MP_(threshStepLength),
	MP_(vbdDropTolerance),
	MP_(volBoxSize),
	MP_(vns),
	MP_(xConvTol);

static RealVector
	MP_(anisoDimPref),
	MP_(finalPoint),
	MP_(linearEqConstraintCoeffs),
	MP_(linearEqScales),
	MP_(linearEqTargets),
	MP_(linearIneqConstraintCoeffs),
	MP_(linearIneqLowerBnds),
	MP_(linearIneqUpperBnds),
	MP_(linearIneqScales),
	MP_(listOfPoints),
	MP_(proposalCovScale),
	MP_(regressionNoiseTol),
	MP_(stepVector);

static RealVectorArray
	MP_(genReliabilityLevels),
	MP_(probabilityLevels),
	MP_(reliabilityLevels),
	MP_(responseLevels);

static unsigned short
	MP_(cubIntOrder),
	MP_(vbdOrder);

static SizetArray
	MP_(collocationPoints),
	MP_(expansionSamples);

static UShortArray
        MP_(expansionOrder),
        MP_(quadratureOrder),
	MP_(sparseGridLevel),
        MP_(tensorGridOrder),
	MP_(varPartitions);

static String
	MP_(approxExportFile),
	MP_(approxImportFile),
        MP_(betaSolverName),
        MP_(displayFormat),
	MP_(expansionImportFile),
	MP_(historyFile),
	MP_(idMethod),
	MP_(logFile),
	MP_(modelPointer),
	MP_(pstudyFilename),
	MP_(subMethodName),
        MP_(subMethodPointer);

static StringArray
	MP_(linearEqScaleTypes),
	MP_(linearIneqScaleTypes),
        MP_(miscOptions);

static bool
	MP_(approxExportAnnotated),
	MP_(approxImportAnnotated),
	MP_(calibrateSigmaFlag),
	MP_(constantPenalty),
	MP_(crossValidation),
	MP_(expansionFlag),
	MP_(fixedSeedFlag),
	MP_(fixedSequenceFlag),
	MP_(latinizeFlag),
	MP_(mainEffectsFlag),
	MP_(methodScaling),
	MP_(methodUseDerivsFlag),
	MP_(mutationAdaptive),
	MP_(normalizedCoeffs),
	MP_(printPopFlag),
	MP_(pstudyFileAnnotated),
	MP_(randomizeOrderFlag),
	MP_(regressDiag),
	MP_(showAllEval),
	MP_(showMiscOptions),
	MP_(speculativeFlag),
	MP_(tensorGridFlag),
	MP_(surrBasedGlobalReplacePts),
	MP_(surrBasedLocalLayerBypass),
	MP_(vbdFlag),
	MP_(volQualityFlag);

static short
        MP_(expansionType),
	MP_(nestingOverride),
	MP_(refinementType);

static int
	MP_(batchSize),
	MP_(contractAfterFail),
	MP_(covarianceType),
        MP_(crossoverChainPairs),
	MP_(emulatorSamples),
	MP_(expandAfterSuccess),
	MP_(jumpStep),
	MP_(maxFunctionEvaluations),
	MP_(maxIterations),
	MP_(mutationRange),
	MP_(newSolnsGenerated),
	MP_(numChains),
	MP_(numCR),
	MP_(numSamples),
	MP_(numSteps),
	MP_(numSymbols),
	MP_(numTrials),
	MP_(populationSize),
	MP_(previousSamples),
	MP_(randomSeed),
	MP_(refineSamples),
	MP_(searchSchemeSize),
	MP_(surrBasedLocalSoftConvLimit),
	MP_(totalPatternSize),
	MP_(verifyLevel);

static size_t
        MP_(numDesigns),
        MP_(numFinalSolutions),
	MP_(numGenerations),
	MP_(numOffspring),
	MP_(numParents);

static Method_mp_type
	MP2s(covarianceControl,DIAGONAL_COVARIANCE),
	MP2s(covarianceControl,FULL_COVARIANCE),
	MP2s(distributionType,COMPLEMENTARY),
	MP2s(distributionType,CUMULATIVE),
	MP2s(emulatorType,GAUSSIAN_PROCESS),
	MP2s(emulatorType,KRIGING),
	MP2s(emulatorType,POLYNOMIAL_CHAOS),
	MP2s(emulatorType,STOCHASTIC_COLLOCATION),
	MP2s(expansionType,ASKEY_U),
	MP2s(expansionType,STD_NORMAL_U),
	MP2p(growthOverride,RESTRICTED),                   // Pecos enumeration
	MP2p(growthOverride,UNRESTRICTED),                 // Pecos enumeration
	MP2s(lsRegressionType,EQ_CON_LS),
	MP2s(lsRegressionType,SVD_LS),
	MP2o(meritFn,ArgaezTapia),                         // OPTPP enumeration
	MP2o(meritFn,NormFmu),                             // OPTPP enumeration
	MP2o(meritFn,VanShanno),                           // OPTPP enumeration
	MP2s(methodOutput,DEBUG_OUTPUT),
	MP2s(methodOutput,NORMAL_OUTPUT),
	MP2s(methodOutput,QUIET_OUTPUT),
	MP2s(methodOutput,SILENT_OUTPUT),
	MP2s(methodOutput,VERBOSE_OUTPUT),
	MP2p(nestingOverride,NESTED),                      // Pecos enumeration
	MP2p(nestingOverride,NON_NESTED),                  // Pecos enumeration
	MP2p(refinementControl,DIMENSION_ADAPTIVE_CONTROL_GENERALIZED),// Pecos
	MP2p(refinementControl,DIMENSION_ADAPTIVE_CONTROL_DECAY),      // Pecos
	MP2p(refinementControl,DIMENSION_ADAPTIVE_CONTROL_SOBOL),      // Pecos
	MP2p(refinementControl,LOCAL_ADAPTIVE_CONTROL),                // Pecos
	MP2p(refinementControl,UNIFORM_CONTROL),                       // Pecos
	MP2p(refinementType,P_REFINEMENT),                 // Pecos enumeration
        MP2p(refinementType,H_REFINEMENT),                 // Pecos enumeration
	MP2p(regressionType,BASIS_PURSUIT),                // Pecos enumeration
	MP2p(regressionType,BASIS_PURSUIT_DENOISING),      // Pecos enumeration
	MP2p(regressionType,DEFAULT_LEAST_SQ_REGRESSION),  // Pecos enumeration
	MP2p(regressionType,LASSO_REGRESSION),             // Pecos enumeration
	MP2p(regressionType,LEAST_ANGLE_REGRESSION),       // Pecos enumeration
	MP2p(regressionType,ORTHOG_LEAST_INTERPOLATION),   // Pecos enumeration
	MP2p(regressionType,ORTHOG_MATCH_PURSUIT),         // Pecos enumeration
	MP2s(responseLevelTarget,GEN_RELIABILITIES),
	MP2s(responseLevelTarget,PROBABILITIES),
	MP2s(responseLevelTarget,RELIABILITIES),
	MP2s(responseLevelTargetReduce,SYSTEM_PARALLEL),
	MP2s(responseLevelTargetReduce,SYSTEM_SERIES),
	MP2s(sparseGridBasisType,NODAL_INTERPOLANT),
	MP2s(sparseGridBasisType,HIERARCHICAL_INTERPOLANT),
	MP2s(surrBasedLocalAcceptLogic,FILTER),
	MP2s(surrBasedLocalAcceptLogic,TR_RATIO),
	MP2s(surrBasedLocalConstrRelax,HOMOTOPY),
	MP2s(surrBasedLocalMeritFn,ADAPTIVE_PENALTY_MERIT),
	MP2s(surrBasedLocalMeritFn,AUGMENTED_LAGRANGIAN_MERIT),
	MP2s(surrBasedLocalMeritFn,LAGRANGIAN_MERIT),
	MP2s(surrBasedLocalMeritFn,PENALTY_MERIT),
	MP2s(surrBasedLocalSubProbCon,LINEARIZED_CONSTRAINTS),
	MP2s(surrBasedLocalSubProbCon,NO_CONSTRAINTS),
	MP2s(surrBasedLocalSubProbCon,ORIGINAL_CONSTRAINTS),
	MP2s(surrBasedLocalSubProbObj,AUGMENTED_LAGRANGIAN_OBJECTIVE),
	MP2s(surrBasedLocalSubProbObj,LAGRANGIAN_OBJECTIVE),
	MP2s(surrBasedLocalSubProbObj,ORIGINAL_PRIMARY),
	MP2s(surrBasedLocalSubProbObj,SINGLE_OBJECTIVE);

#undef MP4
#undef MP3
#undef MP2s
#undef MP2o
#undef MP2p
#undef MP2
#undef MP_


// Macros for handling Model data

#define MP_(x) DataModelRep::* model_mp_##x = &DataModelRep::x
#define MP2(x,y) model_mp_##x##_##y = {&DataModelRep::x,#y}
#define MP2s(x,y) model_mp_##x##_##y = {&DataModelRep::x,y}

static IntSet
	MP_(surrogateFnIndices);

static Model_mp_lit
	MP2(approxPointReuse,all),
	MP2(approxPointReuse,none),
	MP2(approxPointReuse,region),
	MP2(marsInterpolation,linear),
	MP2(marsInterpolation,cubic),
	MP2(modelType,nested),
	MP2(modelType,single),
	MP2(modelType,surrogate),
	MP2(surrogateType,hierarchical),
	MP2(surrogateType,global_gaussian),
	MP2(surrogateType,global_kriging),
	MP2(surrogateType,global_mars),
	MP2(surrogateType,global_moving_least_squares),
	MP2(surrogateType,global_neural_network),
	MP2(surrogateType,global_polynomial),
	MP2(surrogateType,global_radial_basis),
	MP2(surrogateType,local_taylor),
        MP2(surrogateType,multipoint_tana),
        MP2(trendOrder,constant),
        MP2(trendOrder,linear),
        MP2(trendOrder,reduced_quadratic),
        MP2(trendOrder,quadratic);

static Model_mp_ord
	MP2s(approxCorrectionOrder,0),
	MP2s(approxCorrectionOrder,1),
	MP2s(approxCorrectionOrder,2),
	MP2s(polynomialOrder,1),
	MP2s(polynomialOrder,2),
        MP2s(polynomialOrder,3);

static Model_mp_type
	MP2s(approxCorrectionType,ADDITIVE_CORRECTION),
	MP2s(approxCorrectionType,COMBINED_CORRECTION),
	MP2s(approxCorrectionType,MULTIPLICATIVE_CORRECTION),
	MP2s(pointsManagement,MINIMUM_POINTS),
        MP2s(pointsManagement,RECOMMENDED_POINTS);

static Real
	MP_(annRange),
	MP_(krigingNugget),
	MP_(percentFold);

static RealVector
	MP_(krigingCorrelations),
//	MP_(krigingMaxCorrelations),
//	MP_(krigingMinCorrelations),
	MP_(primaryRespCoeffs),
	MP_(secondaryRespCoeffs);

static String
	MP_(approxChallengeFile),
	MP_(approxExportFile),
	MP_(approxImportFile),
	MP_(approxExportModelFile),
	MP_(idModel),
	MP_(interfacePointer),
	MP_(krigingOptMethod),
	MP_(lowFidelityModelPointer),
	MP_(optionalInterfRespPointer),
	MP_(responsesPointer),
	MP_(subMethodPointer),
	MP_(truthModelPointer),
	MP_(variablesPointer);

static StringArray
	MP_(primaryVarMaps),
        MP_(secondaryVarMaps),
        MP_(diagMetrics);

static bool
	MP_(approxChallengeAnnotated),
	MP_(approxExportAnnotated),
	MP_(approxImportAnnotated),
	MP_(crossValidateFlag),
	MP_(hierarchicalTags),
	MP_(modelUseDerivsFlag),
	MP_(pressFlag),
	MP_(pointSelection);

static short
	MP_(annNodes),
	MP_(annRandomWeight),
	MP_(krigingFindNugget),
	MP_(krigingMaxTrials),
	MP_(marsMaxBases),
	MP_(mlsPolyOrder),
	MP_(mlsWeightFunction),
	MP_(rbfBases),
	MP_(rbfMaxPts),
	MP_(rbfMaxSubsets),
	MP_(rbfMinPartition);

static int
	MP_(numFolds),
        MP_(pointsTotal);

#undef MP2s
#undef MP2
#undef MP_

#define MP_(x) DataResponsesRep::* resp_mp_##x = &DataResponsesRep::x
#define MP2(x,y) resp_mp_##x##_##y = {&DataResponsesRep::x,#y}

static IntSet
	MP_(idAnalyticGrads),
	MP_(idAnalyticHessians),
	MP_(idNumericalGrads),
	MP_(idNumericalHessians),
	MP_(idQuasiHessians);

static IntVector
	MP_(numReplicates);

static RealVector
	MP_(expConfigVars),
	MP_(expObservations),
	MP_(expStdDeviations),
	MP_(primaryRespFnWeights),
	MP_(nonlinearEqTargets),
	MP_(nonlinearIneqLowerBnds),
	MP_(nonlinearIneqUpperBnds),
	MP_(fdGradStepSize),
	MP_(fdHessStepSize),
	MP_(primaryRespFnScales),
	MP_(nonlinearEqScales),
	MP_(nonlinearIneqScales);

static Resp_mp_lit
	MP2(gradientType,analytic),
	MP2(gradientType,mixed),
	MP2(gradientType,none),
	MP2(gradientType,numerical),
	MP2(hessianType,analytic),
	MP2(hessianType,mixed),
	MP2(hessianType,none),
	MP2(hessianType,numerical),
	MP2(hessianType,quasi),
	MP2(intervalType,central),
	MP2(intervalType,forward),
	MP2(methodSource,dakota),
	MP2(methodSource,vendor),
        MP2(fdGradStepType,absolute),
        MP2(fdGradStepType,bounds),
        MP2(fdGradStepType,relative),
        MP2(fdHessStepType,absolute),
        MP2(fdHessStepType,bounds),
        MP2(fdHessStepType,relative),
	MP2(quasiHessianType,bfgs),
	MP2(quasiHessianType,damped_bfgs),
	MP2(quasiHessianType,sr1);

static String
	MP_(expDataFileName),
        MP_(idResponses);

static StringArray
	MP_(nonlinearEqScaleTypes),
	MP_(nonlinearIneqScaleTypes),
	MP_(primaryRespFnScaleTypes),
	MP_(primaryRespFnSense),
	MP_(responseLabels);

static bool
	MP_(centralHess),
	MP_(expDataFileAnnotated),
	MP_(ignoreBounds);

static size_t
	MP_(numExpStdDeviations),
	MP_(numExpConfigVars),
        MP_(numExperiments),
	MP_(numLeastSqTerms),
	MP_(numNonlinearEqConstraints),
	MP_(numNonlinearIneqConstraints),
	MP_(numObjectiveFunctions),
	MP_(numResponseFunctions);

#undef MP2
#undef MP_

#define MP_(x) DataStrategyRep::* strategy_mp_##x = &DataStrategyRep::x
#define MP2(x,y) strategy_mp_##x##_##y = {&DataStrategyRep::x,#y}

static Real
      //MP_(hybridProgThresh),
	MP_(hybridLSProb);

static RealVector
	MP_(concurrentParameterSets);

static Strategy_mp_lit
      //MP2(hybridCollabType,abo),
      //MP2(hybridCollabType,hops),
	MP2(hybridType,collaborative),
	MP2(hybridType,embedded),
	MP2(hybridType,sequential),
      //MP2(hybridType,sequential_adaptive),
	MP2(iteratorScheduling,master),
	MP2(iteratorScheduling,peer),
      //MP2(iteratorScheduling,peer_dynamic),
      //MP2(iteratorScheduling,peer_static),
	MP2(strategyType,hybrid),
	MP2(strategyType,multi_start),
	MP2(strategyType,pareto_set),
	MP2(strategyType,single_method);

static String
	MP_(hybridGlobalMethodPointer),
	MP_(hybridLocalMethodPointer),
	MP_(methodPointer),
        MP_(resultsOutputFile),
	MP_(tabularDataFile);

static StringArray
	MP_(hybridMethodList);

static bool
	MP_(graphicsFlag),
        MP_(resultsOutputFlag),
	MP_(tabularDataFlag);

static int
	MP_(concurrentRandomJobs),
	MP_(concurrentSeed),
        MP_(iteratorServers),
        MP_(outputPrecision);

#undef MP2
#undef MP_

#define MP_(x) DataVariablesRep::* var_mp_##x = &DataVariablesRep::x
#define MP2s(x,y) var_mp_##x = {&DataVariablesRep::x,y}
#define VP_(x) *Var_Info::* var_mp_Var_Info_##x = &Var_Info::x
#define Vtype(x,y) var_mp_##x##_##y = {&DataVariablesRep::x,y}

static size_t
	MP_(numBetaUncVars),
	MP_(numBinomialUncVars),
	MP_(numContinuousDesVars),
	MP_(numContinuousIntervalUncVars),
	MP_(numContinuousStateVars),
	MP_(numDiscreteDesRangeVars),
	MP_(numDiscreteDesSetIntVars),
	MP_(numDiscreteDesSetRealVars),
	MP_(numDiscreteIntervalUncVars),
	MP_(numDiscreteStateRangeVars),
	MP_(numDiscreteStateSetIntVars),
	MP_(numDiscreteStateSetRealVars),
	MP_(numDiscreteUncSetIntVars),
	MP_(numDiscreteUncSetRealVars),
	MP_(numExponentialUncVars),
	MP_(numFrechetUncVars),
	MP_(numGammaUncVars),
	MP_(numGeometricUncVars),
	MP_(numGumbelUncVars),
	MP_(numHistogramBinUncVars),
	MP_(numHistogramPtUncVars),
	MP_(numHyperGeomUncVars),
	MP_(numLognormalUncVars),
	MP_(numLoguniformUncVars),
	MP_(numNegBinomialUncVars),
	MP_(numNormalUncVars),
	MP_(numPoissonUncVars),
	MP_(numTriangularUncVars),
	MP_(numUniformUncVars),
	MP_(numWeibullUncVars);

static IntVector
	VP_(ddsi),
	VP_(DIlb),
	MP_(discreteDesignRangeLowerBnds),
	MP_(discreteDesignRangeUpperBnds),
	MP_(discreteDesignRangeVars),
	MP_(discreteDesignSetIntVars),
	MP_(discreteIntervalUncVars),
        MP_(discreteStateRangeLowerBnds),
	MP_(discreteStateRangeUpperBnds),
	MP_(discreteStateRangeVars),
	MP_(discreteStateSetIntVars),
	MP_(discreteUncSetIntVars),
	VP_(DIub),
        VP_(dssi),
        VP_(dusi);

static IntArray
	VP_(nddsi),
	VP_(nddsr),
	VP_(ndssi),
	VP_(ndssr),
	VP_(ndusi),
	VP_(ndusr),
	VP_(nhbp),
	VP_(nhpp),
	VP_(nCI),
	VP_(nDI);

static RealVector
	MP_(betaUncLowerBnds),
	MP_(betaUncUpperBnds),
	MP_(betaUncVars),
        MP_(binomialUncProbPerTrial),
        MP_(continuousDesignLowerBnds),
	MP_(continuousDesignUpperBnds),
	MP_(continuousDesignVars),
	MP_(continuousDesignScales),
	MP_(continuousIntervalUncVars),
	MP_(continuousStateLowerBnds),
	MP_(continuousStateUpperBnds),
	MP_(continuousStateVars),
	MP_(discreteDesignSetRealVars),
	MP_(discreteStateSetRealVars),
	MP_(discreteUncSetRealVars),
	MP_(frechetUncBetas),
	MP_(frechetUncVars),
        MP_(geometricUncProbPerTrial),
	MP_(gumbelUncBetas),
	MP_(gumbelUncVars),
	MP_(histogramBinUncVars),
	MP_(histogramPointUncVars),
        MP_(negBinomialUncProbPerTrial),
	MP_(normalUncLowerBnds),
	MP_(normalUncMeans),
	MP_(normalUncUpperBnds),
	MP_(normalUncVars),
	MP_(triangularUncModes),
	MP_(triangularUncVars),
	MP_(uniformUncVars),
	MP_(weibullUncVars),
	VP_(ddsr),
	VP_(dssr),
	VP_(dusr),
	VP_(CIlb),
	VP_(CIub),
	VP_(CIp),
	VP_(DIp),
	VP_(DSIp),
	VP_(DSRp),
	VP_(hba),
	VP_(hbo),
	VP_(hbc),
	VP_(hpa),
	VP_(hpc),
	VP_(ucm);

static String
	MP_(idVariables);

static StringArray
	MP_(continuousDesignLabels),
	MP_(continuousDesignScaleTypes),
	MP_(continuousStateLabels),
	MP_(discreteDesignRangeLabels),
	MP_(discreteDesignSetIntLabels),
	MP_(discreteDesignSetRealLabels),
	MP_(discreteStateRangeLabels),
	MP_(discreteStateSetIntLabels),
	MP_(discreteStateSetRealLabels);

static Var_brv
	MP2s(betaUncAlphas,0.),
	MP2s(betaUncBetas,0.),
	MP2s(exponentialUncBetas,0.),
	MP2s(exponentialUncVars,0.),
	MP2s(frechetUncAlphas,2.),
	MP2s(gammaUncAlphas,0.),
	MP2s(gammaUncBetas,0.),
	MP2s(gammaUncVars,0.),
	MP2s(gumbelUncAlphas,0.),
	MP2s(lognormalUncErrFacts,1.),
	MP2s(lognormalUncLambdas,0.),
	MP2s(lognormalUncLowerBnds,0.),
	MP2s(lognormalUncMeans,0.),
	MP2s(lognormalUncStdDevs,0.),
	MP2s(lognormalUncUpperBnds,DBL_MAX),
	MP2s(lognormalUncVars,0.),
	MP2s(lognormalUncZetas,0.),
	MP2s(loguniformUncLowerBnds,0.),
	MP2s(loguniformUncUpperBnds,DBL_MAX),
	MP2s(loguniformUncVars,0.),
	MP2s(normalUncStdDevs,0.),
	MP2s(poissonUncLambdas,0.),
	MP2s(triangularUncLowerBnds, -DBL_MAX),
	MP2s(triangularUncUpperBnds, DBL_MAX),
	MP2s(uniformUncLowerBnds, -DBL_MAX),
	MP2s(uniformUncUpperBnds, DBL_MAX),
	MP2s(weibullUncAlphas,0.),
	MP2s(weibullUncBetas,0.);

static Var_biv
	MP2s(binomialUncNumTrials,0),
	MP2s(binomialUncVars,0),
	MP2s(geometricUncVars,0),
	MP2s(hyperGeomUncNumDrawn,0),
	MP2s(hyperGeomUncSelectedPop,0),
	MP2s(hyperGeomUncTotalPop,0),
	MP2s(hyperGeomUncVars,0),
	MP2s(negBinomialUncNumTrials,0),
	MP2s(negBinomialUncVars,0),
	MP2s(poissonUncVars,0);

static Var_mp_type
	Vtype(varsDomain,MIXED_DOMAIN),
	Vtype(varsDomain,RELAXED_DOMAIN),
	Vtype(varsView,ALL_VIEW),
	Vtype(varsView,DESIGN_VIEW),
	Vtype(varsView,UNCERTAIN_VIEW),
	Vtype(varsView,ALEATORY_UNCERTAIN_VIEW),
	Vtype(varsView,EPISTEMIC_UNCERTAIN_VIEW),
        Vtype(varsView,STATE_VIEW);

#undef VP_
#undef MP2s
#undef MP_
#undef Vtype

} // namespace Dakota

#undef Warn
#undef Squawk

#define N_ifm(x,y)	NIDRProblemDescDB::iface_##x,&iface_mp_##y
#define N_ifm3(x,y,z)	NIDRProblemDescDB::iface_##x,y,NIDRProblemDescDB::iface_##z
#define N_mdm(x,y)	NIDRProblemDescDB::method_##x,&method_mp_##y
//#define N_mdf(x,y)	N_mdm(x,y),NIDRProblemDescDB::method_##x##_final
#define N_mdm3(x,y,z)	NIDRProblemDescDB::method_##x,y,NIDRProblemDescDB::method_##z
#define N_mom(x,y)	NIDRProblemDescDB::model_##x,&model_mp_##y
#define N_mof(x,y)	N_mom(x,y),NIDRProblemDescDB::model_##x##_final
#define N_mom3(x,y,z)	NIDRProblemDescDB::model_##x,y,NIDRProblemDescDB::model_##z
#define N_rem(x,y)	NIDRProblemDescDB::resp_##x,&resp_mp_##y
#define N_rem3(x,y,z)	NIDRProblemDescDB::resp_##x,y,NIDRProblemDescDB::resp_##z
#define N_stm(x,y)	NIDRProblemDescDB::strategy_##x,&strategy_mp_##y
#define N_vae(x,y)	NIDRProblemDescDB::var_##x,(void*)y
#define N_vam(x,y)	NIDRProblemDescDB::var_##x,&var_mp_##y
#define N_vam3(x,y,z)	NIDRProblemDescDB::var_##x,y,NIDRProblemDescDB::var_##z

#include "NIDR_keywds.hpp"
