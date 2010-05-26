/** @file bessel.c Documented Bessel module
 * Julien Lesgourgues, 18.04.2010    
 *
 * This module loads spherical Bessel functions
 * (either read from file or computed from scratch).
 *
 * Hence the following functions can be called from other modules:
 *
 * -# bessel_init() at the beginning (but after background_init(), thermodynamics_init() and perturb_init())
 * -# bessel_at_x() at any time for computing a value j_l(x) at any x by interpolation
 * -# bessel_free() at the end
 */

#include "bessel.h"
#ifdef _OPENMP
#include "omp.h"
#endif

/** 
 * Bessel function for arbitrary argument x. 
 *
 * Evaluates the spherical Bessel function x at a given value of x by
 * interpolating in the pre-computed table.  This function can be
 * called from whatever module at whatever time, provided that
 * bessel_init() has been called before, and bessel_free() has not
 * been called yet.
 *
 * @param x Input: argument x of \f$j_l(x) \f$
 * @param l Input: order l of \f$j_l(x) \f$
 * @param j Ouput: \f$j_l(x) \f$
 * @return the error status
 */
int bessel_at_x(
		struct bessels * pbs,
		double x,
		int index_l,
		double * j
		) {

  
  /** Summary: */

  /** - define local variables */

  int index_x;          /* index in the interpolation table */
  double a,b,h;         /* quantities for the splint interpolation formula */

  /** - if index_l is too large to be in the interpolation table, return  an error */

  class_test(index_l > pbs->l_size,
	     pbs->error_message,
	     "index_l=%d>l_size=%d; increase l_max.",index_l,pbs->l_size);

  /** - if x is too small to be in the interpolation table, return 0 */

  if (x < pbs->x_min[index_l]) {
    *j=0;
    return _SUCCESS_;
  }
  else {

    /** - if x is too large to be in the interpolation table, return  an error (this should never occur since x_max in the table should really be the highest value needed by the code, given the precision parameters) */

    class_test(x > pbs->x_max,
	       pbs->error_message,
	       "x=%e>x_max=%e; increase x_max.",x,pbs->x_max);

    /** - otherwise, interpolation is needed: */

    /** (a) find index_x, i.e. the position of x in the table; no complicated algorithm needed, since values are regularly spaced with a known step and known first value */

    index_x = (int)((x-pbs->x_min[index_l])/pbs->x_step);

    /** (b) find result with the splint algorithm (equivalent to the one in numerical recipies) */
    h=pbs->x_step;
    b = ( x - (pbs->x_min[index_l]+pbs->x_step*index_x) )/h;
    a = 1.-b;
    *j= a * pbs->j[index_l][index_x] 
      + b * pbs->j[index_l][index_x+1]
      + ((a*a*a-a) * pbs->ddj[index_l][index_x]
	 +(b*b*b-b) * pbs->ddj[index_l][index_x+1]) * (h*h)/6.0;
  }

  return _SUCCESS_;

}

/**
 * Get spherical Bessel functions (either read from file or compute from scratch).  
 *
 * Each table of spherical Bessel functions \f$ j_l(x) \f$ corresponds
 * to a set of values for: 
 *
 * -# l_max: last value of l (first value is always 2, so number of values is l_max-1)
 * -# x_step: step dx for sampling Bessel functions \f$ j_l(x) \f$
 * -# x_max: last value of x (always a multiple of x_step!)
 * -# j_cut: value of \f$ j_l \f$ below which it is approximated by zero (in the region x << l)
 *
 * This function checks whether there is alread a file "bessels.dat"
 * with the same (x_step, j_cut), and greater or equal values of
 * (l_max, x_max). If yes, it fills the table of bessel functions (and
 * their second derivatives, needed for spline interpolation) with the
 * values read from the file. If not, it computes all values using
 * bessel_j() and array_spline(), and stores them both in the bessels
 * stucture pbs, and in a file "bessels.dat" (in view of the next
 * runs).
 *
 * This function shall be called at the beginning of each run, but
 * only after background_init(), thermodynamics_init() and perturb_init(). It
 * allocates memory spaces which should be freed later with
 * bessel_free().
 *
 * @param pba_input Input : Initialized background structure (useful for knowing conformal age eta_0)
 * @param ppt_input Input : Initialized perturbation structure (useful for knowing k_max)
 * @param ppr_input Input : Parameters describing how the computation is to be performed
 * @param pbs_output Output : Initialized bessel structure 
 * @return the error status
 */
