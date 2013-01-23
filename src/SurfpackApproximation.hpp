/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright (c) 2010, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

//- Class:        SurfpackApproximation 
//- Description:  Class for polynomial response surface approximation
//-               
//- Owner:        Brian Adams, Sandia National Laboratories
 

#ifndef SURFPACK_APPROXIMATION_H 
#define SURFPACK_APPROXIMATION_H 

#include "DakotaApproximation.hpp"


// Pertinent classes from Surfpack
class SurfData;
class SurfpackModel;
class SurfpackModelFactory;
template< typename T > class SurfpackMatrix;

namespace Dakota {


/// Derived approximation class for Surfpack approximation classes.
/// Interface between Surfpack and Dakota.

/** The SurfpackApproximation class is the interface between Dakota
    and Surfpack.  Based on the information in the ProblemDescDB that
    is passed in through the constructor, SurfpackApproximation builds
    a Surfpack Surface object that corresponds to one of the following
    data-fitting techniques: polynomial regression, kriging,
    artificial neural networks, radial basis function network, or
    multivariate adaptaive regression splines (MARS). */

class SurfpackApproximation: public Approximation
{
public:

  //
  //- Heading: Constructor and destructor
  //

  /// default constructor
  SurfpackApproximation();
  /// alternate constructor
  SurfpackApproximation(const String& approx_type,
			const UShortArray& approx_order, size_t num_vars,
			short data_order);
  /// standard constructor: Surfpack surface of appropriate type will be created
  SurfpackApproximation(const ProblemDescDB& problem_db, size_t num_vars);
  /// destructor
  ~SurfpackApproximation();

protected:

  //
  //- Heading: Virtual function redefinitions
  //

  int min_coefficients() const;
  int recommended_coefficients() const;
  //int num_constraints() const; // use default implementation

  /// SurfData object will be created from Dakota's SurrogateData,
  /// and the appropriate Surfpack build method will be invoked
  void build();

  // return #coefficients
  //const RealVector& approximation_coefficients() const;
  // set #coefficients
  //void approximation_coefficients(const RealVector& approx_coeffs);

  /// Return the value of the Surfpack surface for a given parameter vector x
  Real value(const Variables& vars);
  /// retrieve the approximate function gradient for a given parameter vector x
  const RealVector& gradient(const Variables& vars);
  /// retrieve the approximate function Hessian for a given parameter vector x
  const RealSymMatrix& hessian(const Variables& vars);
  /// retrieve the variance of the predicted value for a given parameter set x
  /// (KrigingModel only)
  Real prediction_variance(const Variables& vars);
  /// retrieve the diagnostic metric for the diagnostic type specified
  Real diagnostic(const String& metric_type);
  /// check if the diagnostics are available (true for the Surfpack types)
  bool diagnostics_available();

private:

  //
  //- Heading: Convenience functions
  //

  /// copy from SurrogateData to SurfPoint/SurfData
  SurfData* surrogates_to_surf_data();

  /// set the anchor point (including gradient and hessian if present)
  /// into surf_data
  void add_anchor_to_surfdata(SurfData& surf_data);

  /// add Pecos::SurrogateData::SurrogateData{Vars,Resp} to SurfData,
  /// accounting for buildDataOrder available
  void add_sd_to_surfdata(const Pecos::SurrogateDataVars& sdv,
			  const Pecos::SurrogateDataResp& sdr, short fail_code,
			  SurfData& surf_data);

  /// copy RealSymMatrix to SurfpackMatrix (Real type only)
  void copy_matrix(const RealSymMatrix& rsm,
		   SurfpackMatrix<Real>& surfpack_matrix);

  /// merge cv, div, and drv vectors into a single ra array
  void merge_variable_arrays(const RealVector& cv,  const IntVector& div,
			     const RealVector& drv, RealArray& ra);
  /// aggregate {continuous,discrete int,discrete real} variables 
  /// from SurrogateDataVars into ra
  void sdv_to_realarray(const Pecos::SurrogateDataVars& sdv, RealArray& ra);
  /// aggregate {active,all} {continuous,discrete int,discrete real}
  /// variables into ra
  void vars_to_realarray(const Variables& vars, RealArray& ra);

  //
  //- Heading: Data
  //

  /// order of polynomial approximation
  unsigned short approxOrder;

  // Vector representation of the Approximation (e.g., polynomial coefficients
  // for linear regression or trained neural network weights).  The format of
  // such a vector has not been defined for all Surfpack classes
  //RealVector coefficients;

  /// The native Surfpack approximation
  SurfpackModel* model;
  /// factory for the SurfpackModel instance
  SurfpackModelFactory* factory;
  /// The data used to build the approximation, in Surfpack format
  SurfData* surfData;
};


inline SurfpackApproximation::SurfpackApproximation():
  surfData(NULL), model(NULL), factory(NULL)
{ }


inline bool SurfpackApproximation::diagnostics_available()
{ return true; }


/* The value returned from this function is currently meaningless.
   \todo: Provide an appropriate list of coefficients for each surface type */
//inline const RealVector& SurfpackApproximation::
//approximation_coefficients() const
//{ return coefficients; }


//inline void SurfpackApproximation::
//approximation_coefficients(const RealVector& approx_coeffs)
//{ coefficients = approx_coeffs; }

} // namespace Dakota
#endif
