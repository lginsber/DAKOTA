#include "ExperimentDataUtils.hpp"
#include <iostream>

namespace Dakota {

void linear_interpolate_1d( RealMatrix &build_pts, RealVector &build_vals, 
			    RealMatrix &pred_pts, RealVector &pred_vals )
{
  if ( build_pts.numRows()!=1 )
    throw( std::runtime_error("build pts must be 1xN") );
  if ( pred_pts.numRows()!=1 )
    throw( std::runtime_error("build pts must be 1xM") );
  RealVector pred_pts_1d( Teuchos::View, pred_pts.values(), pred_pts.numCols() );
  RealVector build_pts_1d( Teuchos::View, build_pts.values(), 
			   build_pts.numCols() );
  int num_pred_pts = pred_pts.numCols();
  int num_build_pts = build_pts.numCols();
  if ( pred_vals.length() != num_pred_pts )
    pred_vals.sizeUninitialized( num_pred_pts );
  for ( int i = 0; i < num_pred_pts; i++ )
    {
      // enforce constant interpolation when interpolation is outside the
      // range of build_pts
      if ( pred_pts_1d[i] <= build_pts_1d[0] )
	pred_vals[i] = build_vals[0];
      else if ( pred_pts_1d[i] >= build_pts_1d[num_build_pts-1] )
	pred_vals[i] = build_vals[num_build_pts-1];
      else
	{
	  // assumes binary search returns index of the closest point in 
	  // build_pts to the left of pts(0,i)
	  int index = binary_search( pred_pts_1d[i], build_pts_1d );
	  pred_vals[i] = 
	    build_vals[index] + ( (build_vals[index+1]-build_vals[index] ) /
				  (build_pts_1d[index+1]-build_pts_1d[index]) ) *
	    ( pred_pts_1d[i] - build_pts_1d[index] );
	}
    }
}

void function_difference_1d( RealMatrix &coords_1, RealVector &values_1,
			     RealMatrix &coords_2, RealVector &values_2,
			     RealVector &diff )
{
  linear_interpolate_1d( coords_1, values_1, coords_2, diff );
  diff -= values_2;
}

CovarianceMatrix::CovarianceMatrix() : numDOF_(0), covIsDiagonal_(false) {}

CovarianceMatrix::CovarianceMatrix( const CovarianceMatrix &source ){
  copy( source );
}

CovarianceMatrix::~CovarianceMatrix(){}

void CovarianceMatrix::copy( const CovarianceMatrix &source ){
  numDOF_=source.numDOF_;
  covIsDiagonal_ = source.covIsDiagonal_;
  if ( source.covDiagonal_.length() > 0 )
    covDiagonal_=source.covDiagonal_;
  else if ( source.covMatrix_.numRows() > 0 )
  {
    covMatrix_ = source.covMatrix_;
    // Copy covariance matrix cholesky factor from source.
    // WARNING: Using Teuchos::SerialDenseSpdSolver prevents copying of
    // covariance cholesky factor so it must be done again here.
    factor_covariance_matrix(); // The source has already been factored in-place? RWH
  }
  else 
    // No covariance matrix is present in source
    return;
}

CovarianceMatrix& CovarianceMatrix::operator=( const CovarianceMatrix &source ){
  if(this == &source)
    return(*this); // Special case of source same as target
  
  copy( source );
}

void CovarianceMatrix::get_covariance( RealMatrix &cov ) const
{
  cov.shape( numDOF_, numDOF_ );
  if ( !covIsDiagonal_ ){
    for (int j=0; j<numDOF_; j++)
      for (int i=j; i<numDOF_; i++){
	cov(i,j) = covMatrix_(i,j);
	cov(j,i) = covMatrix_(i,j);
      }
  }else{
    for (int j=0; j<numDOF_; j++)
      cov(j,j) = covMatrix_(j,j);
  }
}

void CovarianceMatrix::set_covariance( const RealMatrix &cov )
{
  if ( cov.numRows() != cov.numCols() ){
    std::string msg = "Covariance matrix must be square.";
    throw( std::runtime_error( msg ) );
  }

  numDOF_ = cov.numRows();
  covMatrix_.shape(numDOF_);
  for (int j=0; j<numDOF_; j++)
    for (int i=j; i<numDOF_; i++){
      covMatrix_(i,j) = cov(i,j);
      covMatrix_(j,i) = cov(i,j);
    }

  covIsDiagonal_ = false;
  factor_covariance_matrix();
}

void CovarianceMatrix::set_covariance( Real cov )
{
  RealVector cov_diag(1,false);
  cov_diag[0] = cov;
  set_covariance( cov_diag );
}

void CovarianceMatrix::set_covariance( const RealVector & cov )
{
  covDiagonal_.sizeUninitialized( cov.length() );
  covDiagonal_.assign( cov );
  covIsDiagonal_ = true;
  numDOF_ = cov.length();
}

Real CovarianceMatrix::apply_covariance_inverse( const RealVector &vector ) const
{
  RealVector result;
  apply_covariance_inverse_sqrt( vector, result );
  return result.dot( result );
}

void CovarianceMatrix::factor_covariance_matrix()
  {
    covCholFactor_ = covMatrix_;
    covSlvr_.setMatrix( rcp(&covCholFactor_, false) );
    // Equilibrate must be false (which is the default) when chol factor is
    // used to compute inverse sqrt
    //covSlvr_.factorWithEquilibration(true); 
    int info = covSlvr_.factor();
    if ( info > 0 ){
      std::string msg = "The covariance matrix is not positive definite\n";
      throw( std::runtime_error( msg ) );
    }
    invert_cholesky_factor();
  };

void CovarianceMatrix::invert_cholesky_factor()
{
  // covCholFactor is a symmetric matrix so must make a copy
  // that only copies the correct lower or upper triangular part
  cholFactorInv_.shape( numDOF_, numDOF_ );
  if ( covCholFactor_.UPLO()=='L' ){
      for (int j=0; j<numDOF_; j++)
	for (int i=j; i<numDOF_; i++)
	  cholFactorInv_(i,j)=covCholFactor_(i,j);
    }else{
      for (int i=0; i<numDOF_; i++)
	for (int j=i; j<numDOF_; j++)
	  cholFactorInv_(i,j)=covCholFactor_(i,j);
    }
    
    int info = 0;
    Teuchos::LAPACK<int, Real> la;
    // always assume non_unit_diag even if covSlvr_.equilibratedA()==true
    la.TRTRI( covCholFactor_.UPLO(), 'N', 
	      numDOF_, cholFactorInv_.values(), 
	      cholFactorInv_.stride(), &info );
    if ( info > 0 ){
      std::string msg = "Inverting the covariance Cholesky factor failed\n";
      throw( std::runtime_error( msg ) );
    }
}

void CovarianceMatrix::apply_covariance_inverse_sqrt( const RealVector &vector,
						      RealVector &result ) const
{
  if ( vector.length() != numDOF_ ){
    std::string msg = "Vector and covariance are incompatible for ";
    msg += "multiplication.";
    throw( std::runtime_error( msg ) );
  }

  if ( result.length() != numDOF_)
    result.sizeUninitialized( numDOF_ );
  if ( covIsDiagonal_ ) {
    for (int i=0; i<numDOF_; i++)
      result[i] = vector[i] / std::sqrt( covDiagonal_[i] ); 
  }else{
    result.multiply( Teuchos::NO_TRANS, Teuchos::NO_TRANS, 
		     1.0, cholFactorInv_, vector, 0.0 );
  }
}

void CovarianceMatrix::apply_covariance_inverse_sqrt_to_gradients( 
          const RealMatrix &gradients,
	  RealMatrix &result ) const
{
  if ( gradients.numCols() != numDOF_ ){
    std::string msg = "Gradients and covariance are incompatible for ";
    msg += "multiplication.";
    throw( std::runtime_error( msg ) );
  }
  int num_grads = gradients.numRows();
  if ( ( result.numRows() != num_grads ) || ( result.numCols() != numDOF_ ) )
    result.shapeUninitialized( num_grads, numDOF_ );
  if ( covIsDiagonal_ ) {
    for (int j=0; j<numDOF_; j++)
      for (int i=0; i<num_grads; i++)
	result(i,j) = gradients(i,j) / std::sqrt( covDiagonal_[j] ); 
  }else{
    // Let A = cholFactorInv_ and B = gradients. We want to compute C' = AB'
    // so compute C = (AB')' = BA'
    result.multiply( Teuchos::NO_TRANS, Teuchos::TRANS, 
		     1.0, gradients, cholFactorInv_, 0.0 );
  }
}

void CovarianceMatrix::apply_covariance_inverse_sqrt_to_hessian( 
			  RealSymMatrixArray &hessians, int start ) const
{
  if ( (hessians.size()-start) < numDOF_ ){
    std::string msg = "Hessians and covariance are incompatible for ";
    msg += "multiplication.";
    throw( std::runtime_error( msg ) );
  }
  int num_grads = hessians[0].numRows();
  if ( covIsDiagonal_ ) {
    for (int k=0; k<numDOF_; k++){
      // Must only loop over lower or upper triangular part
      // because accessor function (i,j) adjusts both upper and lower triangular
      // part
      for (int j=0; j<num_grads; j++)
	for (int i=0; i<=j; i++)
	  hessians[start+k](i,j) /= std::sqrt( covDiagonal_[k] ); 
    }
  }else{
    RealVector hess_ij_res( numDOF_, false );
    RealVector scaled_hess_ij_res( numDOF_, false );
    // Must only loop over lower or upper triangular part
    // because accessor function (i,j) adjusts both upper and lower triangular
    // part
    for (int j=0; j<num_grads; j++){
      for (int i=0; i<=j; i++){
	// Extract the ij hessian components for each degree of freedom 
	for (int k=0; k<numDOF_; k++)
	  hess_ij_res[k] = hessians[start+k](i,j);
	// Apply covariance matrix to the extracted hessian components
	apply_covariance_inverse_sqrt( hess_ij_res, scaled_hess_ij_res );
	// Copy result back into hessians
	for (int k=0; k<numDOF_; k++)
	  hessians[start+k](i,j) = scaled_hess_ij_res[k];
      }
    }
  }
}

int CovarianceMatrix::num_dof() const {
  return numDOF_;
}

void CovarianceMatrix::print() const {
  if ( covIsDiagonal_ ) {
    std::cout << " Covariance is Diagonal " << '\n';
    covDiagonal_.print(std::cout);
  }
  else {  
    std::cout << " Covariance is Full " << '\n';
    covMatrix_.print(std::cout);
  }
}


ExperimentCovariance & ExperimentCovariance::operator=(const ExperimentCovariance& source)
{
  numBlocks_ = source.numBlocks_;
  covMatrices_.resize(source.covMatrices_.size());
  for( size_t i=0; i<source.covMatrices_.size(); ++i)
    covMatrices_[i] = source.covMatrices_[i];

  return *this;
}

void ExperimentCovariance::set_covariance_matrices( 
std::vector<RealMatrix> &matrices, 
std::vector<RealVector> &diagonals,
RealVector &scalars,
IntVector matrix_map_indices,
IntVector diagonal_map_indices, 
IntVector scalar_map_indices ){

  if ( matrices.size() != matrix_map_indices.length() ){
    std::string msg = "must specify a index map for each full ";
    msg += "covariance matrix.";
    throw( std::runtime_error( msg ) );
  }
  if ( diagonals.size() != diagonal_map_indices.length() ){
    std::string msg = "must specify a index map for each diagonal ";
    msg += "covariance matrix.";
    throw( std::runtime_error( msg ) );
  }
  if ( scalars.length() != scalar_map_indices.length() ){
    std::string msg = "must specify a index map for each scalar ";
    msg += "covariance matrix.";
    throw( std::runtime_error( msg ) );
  }

  numBlocks_ = matrix_map_indices.length() + diagonal_map_indices.length() + 
    scalar_map_indices.length();

  covMatrices_.resize( numBlocks_ );

  for (int i=0; i<matrices.size(); i++ ){
    int index = matrix_map_indices[i];
    if ( index >= numBlocks_ )
      throw( std::runtime_error( "matrix_map_indices was out of bounds." ) );
    covMatrices_[index].set_covariance( matrices[i] );
  }

  for (int i=0; i<diagonals.size(); i++){
    int index = diagonal_map_indices[i];
    if ( index >= numBlocks_ )
      throw( std::runtime_error( "diagonal_map_indices was out of bounds." ) );
    covMatrices_[index].set_covariance( diagonals[i] );
  }

  for (int i=0; i<scalars.length(); i++ ){
    int index = scalar_map_indices[i];
    if ( index >= numBlocks_ )
      throw( std::runtime_error( "scalar_map_indices was out of bounds." ) );
    covMatrices_[index].set_covariance( scalars[i] );
    }
}

Real ExperimentCovariance::apply_experiment_covariance( const RealVector &vector)
  const{
  int shift = 0;
  Real result = 0.;
  for (int i=0; i<covMatrices_.size(); i++ ){
    int num_dof = covMatrices_[i].num_dof();
    RealVector sub_vector( Teuchos::View, vector.values()+shift, num_dof );
    result += covMatrices_[i].apply_covariance_inverse( sub_vector );
    shift += num_dof;
  }
  return result;
}

void ExperimentCovariance::apply_experiment_covariance_inverse_sqrt( 
		  const RealVector &vector, RealVector &result ) const{
  int shift = 0;
  result.sizeUninitialized( vector.length() );
  for (int i=0; i<covMatrices_.size(); i++ ){
    int num_dof = covMatrices_[i].num_dof();
    RealVector sub_vector( Teuchos::View, vector.values()+shift, num_dof );
    RealVector sub_result( Teuchos::View, result.values()+shift, num_dof );
    covMatrices_[i].apply_covariance_inverse_sqrt( sub_vector, sub_result );
    shift += num_dof;
  }
}

void ExperimentCovariance::apply_experiment_covariance_inverse_sqrt_to_gradients(
const RealMatrix &gradients, RealMatrix &result ) const{

  int shift = 0;
  int num_grads = gradients.numRows();
  result.shape( num_grads, gradients.numCols() );
  for (int i=0; i<covMatrices_.size(); i++ ){
    int num_dof = covMatrices_[i].num_dof();
    RealMatrix sub_matrix( Teuchos::View,gradients,num_grads,num_dof,0,shift );
    RealMatrix sub_result( Teuchos::View, result, num_grads, num_dof, 0, shift );
    covMatrices_[i].apply_covariance_inverse_sqrt_to_gradients( sub_matrix, 
								sub_result );
    shift += num_dof;
  }
}

void ExperimentCovariance::apply_experiment_covariance_inverse_sqrt_to_hessians( 
	const RealSymMatrixArray &hessians, RealSymMatrixArray &result ) const {
  // perform deep copy of hessians
  result.resize( hessians.size() );
  for (int i=0; i<hessians.size(); i++ ){
    result[i].shapeUninitialized( hessians[i].numRows() );
    result[i].assign( hessians[i] );
  }
  int shift = 0;
  for (int i=0; i<covMatrices_.size(); i++ ){
    int num_dof = covMatrices_[i].num_dof();
    // modifify in place the hessian copies stored in result
    covMatrices_[i].apply_covariance_inverse_sqrt_to_hessian( result, shift );
    shift += num_dof;
  }
}

void ExperimentCovariance::print_covariance_blocks() const {
  
  for (int i=0; i<covMatrices_.size(); i++ ){
    std::cout << "Covariance Matrix " << i << '\n';
    covMatrices_[i].print();
  }
}

} //namespace Dakota