int bessel_init(
		struct precision * ppr,
		struct background * pba,
		struct perturbs * ppt,
		struct bessels * pbs
		) {

  /** Summary: */

  /** - define local variables */

  /* index for l (since first value of l is always 2, l=index_l+2) */
  int index_l;
  /* index for x (x=x_min[index_l]+x_step*index_x) */
  int index_x;
  /* conformal time today (useful for computing x_max) */
  double eta0;
  /* minimum, maximum value of k (useful for computing x_min,x_max) */
  double kmin,kmax;
  /* for computing x_min */
  double x_min_up,x_min_down;
  /* first numbers to be read in bessels.dat file */
  int l_size_file;
  int * l_file;
  double x_step_file;
  double x_max_file;
  double j_cut_file;
  /* useful for reading useless data in bessels.dat file */
  int x_extra;
  int l_extra;
  int * x_size_file;
  void * junk;
  /* bessels.dat file */
  FILE * bessel_file;
  /* value of j_l(x) returned by bessel_j() */
  double j;
  /* an array used temporarily: for each l, contains the list of x
     values in column number column_x, of j_l values in columns_j, of
     j_l'' values in column ddj */
  double * j_array;
  int column_x=0;
  int column_j=1;
  int column_ddj=2;
  int column_num=3;

  /* This code can be optionally compiled with the openmp option for parallel computation.
     Inside parallel regions, the use of the command "return" is forbidden.
     For error management, instead of "return _FAILURE_", we will set the variable below
     to "abort = _TRUE_". This will lead to a "return _FAILURE_" jus after leaving the 
     parallel region. */
  int abort;

#ifdef _OPENMP
  /* instrumentation times */
  double tstart, tstop;
#endif

  if ((ppt->has_cl_cmb_temperature == _FALSE_) &&
      (ppt->has_cl_cmb_polarization == _FALSE_) &&
      (ppt->has_cl_cmb_lensing_potential == _FALSE_)) {
    if (pbs->bessels_verbose > 0)
      printf("No harmonic space transfer functions to compute. Bessel module skipped.\n");
    return _SUCCESS_;
  }

  /** - get conformal age from background structure (only place where this structure is used in this module) */
  eta0 = pba->conformal_age;

  /** - get maximum and minimum wavenumber from perturbation structure (only place where this structure is used in this module) */
  if (ppt->has_scalars) {
    kmax = ppt->k[ppt->index_md_scalars]
      [ppt->k_size_cl[ppt->index_md_scalars]-1];
    kmin = ppt->k[ppt->index_md_scalars][0];
  }
  if (ppt->has_tensors) {
    kmax=max(kmax,ppt->k[ppt->index_md_tensors]
	     [ppt->k_size_cl[ppt->index_md_scalars]-1]);
    kmin = min(kmin,ppt->k[ppt->index_md_scalars][0]);
  }

  /** - compute l values, x_step and j_cut given the parameters passed
      in the precision structure; and x_max given eta0 and kmax */

  class_call(bessel_get_l_list(ppr,pbs),
	     pbs->error_message,
	     pbs->error_message);

  pbs->x_step = ppr->bessel_scalar_x_step;
  
  class_test(pbs->x_step <= 0.,
	     pbs->error_message,
	     "x_step=%e",pbs->x_step);

  pbs->j_cut = ppr->bessel_scalar_j_cut;
  pbs->x_max = ((int)(kmax * eta0 / pbs->x_step)+1)*pbs->x_step; /* always multiple of x_step */

  /** - check if file bessels.dat already exists with the same (l's, x_step, x_max, j_cut). If yes, read it. */

  if (ppr->bessel_always_recompute == _FALSE_) {

    bessel_file=fopen("bessels.dat","r");
  
    if (bessel_file == NULL) {
      if (pbs->bessels_verbose > 1)
	printf("File bessels.dat did not exist.\n");
    }
    else {

      class_test(fread(&l_size_file,sizeof(int),1,bessel_file) != 1,
		 pbs->error_message,
		 "Could not read in bessel file");

      class_alloc(l_file,l_size_file * sizeof(int),pbs->error_message);

      for (index_l=0; index_l < l_size_file; index_l++) {
	class_test(fread(&l_file[index_l],sizeof(int),1,bessel_file) != 1,
		   pbs->error_message,
		   "Could not read in bessel file");
      }

      class_test(fread(&x_step_file,sizeof(double),1,bessel_file) != 1,
		 pbs->error_message,
		 "Could not read in bessel file");

      class_test(fread(&x_max_file,sizeof(double),1,bessel_file) != 1,
		 pbs->error_message,
		 "Could not read in bessel file");

      class_test(fread(&j_cut_file,sizeof(double),1,bessel_file) != 1,
		 pbs->error_message,
		 "Could not read in bessel file");

      index_l=0;
      if (l_size_file == pbs->l_size) {
	while ((pbs->l[index_l] == l_file[index_l]) && (index_l < pbs->l_size-1)) {
	  index_l++;
	}
	if (pbs->l[pbs->l_size-1] == l_file[pbs->l_size-1]) 
	  index_l++;
      }

      free(l_file);

      if ((index_l == pbs->l_size) &&
	  (x_step_file == pbs->x_step) &&
	  (j_cut_file == pbs->j_cut) &&
	  (x_max_file == pbs->x_max)) {

	if (pbs->bessels_verbose > 0)
	  printf("Read bessels in file 'bessels.dat'\n");
	
	class_alloc(pbs->x_min,pbs->l_size*sizeof(double),pbs->error_message);
	class_alloc(pbs->x_size,pbs->l_size*sizeof(int),pbs->error_message);
	class_alloc(pbs->j,pbs->l_size*sizeof(double),pbs->error_message);
	class_alloc(pbs->ddj,pbs->l_size*sizeof(double),pbs->error_message);

	class_test(fread(pbs->x_min,sizeof(double),pbs->l_size,bessel_file) != pbs->l_size,
		   pbs->error_message,
		   "Could not read in bessel file");

	class_test(fread(pbs->x_size,sizeof(int),pbs->l_size,bessel_file) != pbs->l_size,
		   pbs->error_message,
		   "Could not read in bessel file");

	for (index_l=0; index_l < pbs->l_size; index_l++) {
	    
	  class_alloc(pbs->j[index_l],pbs->x_size[index_l]*sizeof(double),pbs->error_message);

	  class_test(fread(pbs->j[index_l],sizeof(double),pbs->x_size[index_l],bessel_file) != pbs->x_size[index_l],
		     pbs->error_message,
		     "Could not read in bessel file");
	}

	for (index_l=0; index_l < pbs->l_size; index_l++) {

	  class_alloc(pbs->ddj[index_l],pbs->x_size[index_l]*sizeof(double),pbs->error_message);
	    
	  class_test(fread(pbs->ddj[index_l],sizeof(double),pbs->x_size[index_l],bessel_file) != pbs->x_size[index_l],
		     pbs->error_message,
		     "Could not read in bessel file");
	}

	fclose(bessel_file);

	return _SUCCESS_;

      }
      else {
	fclose(bessel_file);
      }
    }
  }

  /** - if not, compute form scratch : */

  if (pbs->bessels_verbose > 0)
    printf("Computing bessels\n");

  class_alloc(pbs->x_min,pbs->l_size*sizeof(double),pbs->error_message);
  class_alloc(pbs->x_size,pbs->l_size*sizeof(int),pbs->error_message);
  class_alloc(pbs->j,pbs->l_size*sizeof(double),pbs->error_message);
  class_alloc(pbs->ddj,pbs->l_size*sizeof(double),pbs->error_message);

  /* initialize error management flag */
  abort = _FALSE_;
  
  /*** beginning of parallel region ***/

#pragma omp parallel							\
  shared(pbs,kmin,ppr,column_num,column_x,column_j,column_ddj)		\
  private(index_l,index_x,j,x_min_up,x_min_down,j_array,tstart,tstop)

  {

#ifdef _OPENMP
    tstart = omp_get_wtime();
#endif

#pragma omp for schedule (dynamic)

    /** (a) loop over l and x values, compute \f$ j_l(x) \f$ for each of them */

    for (index_l = 0; index_l < pbs->l_size; index_l++) {
    
#pragma omp flush(abort)

      if (abort == _FALSE_) {

	index_x=0;
	j = 0.;

	/* find x_min[index_l] by dichotomy */
	x_min_up=(double)pbs->l[index_l]+0.5;
	x_min_down=0.;

	class_call_parallel(bessel_j(pbs,
				     pbs->l[index_l], /* l */
				     x_min_up, /* x */
				     &j),  /* j_l(x) */
			    pbs->error_message,
			    pbs->error_message);
    
	class_test_parallel(j < pbs->j_cut,
			    pbs->error_message,
			    "in dichotomy, wrong initial guess for x_min_up.");

#pragma omp flush(abort)

	if (abort == _FALSE_) {
 
	  while ((x_min_up-x_min_down) > kmin) {
      
	    class_test_parallel((x_min_up-x_min_down) < ppr->smallest_allowed_variation,
				pbs->error_message,
				"(x_min_up-x_min_down) =%e < machine precision : maybe kmin=%e is too small",
				(x_min_up-x_min_down),kmin);

	    class_call_parallel(bessel_j(pbs,
					 pbs->l[index_l], /* l */
					 0.5 * (x_min_up+x_min_down), /* x */
					 &j),  /* j_l(x) */
				pbs->error_message,
				pbs->error_message);

#pragma omp flush(abort)

	    if (abort == _FALSE_) {

	      if (j >= pbs->j_cut) 
		x_min_up=0.5 * (x_min_up+x_min_down);
	      else
		x_min_down=0.5 * (x_min_up+x_min_down);

	    }
	  }

	  pbs->x_min[index_l]=x_min_down;

	  class_call_parallel(bessel_j(pbs,
				       pbs->l[index_l], /* l */
				       pbs->x_min[index_l], /* x */
				       &j),  /* j_l(x) */
			      pbs->error_message,
			      pbs->error_message);

	  if (abort == _FALSE_) {

	    /* case when all values of j_l(x) were negligible for this l*/
	    if (pbs->x_min[index_l] >= pbs->x_max) {
	      pbs->x_min[index_l] = pbs->x_max;
	      pbs->x_size[index_l] = 1;

	      class_alloc_parallel(pbs->j[index_l],pbs->x_size[index_l]*sizeof(double),pbs->error_message);
	      class_alloc_parallel(pbs->ddj[index_l],pbs->x_size[index_l]*sizeof(double),pbs->error_message);

	      pbs->j[index_l][0]=0;
	      pbs->ddj[index_l][0]=0;
	    }
	    /* write first non-negligible value */
	    else {
	      pbs->x_size[index_l] = (int)((pbs->x_max-pbs->x_min[index_l])/pbs->x_step) + 1;

	      class_alloc_parallel(j_array,pbs->x_size[index_l]*column_num*sizeof(double),pbs->error_message);

	      j_array[0*column_num+column_x]=pbs->x_min[index_l];
	      j_array[0*column_num+column_j]=j;

	      /* loop over other non-negligible values */
	      for (index_x=1; index_x < pbs->x_size[index_l]; index_x++) {

#pragma omp flush(abort)

		if (abort == _FALSE_) {

		  j_array[index_x*column_num+column_x]=pbs->x_min[index_l]+index_x*pbs->x_step;

		  class_call_parallel(bessel_j(pbs,
					       pbs->l[index_l], /* l */
					       j_array[index_x*column_num+column_x], /* x */
					       j_array+index_x*column_num+column_j),  /* j_l(x) */
				      pbs->error_message,
				      pbs->error_message);
		}
	      }

	      class_call_parallel(array_spline(j_array,
					       column_num,
					       pbs->x_size[index_l],
					       column_x,
					       column_j,
					       column_ddj,
					       _SPLINE_EST_DERIV_,
					       pbs->error_message),
				  pbs->error_message,
				  pbs->error_message);

	      class_alloc_parallel(pbs->j[index_l],pbs->x_size[index_l]*sizeof(double),pbs->error_message);
	      class_alloc_parallel(pbs->ddj[index_l],pbs->x_size[index_l]*sizeof(double),pbs->error_message);

	      for (index_x=0; index_x < pbs->x_size[index_l]; index_x++) {
		pbs->j[index_l][index_x] = j_array[index_x*column_num+column_j];
		pbs->ddj[index_l][index_x] = j_array[index_x*column_num+column_ddj];
	      }

	      free(j_array);
     
	    }      
	  }
	}
      }
    } /* end of loop over l */

#ifdef _OPENMP
    tstop = omp_get_wtime();
    if (pbs->bessels_verbose > 1)
      printf("In %s: time spent in parallel region (loop over l's) = %e s for thread %d\n",
	     __func__,tstop-tstart,omp_get_thread_num());
#endif

  }

  /*** end of parallel region ***/

  if (abort == _TRUE_) return _FAILURE_;

  if (pbs->bessels_verbose > 0)
    printf(" -> (over)write in file 'bessels.dat'\n");

  /** (b) write in file */

  bessel_file = fopen("bessels.dat","w");

  fwrite(&(pbs->l_size),sizeof(int),1,bessel_file);
  fwrite(pbs->l,sizeof(int),pbs->l_size,bessel_file);
  fwrite(&(pbs->x_step),sizeof(double),1,bessel_file);
  fwrite(&(pbs->x_max),sizeof(double),1,bessel_file);
  fwrite(&(pbs->j_cut),sizeof(double),1,bessel_file);

  fwrite(pbs->x_min,sizeof(double),pbs->l_size,bessel_file);

  fwrite(pbs->x_size,sizeof(int),pbs->l_size,bessel_file);

  for (index_l=0; index_l<pbs->l_size; index_l++) {
    fwrite(pbs->j[index_l],sizeof(double),pbs->x_size[index_l],bessel_file);
  }

  for (index_l=0; index_l<pbs->l_size; index_l++) {
    fwrite(pbs->ddj[index_l],sizeof(double),pbs->x_size[index_l],bessel_file);
  }

  fclose(bessel_file);

  return _SUCCESS_;
}

