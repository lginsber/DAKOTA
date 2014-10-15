/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:       OutputManager
//- Description: Implementation code for the OutputManager class
//- Owner:       Brian Adams
//- Checked by:

#include <boost/algorithm/string/predicate.hpp>
#include "dakota_global_defs.hpp"
#include "OutputManager.hpp"
#include "ProgramOptions.hpp"
#include "ProblemDescDB.hpp"
#include "ParamResponsePair.hpp"
#include "PRPMultiIndex.hpp"
#include "DakotaGraphics.hpp"
#include "ResultsManager.hpp"
#include "DakotaBuildInfo.hpp"

#include "dakota_tabular_io.hpp"

namespace Dakota {

// Note: MSVC requires these externs defined outside any function
extern PRPCache data_pairs;
extern ResultsManager iterator_results_db;

// BMA TODO: consider removing or reimplementing
/** Heartbeat function provided by dakota_filesystem_utils; pass
    output interval in seconds, or -1 to use $DAKOTA_HEARTBEAT */
void start_dakota_heartbeat(int);


OutputManager::OutputManager():
  graph2DFlag(false), tabularDataFlag(false), resultsOutputFlag(false), 
  worldRank(0), mpirunFlag(false), 
  redirCalled(false), 
  coutRedirector(dakota_cout, &std::cout), 
  cerrRedirector(dakota_cerr, &std::cerr),
  graphicsCntr(1), tabularCntrLabel("eval_id"), outputLevel(NORMAL_OUTPUT)
{  /* empty ctor */  }


/// Only get minimal information off ProgramOptions as may be updated
/// later by broadcast
OutputManager::
OutputManager(const ProgramOptions& prog_opts, int dakota_world_rank,
	      bool dakota_mpirun_flag):
  graph2DFlag(false), tabularDataFlag(false), resultsOutputFlag(false),
  worldRank(dakota_world_rank), mpirunFlag(dakota_mpirun_flag), 
  redirCalled(false), 
  coutRedirector(dakota_cout, &std::cout), 
  cerrRedirector(dakota_cerr, &std::cerr),
  graphicsCntr(1), tabularCntrLabel("eval_id"), outputLevel(NORMAL_OUTPUT)
{
  //  if output file specified, redirect immediately, possibly rebind later
  if (worldRank == 0 && prog_opts.user_stdout_redirect())
    coutRedirector.push_back(prog_opts.output_file());

  //  if error file specified, redirect immediately, possibly rebind later
  if (worldRank == 0 && prog_opts.user_stderr_redirect())
    cerrRedirector.push_back(prog_opts.error_file());

  if (!mpirunFlag
      // || (mpirunFlag && worldRank == 0) 
      )
    start_dakota_heartbeat(-1); // -1 ==> take interval from $DAKOTA_HEARTBEAT
}


OutputManager::~OutputManager()
{
  close_streams();
}


void OutputManager::close_streams()
{
  // cout/cerr will be restored to default when the redirector is destroyed

  // any remaining restart files will be closed at the destructor...
  //restartDestinations.clear();

  // After completion of timings in ParallelLibrary... 
  //
  // Don't need rank-based protection as will only be closed if they
  // were opened.  Do need user flag protection, otherwise dummy output
  // manager will close the global streams a second time wrongly.
  // BMA TODO: consider adding flag to track whether active...
  if (graph2DFlag || tabularDataFlag) {
    if (graph2DFlag)
      dakotaGraphics.close();
    // only close tabular stream if initialization was previously performed
    // not an error when not open so all ranks can call this
    if (tabularDataFlag && tabularDataFStream.is_open())
      tabularDataFStream.close();

    // could omit entirely or do this unconditionally...
    graphicsCntr = 1;
  }
}


void OutputManager::parse(const ProblemDescDB& problem_db)
{
  graph2DFlag = problem_db.get_bool("environment.graphics");
  tabularDataFlag = problem_db.get_bool("environment.tabular_graphics_data");
  tabularDataFile = problem_db.get_string("environment.tabular_graphics_file");
  resultsOutputFlag = problem_db.get_bool("environment.results_output");
  resultsOutputFile = problem_db.get_string("environment.results_output_file");

  int db_write_precision = problem_db.get_int("environment.output_precision");
  if (db_write_precision > 0) {  // assign global write_precision
    if (db_write_precision > 16) {
      std::cout << "\nWarning: requested output_precision exceeds DAKOTA's "
		<< "internal precision;\n         resetting to 16."<< std::endl;
      write_precision = 16;
    }
    else
      write_precision = db_write_precision;
  }
}


void OutputManager::startup_message(const String& start_msg) 
{ startupMessage = start_msg; }


void OutputManager::
push_output_tag(const String& iterator_tag, const ProgramOptions& prog_opts,
		bool force_cout_redirect) 
{
  fileTags.push_back(iterator_tag); 

  String file_tag = std::accumulate(fileTags.begin(), fileTags.end(), String());
  
  if (outputLevel >= DEBUG_OUTPUT)
    std::cout << "DEBUG: Rank " << worldRank << " pushing output tag; new tag '"
	      << file_tag << "'" << std::endl;

  if (force_cout_redirect || prog_opts.user_stdout_redirect())
    coutRedirector.push_back(prog_opts.output_file() + file_tag);
  else  // output file remains same as before (possibly cout)
    coutRedirector.push_back();

  if (!prog_opts.error_file().empty())
    cerrRedirector.push_back(prog_opts.error_file() + file_tag);
  else  // error file remains same as before (possibly cee)
    cerrRedirector.push_back();

  // We always write a restart file; need to be careful to only read
  // if first time encountering this name
  // TODO: review whether all read/write should be tagged
  bool read_restart_flag = !prog_opts.read_restart_file().empty();
  read_write_restart(read_restart_flag, 
		     prog_opts.read_restart_file() + file_tag,
		     prog_opts.stop_restart_evals(),
		     prog_opts.write_restart_file() + file_tag);

  // for now protect results DB from more than one call
  if (!redirCalled) {
    if (resultsOutputFlag)
      iterator_results_db.initialize(resultsOutputFile + file_tag);
    redirCalled = true;
  }

}


/** For now this assumes the tag is .<int> */
void OutputManager::pop_output_tag()
{
  if (fileTags.empty()) {
    Cerr << "\nWarning: Rank " << worldRank 
	 << " attempting to pop non-existent output tag." << std::endl;
    return;
  }

  fileTags.pop_back();

  if (outputLevel >= DEBUG_OUTPUT) {
    String file_tag = std::accumulate(fileTags.begin(), fileTags.end(), String());
    std::cout << "DEBUG: Rank " << worldRank << " popping output tag; new tag '"
	      << file_tag << "'" << std::endl;
  }

  // the redirector will rebind to previous stream when popped
  coutRedirector.pop_back();
  cerrRedirector.pop_back();

  // the restart file will get closed if this pops the last reference to it
  if (restartDestinations.empty())
    Cerr << "\nWarning: Attempt to pop non-existent restart destination!"
	 << std::endl;
  else
    restartDestinations.pop_back();
}


void OutputManager::output_version(std::ostream& os) const
{
  // Version is always output to Cout

  if (worldRank == 0) {
    std::string version_info("Dakota version ");

    // release version, possibly stable with '+'
    version_info += DakotaBuildInfo::get_release_num(); 
    if (boost::ends_with(DakotaBuildInfo::get_release_num(), "+"))
      version_info += " (stable)";
    version_info += " released " + DakotaBuildInfo::get_release_date() + ".\n"; 

    // subversion revision
    version_info += "Subversion revision " 
      + DakotaBuildInfo::get_rev_number()
      + " built " + DakotaBuildInfo::get_build_date()
      + " " + DakotaBuildInfo::get_build_time() + ".";

    os << version_info << std::endl;
  }
}


void OutputManager::output_startup_message(std::ostream& os) const 
{
  if (worldRank == 0) {
    output_version(os);
    // Generate the startup header, now that streams are potentially reassigned
    os << startupMessage << '\n'; 
    std::time_t curr_time = std::time(NULL);
    std::string pretty_time(std::asctime(std::localtime(&curr_time))); 
    // asctime appends a newline, but use the endl to force flush
    os << "Start time: " << pretty_time << std::endl;
  }
}



void OutputManager::
output_helper(const String& message, std::ostream &os) const
{
  if (worldRank == 0)
    os << message << std::endl;
}


void OutputManager::append_restart(const ParamResponsePair& prp)
{
  if (restartDestinations.empty()) {
    Cerr << "\nError: Attempt to append to restart file when not open."
	 << std::endl;
    abort_handler(-1);
  }
  boost::shared_ptr<RestartWriter> rst_writer = restartDestinations.back();
  rst_writer->append_prp(prp);
  // flush is critical so we have a complete restart record should Dakota abort
  rst_writer->flush();
}


/** Opens the tabular data file stream and prints headings, one for
    each active continuous and discrete variable and one for each response
    function, using the variable and response function labels. This
    tabular data is used for post-processing of DAKOTA results in
    Matlab, Tecplot, etc. */
void OutputManager::
create_tabular_datastream(const Variables& vars, const Response& response)
{
  // For output/restart/tabular data, all Iterator masters stream
  // output so tabular graphics files need to be tagged

  // tabular data file set up
  // prevent multiple opens of tabular_data_file
  if (!tabularDataFStream.is_open()) {
    String file_tag = std::accumulate(fileTags.begin(), fileTags.end(), String());
    TabularIO::open_file(tabularDataFStream, tabularDataFile + file_tag, 
			 "DakotaGraphics");
  }

  bool active_only = false;
  bool response_labels = true;
  bool annotated = true;  // tabular graphics data only supports annotated
  if (annotated)
    TabularIO::write_header_tabular(tabularDataFStream, vars, response,
				    tabularCntrLabel, active_only,
				    response_labels);
}


/** Adds data to each 2d plot and each tabular data column (one for
    each active variable and for each response function).
    graphicsCntr is used for the x axis in the graphics and the first
    column in the tabular data.  */
void OutputManager::
add_datapoint(const Variables& vars, const Response& response)
{
  // If the response data only contains derivative info, then there are no
  // response function values to record in either the graphics window or the
  // tabular data file.
  bool plot_data = false;
  const ShortArray& asv = response.active_set_request_vector();
  int i, num_fns = asv.size();
  for (i=0; i<num_fns; ++i) {
    if (asv[i] & 1) {
      plot_data = true;
      break;
    }
  }
  if (!plot_data)
    return;
  
  // post to the X graphics plots
  dakotaGraphics.add_datapoint(graphicsCntr, vars, response);
  
  // whether the file is open, not whether the user asked
  if (tabularDataFStream.is_open()) {
    // In the tabular graphics file, only the *active* variables are tabulated
    // for top level evaluations/iterations in the strategy.  This differs from
    // the "to_tabular" option of dakota_restart_util, which tabulates all
    // variables for all application interface evaluations.

    // NOTE: could add ability to monitor response data subsets based on
    // user specification.
    bool active_only = false;
    bool write_responses = true;
    TabularIO::write_data_tabular(tabularDataFStream, vars, response,
				  graphicsCntr, active_only, write_responses);
  }

  // Only increment the graphics counter if posting data (incrementing on every
  // call regardless of data posting causes skipping in the response plots).
  ++graphicsCntr;
}


void OutputManager::tabular_counter_label(const std::string& label)
{ tabularCntrLabel = label; }


void OutputManager::graphics_counter(int cntr)
{ graphicsCntr = cntr; }


int OutputManager::graphics_counter() const
{ return graphicsCntr; }


void OutputManager::read_write_restart(bool read_restart_flag,
				       const String& read_restart_filename,
				       size_t stop_restart_evals,
				       const String& write_restart_filename)
{
  // true if this filename has already been read and opened for writing
  // this restart name has already been processed, push it again
  if ( !restartDestinations.empty() &&
       write_restart_filename == restartDestinations.back()->filename() ) {
    restartDestinations.push_back(restartDestinations.back());
    return;
  }

  // Conditionally process the evaluations from the restart file
  PRPCache read_pairs;
  if (read_restart_flag) {
    
    std::ifstream restart_input_fs(read_restart_filename.c_str(), 
				   std::ios::binary);
    if (!restart_input_fs.good()) {
      Cerr << "\nError: could not open restart file " << read_restart_filename
	   << std::endl;
      abort_handler(-1);
    }
    boost::archive::binary_iarchive restart_input_archive(restart_input_fs);

    // The -stop_restart input for restricting the number of evaluations read
    // in from the restart file is very useful when the last few evaluations in
    // a run were corrupted.  Note that the desired -stop_restart setting may
    // differ from the evaluation number in the previous run since detected 
    // duplicates are included in Interface::evalIdCntr, but are not written
    // to the restart file!
    if (stop_restart_evals)// cmd_line_handler rtns 0 if no setting
      Cout << "Stopping restart file processing at "
	   << stop_restart_evals << " evaluations." << std::endl;

    int cntr = 0;
    while ( restart_input_fs.good() && !restart_input_fs.eof() && 
            (!stop_restart_evals ||
	     cntr < stop_restart_evals) ) {
      // Use default constr. & rely on Variables::read(BiStream&)
      // & Response::read(BiStream&) to resize vars and response.
      ParamResponsePair current_pair;
      try { 
	restart_input_archive & current_pair;
      }
      catch(const boost::archive::archive_exception& e) {
	Cerr << "\nError reading restart file (boost::archive exception):\n" 
	     << e.what() << std::endl;
	abort_handler(-1);
      }
      catch(const std::string& err_msg) {
        Cout << "\nWarning reading restart file: " << err_msg << std::endl;
        break;
      }

      read_pairs.insert(current_pair); 
      ++cntr;
      Cout << "\n------------------------------------------\nRestart record "
	   << std::setw(4) << cntr << "  (evaluation id " << std::setw(4)
	   << current_pair.eval_id() << "):"
	   << "\n------------------------------------------\n" << current_pair;
      // Note: interface id printed in ParamResponsePair::write(ostream&)

      restart_input_fs.peek(); // peek to force EOF if the last record was read
    }
    restart_input_fs.close();
    Cout << "Restart file processing completed: " << cntr
	 << " evaluations retrieved.\n";
  }


  // Always write a restart log file.  Assign the write_restart stream to
  // the filename specified by the user on the dakota command line.  If a
  // write_restart file is not specified, "dakota.rst" is the default.

  // It would be desirable to suppress the creation of the restart file
  // altogether if the user has explicitly deactivated this feature; however
  // this is problematic for 2 reasons: (1) problem_db is not readily available
  // (except in init_iterator_communicators()), and (2) the "deactivate
  // restart_file" specification is linked to the interface and therefore should
  // be enforced per interface, whereas there is only one parallel lib instance.
  //if (!deactivateRestartFlag) {

  // Previously we supported append to an existing restart file.  With
  // Boost serialization, there's no easy way to append to a file (all
  // writes must occur with a single output archive instance).  This
  // also improves behavior with stop_restart, as now only the desired
  // evals are rewritten, omitting any corrupt data at the end of file.

  if (write_restart_filename == read_restart_filename)
    Cout << "Overwriting existing restart file " << write_restart_filename 
	 << std::endl;
  else
    Cout << "Writing new restart file " << write_restart_filename << std::endl;

  // create a new restart destination
  boost::shared_ptr<RestartWriter> 
    rst_writer(new RestartWriter(write_restart_filename));
  restartDestinations.push_back(rst_writer);

  // Write any processed records from the old restart file to the new file.
  // This prevents the situation where good data from an initial run and a 
  // restart run are in separate files.  By keeping all of the saved data in
  // 1 file, restarts can be chained together indefinitely.
  //
  // "View" the read_pairs cache as a collection ordered by eval_id

  if (!read_pairs.empty()) {
    // don't use a const iterator as PRP does not have a const serialize fn
    PRPCacheIter it, it_end = read_pairs.end();
    for (it = read_pairs.begin(); it != it_end; ++it) {
      rst_writer->append_prp(*it);

      // Distinguish restart evals in memory by negating their eval ids;
      // positive ids could be misleading if inconsistent with the progression
      // of a restarted run (resulting in different evaluations that share the
      // same evalInterfaceIds).  Moreover, they may not be unique within the
      // restart DB in the presence of overlay/concatenation of multiple runs.
      //   id > 0 for unique evals from current execution (in data_pairs)
      //   id = 0 for evals from file import (not in data_pairs)
      //   id < 0 for non-unique evals from restart (in data_pairs)
      int restart_eval_id = it->eval_id();
      if (restart_eval_id > 0) {
	ParamResponsePair pair(*it); // shallow vars/resp copy, deep ids copy
	pair.eval_id(-restart_eval_id);
	data_pairs.insert(pair);
      }
      else
	data_pairs.insert(*it);
    }

    // flush is critical so we have a complete restart record in case of abort
    rst_writer->flush();
  }

  //}
}


OutputWriter::OutputWriter(std::ostream* output_stream)
{ outputStream = output_stream; }

OutputWriter::OutputWriter(const String& output_filename)
{
  outputFS.open(output_filename.c_str(), std::ios::out);
  if (!outputFS.good()) {
    Cerr << "\nError opening output file '" << output_filename << "'" 
	 << std::endl;
    abort_handler(-1);
  }
  outputStream = &outputFS;
}

const String& OutputWriter::filename() const
{ return outputFilename; }

 
std::ostream* OutputWriter::output_stream()
{ return outputStream; }


ConsoleRedirector::
ConsoleRedirector(std::ostream* & dakota_stream, std::ostream* default_dest):
  ostreamHandle(dakota_stream), defaultOStream(default_dest)
{ /* empty ctor */ }


ConsoleRedirector::~ConsoleRedirector()
{
  ostreamHandle = defaultOStream;
}


void ConsoleRedirector::push_back()
{
  if (ostreamDestinations.empty()) {
    boost::shared_ptr<OutputWriter> 
      out_writer_ptr(new OutputWriter(defaultOStream));
    ostreamDestinations.push_back(out_writer_ptr);
  }
  else 
    ostreamDestinations.push_back(ostreamDestinations.back());
}


void ConsoleRedirector::push_back(const String& output_filename)
{
  if (output_filename.empty()) {
    push_back();
    return;
  }

  // if this is a new redirection, or a redirection with a new
  // filename, make a new stream, overwriting file contents
  if (ostreamDestinations.empty() || 
      ostreamDestinations.back()->filename() != output_filename) {
    boost::shared_ptr<OutputWriter> 
      out_writer_ptr(new OutputWriter(output_filename));
    ostreamDestinations.push_back(out_writer_ptr);
  }
  // otherwise keep using the same stream
  else
    ostreamDestinations.push_back(ostreamDestinations.back());

  ostreamHandle = ostreamDestinations.back()->output_stream();
}


void ConsoleRedirector::pop_back()
{
  if (!ostreamDestinations.empty())
    ostreamDestinations.pop_back();
  else 
    Cerr << "\nWarning: Attempt to pop non-existent console output destination!"
	 << std::endl;

  // if stack is empty, rebind to the default
  if (ostreamDestinations.empty())
    ostreamHandle = defaultOStream;
  else
    ostreamHandle = ostreamDestinations.back()->output_stream();
}


RestartWriter::RestartWriter(const String& write_restart_filename):
  restartOutputFilename(write_restart_filename),
  restartOutputFS(restartOutputFilename.c_str(), std::ios::binary),
  restartOutputArchive(restartOutputFS)
{  /* empty ctor */  }


const String& RestartWriter::filename()
{ return restartOutputFilename; }


void RestartWriter::append_prp(const ParamResponsePair& prp_in)
{ restartOutputArchive & prp_in; }


void RestartWriter::flush()
{ restartOutputFS.flush(); }


} //namespace Dakota

