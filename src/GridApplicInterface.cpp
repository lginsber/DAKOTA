/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright (c) 2010, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:        GridApplicInterface
//- Description:  Class implementation
//- Owner:        Mike Eldred

#ifdef DAKOTA_GRID

#include <dlfcn.h>
#include <link.h>
#include <sys/types.h> // MAY REQUIRE ifndef(HPUX)
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h> // for usleep()
#endif
#include "DakotaResponse.hpp"
#include "ParamResponsePair.hpp"
#include "GridApplicInterface.hpp"
#include "dakota_system_defs.hpp"
#include "ProblemDescDB.hpp"

//#define TESTING_GRID

//extern "C" int start_grid_computing(char *analysis_driver_script,
//                                    char *params_file, char *results_file);
//extern "C" int perform_analysis(char *iteration_num);
//extern "C" int *get_jobs_completed();
//extern "C" int stop_grid_computing();
/*
This api returns the pointer to a pre-allocated fixed interger array.
The first element contains the number of returned iteration nums ;
the following elements contain the iteration nums.
    e.g.  3 1 3 5 
          returns 3 iteration nums 1, 3, 5
         
          0
          no iteration num returned
*/
#ifdef TESTING_GRID
Dakota::SysCallAnalysisCode* global_code;
#endif


namespace Dakota {


GridApplicInterface::
GridApplicInterface(const ProblemDescDB& problem_db):
  ApplicationInterface(problem_db), code(problem_db)
{ 
  void* handle = dlopen("foo.so", RTLD_NOW);
  if (!handle) {
    Cerr << "Problem loading shared object file: foo.so" << std::endl;
    abort_handler(-1);
  }
  start_grid_computing
    = (start_grid_computing_t)(dlsym(handle, "start_grid_computing"));
  const char* error;
  if ((error = dlerror()) != NULL) {
    Cerr << "Problem loading start_grid_computing function: " << error
         << std::endl;
    abort_handler(-1);
  }
  stop_grid_computing
    = (stop_grid_computing_t)dlsym(handle, "stop_grid_computing");
  if ((error = dlerror()) != NULL) {
    Cerr << "Problem loading stop_grid_computing function: " << error
         << std::endl;
    abort_handler(-1);
  }
  perform_analysis = (perform_analysis_t)dlsym(handle, "perform_analysis");
  if ((error = dlerror()) != NULL) {
    Cerr << "Problem loading perform_analysis function: " << error << std::endl;
    abort_handler(-1);
  }
  get_jobs_completed = (get_jobs_completed_t)dlsym(handle,"get_jobs_completed");
  if ((error = dlerror()) != NULL) {
    Cerr << "Problem loading get_jobs_completed function: " << error
         << std::endl;
    abort_handler(-1);
  }
  int status = (*start_grid_computing)(code.program_names()[0].data(),
				       code.parameters_filename().data(), 
				       code.results_filename().data());
  // TODO - ERROR CHECKING
#ifdef TESTING_GRID
  global_code = &code;
#endif
  //code.fileSaveFlag=true;
}


GridApplicInterface::~GridApplicInterface() 
{
  int status = (*stop_grid_computing)();
  // TODO - ERROR CHECKING
}


void GridApplicInterface::
derived_map(const Variables& vars, const ActiveSet& set, Response& response,
	    int fn_eval_id)
{
  //
  // Launch the grid solver (asynchronously)
  //
  ParamResponsePair prp(vars, interfaceId, response, fn_eval_id);
  derived_map_asynch(prp);
  //
  // Call derived_synch until our id is in the set
  //
  PRPQueue prp_queue;
  prp_queue.push_back(prp);
  if (!completionSet.empty()) {
    Cerr << "derived_map - should start with an empty completion set\n";
    abort_handler(-1);
  }
  derived_synch(prp_queue); // rebuilds completionSet
  response = prp_queue.front().prp_response();
  completionSet.clear();
#if 0
  //
  // Read the params file and handle exceptions
  //
  try {
    if (evalCommRank == 0)
      code.read_results_files(response, fn_eval_id);
  }
  catch(String& err_msg) {
    // a String exception involves detection of an incomplete file/data
    // set.  In the synchronous case, there is no potential for an incomplete
    // file resulting from a race condition -> echo the error and abort.
    Cerr << err_msg << std::endl;
    abort_handler(-1);
  }
  catch(int fail_code) {
    // The approach here is to have catch(int) rethrow the exception to an 
    // outer catch (either the catch within manage_failure or a catch that 
    // calls manage_failure).
    throw;
  }
#endif
}


void GridApplicInterface::derived_map_asynch(const ParamResponsePair& pair)
{
  //
  // Write the params file
  //
  int fn_eval_id = pair.eval_id();
  code.define_filenames(fn_eval_id);
  code.write_parameters_files(pair.prp_parameters(), pair.active_set(),
			      pair.prp_response(),   fn_eval_id);
  //
  // Launch the grid solver
  //
  // TODO - ERROR CHECKING
  //
  String tmp;
  tmp += fn_eval_id;
  int status = (*perform_analysis)(tmp.data());
  //
  // Insert the evaluation ID into our current set
  //
  idSet.insert(fn_eval_id);
}


/// Convenience function for common code between wait and nowait case.
void GridApplicInterface::derived_synch_kernel(PRPQueue& prp_queue)
{
  //
  // Iterate through the set of requests
  //
  for (ISIter it=idSet.begin(); it!=idSet.end(); it++) {
    //
    // Test for existence of the results file(s) corresponding to this eval
    //
    int fn_eval_id = *it;
    bool err_msg_caught = false;
    const String& file_to_test = code.results_filename(fn_eval_id);
    if (grid_file_test(file_to_test)) {
      //
      // File exists; test for complete/valid set of results (an incomplete
      // set can result from a race condition in which Dakota is reading a
      // file that a simulator has not finished writing).  Response::read
      // throws a String exception if data is missing/misformatted.
      //
      ParamResponsePair pr_pair;
      bool found = lookup_by_eval_id(prp_queue, fn_eval_id, pr_pair);
      if (!found) {
	Cerr << "Error: failure in queue lookup within GridApplicInterface::"
	     << "derived_synch_kernel()." << std::endl;
	abort_handler(-1);
      }
      Response response = pr_pair.prp_response(); // shallow copy

      try { code.read_results_files(response, fn_eval_id); }
      catch(String& err_msg) {
	//
	// If a String exception (incomplete file) is caught, set
	// err_msg_caught to true so that processing is not performed below.
	// The for loop will then cycle through the other active asynch. evals.
	// before coming back to the one with the exception.  This should allow
	// file writing by a simulator to complete.  100 failures are currently
	// allowed for any fn_eval_id before it is assumed that the error is
	// real (not race condition related) and aborting.
	//
	err_msg_caught = true;
	IntShMIter map_iter = failCountMap.find(fn_eval_id);
	if (map_iter != failCountMap.end()) {
	  if (++map_iter->second > 100) {
	    Cerr << "Error: too many failed reads for file " << file_to_test
		 << "\n       check data format and completeness" << std::endl;
	    abort_handler(-1);
	  }
	}
	else
	  failCountMap[fn_eval_id] = 1;
#ifdef HAVE_UNISTD_H
	//
	// Sleep for 1 millisecond
	//
	usleep(1000);
#endif // HAVE_UNISTD_H
#ifdef ASYNCH_DEBUG
	Cerr << "Warning: exception caught in reading response file "
	     << file_to_test << "\nException = \"" << err_msg
	     << "\"\nException recovery: returning " << file_to_test
	     << " to processing queue.\n";
#endif
      }
      catch(int fail_code) {
	//
	// If an int exception ("fail" detected in results file) is caught,
	// call manage_failure which will either (1) repair the failure and
	// populate response, or (2) abort the run.
	//
	manage_failure(pr_pair.prp_parameters(), response.active_set(),
		       response, fn_eval_id);
      }
      //
      // Process successful results for this asynchronous eval.  Set
      // the response within the PRPair, remove entry in failCountMap, and
      // add evaluation id to completion set.
      //
      if (!err_msg_caught) {
	//pr_pair.prp_response(response);                    // not needed
	//replace_by_eval_id(prp_queue, fn_eval_id, pr_pair);// not needed
	completionSet.insert(fn_eval_id);
	failCountMap.erase(fn_eval_id); // if present
      }
    }
  }

#ifdef HAVE_UNISTD_H
  // reduce processor load from DAKOTA testing if jobs are not finishing
  if (completionSet.empty()) // no jobs completed in pass through entire set
    usleep(1000); // 1000 microseconds = 1 millisec
#endif // HAVE_UNISTD_H
  // Remove completed jobs from idSet
  for (ISIter it = completionSet.begin(); it != completionSet.end(); it++)
    idSet.erase(*it);
}


bool GridApplicInterface::grid_file_test(const String& root_file)
{
  // Unix stat utility returns 0 if successful in gathering file statistics,
  // -1 if there's an error (e.g., the file does not exist).
  struct stat buf; // see man pages for info available from buf (not used here)
  size_t num_progs = code.program_names().length();
  if ( num_progs > 1 && code.output_filter_name().empty() ) {
    char prog_num[16];

#ifdef __SUNPRO_CC
    // Sun Solaris has been observed to have problems with the final results
    // file existing before previous results files exist (I/O threading?)
    for (size_t i=0; i<num_progs; i++) {
      std::sprintf(prog_num, ".%d", i+1);
      String tagged_file;
      tagged_file = root_file + prog_num;
      if ( stat((char*)tagged_file.data(), &buf) == -1 )
        return false;
    }
    return true;
#else
    // Testing all files is usually overkill for sequential analyses.  It's only
    // really necessary to check the last tagged_file: root_file.[num_progs]
    std::sprintf(prog_num, ".%d", num_progs);
    String tagged_file;
    tagged_file = root_file + prog_num;
    return ( stat((char*)tagged_file.data(), &buf) == -1 ) ? false : true;
#endif // __SUNPRO_CC
  }
  else
    return ( stat((char*)root_file.data(), &buf) == -1 ) ? false : true;
}


#if defined(TESTING_GRID)
/** sample function prototype for launching grid computing */
extern "C" int start_grid_computing(char *analysis_driver_script, 
				    char *params_file, char *results_file)
{ return 0; }

/** sample function prototype for terminating grid computing */
extern "C" int stop_grid_computing()
{ return 0; }

/** sample function prototype for submitting a grid evaluation */
extern "C" int perform_analysis(char *iteration_num)
{ global_code->spawn_evaluation(true); return 0; }
#endif

} // namespace Dakota

#endif