/**
 * Free all memory space allocated by bessel_init().
 *
 * To be called at the end of each run.
 *
 * @return the error status
 */
int bessel_free(
		struct bessels * pbs) {

  int index_l;

  for (index_l = 0; index_l < pbs->l_size; index_l++) {
    free(pbs->j[index_l]);
    free(pbs->ddj[index_l]);
  }
  free(pbs->j);
  free(pbs->ddj);
  free(pbs->x_min);
  free(pbs->x_size);
  free(pbs->l);

  return _SUCCESS_; 
}

/**
 * Define number of mutipoles l
 *
 * Define the number of multipoles l for each mode, using information
 * in the precision structure.
 *
 * @param pl_list_size Output: number of multipole 
 * @return the error status
 */
int bessel_get_l_list(
		      struct precision * ppr,
		      struct bessels * pbs
		      ) {

  int index_l,increment,current_l;

  index_l = 0;
  current_l = 2;
  increment = max((int)(current_l * (ppr->l_logstep-1.)),1);
    
  while (((current_l+increment) < ppr->l_max) && 
	 (increment < ppr->l_linstep)) {
      
    index_l ++;
    current_l += increment;
    increment = max((int)(current_l * (ppr->l_logstep-1.)),1);

  }

  increment = ppr->l_linstep;

  while ((current_l+increment) <= ppr->l_max) {

    index_l ++;
    current_l += increment;

  }

  if (current_l != ppr->l_max) {

    index_l ++;
    current_l = ppr->l_max;

  } 

  pbs->l_size = index_l+1;

  class_alloc(pbs->l,pbs->l_size*sizeof(int),pbs->error_message);

  index_l = 0;
  pbs->l[0] = 2;
  increment = max((int)(pbs->l[0] * (ppr->l_logstep-1.)),1);

  while (((pbs->l[index_l]+increment) < ppr->l_max) && 
	 (increment < ppr->l_linstep)) {
      
    index_l ++;
    pbs->l[index_l]=pbs->l[index_l-1]+increment;
    increment = max((int)(pbs->l[index_l] * (ppr->l_logstep-1.)),1);
 
  }

  increment = ppr->l_linstep;

  while ((pbs->l[index_l]+increment) <= ppr->l_max) {

    index_l ++;
    pbs->l[index_l]=pbs->l[index_l-1]+increment;
 
  }

  if (pbs->l[index_l] != ppr->l_scalar_max) {

    index_l ++;
    pbs->l[index_l]=ppr->l_max;
       
  }
  
  return _SUCCESS_;

}

/**
 * Compute spherical Bessel function \f$ j_l(x) \f$ for a given l and x.
 *
 * Inspired from Numerical Recipies.
 * 
 * @param l Input: l value
 * @param x Input: x value
 * @param jl Output: \f$ j_l(x) \f$ value
 * @return the error status
 */
int bessel_j(
	     struct bessels * pbs,
	     int l,
	     double x,
	     double * jl
	     ) {
    
  double nu,nu2,beta,beta2;
  double x2,sx,sx2,cx;
  double cotb,cot3b,cot6b,secb,sec2b;
  double trigarg,expterm,fl;
  double l3,cosb;

  class_test(l < 0,
	     pbs->error_message,
	     "");

  class_test(x < 0,
	     pbs->error_message,
	     "");

  fl = (double)l;

  x2 = x*x;

  /************* Use closed form for l<7 **********/

  if (l < 7) {

    sx=sin(x);
    cx=cos(x);

    if(l == 0) {
      if (x > 0.1) *jl=sx/x;
      else *jl=1.-x2/6.*(1.-x2/20.);
      return _SUCCESS_;
    }

    if(l == 1) {
      if (x > 0.2) *jl=(sx/x -cx)/x;
      else *jl=x/3.*(1.-x2/10.*(1.-x2/28.));
      return _SUCCESS_;
    }

    if (l == 2) {
      if (x > 0.3) *jl=(-3.*cx/x-sx*(1.-3./x2))/x;
      else *jl=x2/15.*(1.-x2/14.*(1.-x2/36.));
      return _SUCCESS_;
    }

    if (l == 3) {
      if (x > 0.4) *jl=(cx*(1.-15./x2)-sx*(6.-15./x2)/x)/x;
      else *jl=x*x2/105.*(1.-x2/18.*(1.-x2/44.));
      return _SUCCESS_;
    }

    if (l == 4) {
      if (x > 0.6) *jl=(sx*(1.-45./x2+105./x2/x2) +cx*(10.-105./x2)/x)/x;
      else *jl=x2*x2/945.*(1.-x2/22.*(1.-x2/52.));
      return _SUCCESS_;
    }
    
    if (l == 5) {
      if (x > 1) *jl=(sx*(15.-420./x2+945./x2/x2)/x -cx*(1.0-105./x2+945./x2/x2))/x;
      else *jl=x2*x2*x/10395.*(1.-x2/26.*(1.-x2/60.));
      return _SUCCESS_;
    }

    if (l == 6) {
      if (x > 1) *jl=(sx*(-1.+(210.-(4725.-10395./x2)/x2)/x2)+
		      cx*(-21.+(1260.-10395./x2)/x2)/x)/x;
      else *jl=x2*x2*x2/135135.*(1.-x2/30.*(1.-x2/68.));
      return _SUCCESS_;
    }

  }

  else {

    if (x <= 1.e-40) {
      *jl=0.0;
      return _SUCCESS_;
    }

    nu= fl + 0.5;
    nu2=nu*nu;

    if ((x2/fl) < 0.5) {
      *jl=exp(fl*log(x/nu/2.)+nu*(1-log(2.))-(1.-(1.-3.5/nu2)/nu2/30.)/12./nu)
	/nu*(1.-x2/(4.*nu+4.)*(1.-x2/(8.*nu+16.)*(1.-x2/(12.*nu+36.))));
      return _SUCCESS_;
    }

    if ((fl*fl/x) < 0.5) {

      beta = x - _PI_/2.*(fl+1.);
      *jl = (cos(beta)*(1.-(nu2-0.25)*(nu2-2.25)/8./x2*(1.-(nu2-6.25)*(nu2-12.25)/48./x2))
	     -sin(beta)*(nu2-0.25)/2./x* (1.-(nu2-2.25)*(nu2-6.25)/24./x2*(1.-(nu2-12.25)*(nu2-20.25)/80./x2)) )/x;
      
      return _SUCCESS_;

    }

    l3 = pow(nu,0.325);

    if (x < nu-1.31*l3) {

      cosb=nu/x;
      sx=sqrt(nu2-x2);
      cotb=nu/sx;
      secb=x/nu;
      beta=log(cosb+sx/x);
      cot3b=cotb*cotb*cotb;
      cot6b=cot3b*cot3b;
      sec2b=secb*secb;
      expterm=((2.+3.*sec2b)*cot3b/24.
	       - ((4.+sec2b)*sec2b*cot6b/16.
		  + ((16.-(1512.+(3654.+375.*sec2b)*sec2b)*sec2b)*cot3b/5760.
		     + (32.+(288.+(232.+13.*sec2b)*sec2b)*sec2b)*sec2b*cot6b/128./nu)*cot6b/nu)/nu)/nu;
      *jl=sqrt(cotb*cosb)/(2.*nu)*exp(-nu*beta+nu/cotb-expterm);

      return _SUCCESS_;

    }

    if (x > nu+1.48*l3) {

      cosb=nu/x;
      sx=sqrt(x2-nu2);
      cotb=nu/sx;
      secb=x/nu;
      beta=acos(cosb);
      cot3b=cotb*cotb*cotb;
      cot6b=cot3b*cot3b;
      sec2b=secb*secb;
      trigarg=nu/cotb-nu*beta-_PI_/4.
	-((2.+3.*sec2b)*cot3b/24.
	  +(16.-(1512.+(3654.+375.*sec2b)*sec2b)*sec2b)*cot3b*cot6b/5760./nu2)/nu;
      expterm=((4.+sec2b)*sec2b*cot6b/16.
	       -(32.+(288.+(232.+13.*sec2b)*sec2b)*sec2b)*sec2b*cot6b*cot6b/128./nu2)/nu2;
      *jl=sqrt(cotb*cosb)/nu*exp(-expterm)*cos(trigarg);

      return _SUCCESS_;
    }
    
    /* last possible case */

    beta=x-nu;
    beta2=beta*beta;
    sx=6./x;
    sx2=sx*sx;
    secb=pow(sx,1./3.);
    sec2b=secb*secb;
    *jl=(_GAMMA1_*secb + beta*_GAMMA2_*sec2b
	 -(beta2/18.-1./45.)*beta*sx*secb*_GAMMA1_
	 -((beta2-1.)*beta2/36.+1./420.)*sx*sec2b*_GAMMA2_
	 +(((beta2/1620.-7./3240.)*beta2+1./648.)*beta2-1./8100.)*sx2*secb*_GAMMA1_
	 +(((beta2/4536.-1./810.)*beta2+19./11340.)*beta2-13./28350.)*beta*sx2*sec2b*_GAMMA2_
	 -((((beta2/349920.-1./29160.)*beta2+71./583200.)*beta2-121./874800.)*
	   beta2+7939./224532000.)*beta*sx2*sx*secb*_GAMMA1_)*sqrt(sx)/12./sqrt(_PI_);

    return _SUCCESS_;

  }

  class_test(0==0,
	     pbs->error_message,
	     "value of l=%d or x=%e out of bounds",l,x);

}

