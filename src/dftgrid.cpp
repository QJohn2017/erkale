/*
 *                This source code is part of
 * 
 *                     E  R  K  A  L  E
 *                             -
 *                       DFT from Hel
 *
 * Written by Susi Lehtola, 2010-2011
 * Copyright (c) 2010-2011, Susi Lehtola
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */



#include <cmath>
#include <cstdio>
// LibXC
#include <xc.h>

#include "dftfuncs.h"
#include "dftgrid.h"
#include "elements.h"
#include "mathf.h"
// Lobatto or Lebedev for angular
#include "lobatto.h"
#include "lebedev.h"
// Gauss-Chebyshev for radial integral
#include "chebyshev.h"

#include "stringutil.h"
#include "timer.h"

// OpenMP parallellization for XC calculations
#ifdef _OPENMP
#include <omp.h>
#endif

// Compute closed-shell result from open-shell result
//#define CONSISTENCYCHECK

/* Partitioning functions */

double f_p(double mu) {
  return 1.5*mu-0.5*mu*mu*mu;
}

double f_z(double mu, double a) {
  return f_p(mu/a);
}

double f_q(double mu, double a) {
  if(mu<-a)
    return -1.0;
  else if(mu<a)
    return f_z(mu,a);
  else
    return 1.0;
}

double f_s(double mu, double a) {
  return 0.5*(1.0-f_p(f_p(f_q(mu,a))));
}


void AtomGrid::add_lobatto_shell(atomgrid_t & g, size_t ir) {
  // Add points on ind:th radial shell.

  // Radius
  double rad=g.sh[ir].r;
  // Order of quadrature rule
  int l=g.sh[ir].l;
  // Radial weight
  double wrad=g.sh[ir].w;

  // Number of points in theta
  int nth=(l+3)/2;

  // Get corresponding Lobatto quadrature rule points in theta
  std::vector<double> xl, wl;
  lobatto_compute(nth,xl,wl);

  // Store index of first point
  g.sh[ir].ind0=grid.size();
  // Number of points on this shell
  size_t np=0;

  // Loop over points in theta
  for(int ith=0;ith<nth;ith++) {
    // Compute cos(th) and sin(th);

    double cth=xl[ith];
    double sth=sqrt(1-cth*cth);

    // Determine number of points in phi, defined by smallest integer for which
    // sin^{nphi} \theta < THR
    double thr;
    if(l<=50)
      thr=1e-8;
    else
      thr=1e-9;

    // Calculate nphi
    int nphi=1;
    double t=sth;
    while(t>=thr && nphi<=l+1) {
      nphi++;
      t*=sth;
    }

    // Use an offset in phi?
    double phioff=0.0;
    if(ith%2)
      phioff=M_PI/nphi;

    // Now, generate the points.
    gridpoint_t point;
    double phi, dphi; // Value of phi and the increment
    double cph, sph; // Sine and cosine of phi

    dphi=2.0*M_PI/nphi;

    // Total weight of points on this ring is
    // (disregarding weight from Becke partitioning)
    point.w=2.0*M_PI*wl[ith]/nphi*wrad;
    
    for(int iphi=0;iphi<nphi;iphi++) {
      // Value of phi is
      phi=iphi*dphi+phioff;
      // and the sine and cosine are
      sph=sin(phi);
      cph=cos(phi);

      // Compute x, y and z
      point.r.x=rad*sth*cph;
      point.r.y=rad*sth*sph;
      point.r.z=rad*cth;
      
      // Displace to center
      point.r=point.r+g.cen;

      // Add point
      grid.push_back(point);
      // Increment number of points
      np++;
    }
  }

  // Store number of points on this shell
  g.sh[ir].np=np;
}

void AtomGrid::add_lebedev_shell(atomgrid_t & g, size_t ir) {
  // Add points on ind:th radial shell.

  // Radius
  double rad=g.sh[ir].r;
  // Order of quadrature rule
  int l=g.sh[ir].l;
  // Radial weight
  double wrad=g.sh[ir].w;

  // Get quadrature rule
  std::vector<lebedev_point_t> points=lebedev_sphere(l);

  // Store index of first point
  g.sh[ir].ind0=grid.size();
  // Number of points on this shell
  size_t np=points.size();

  // Loop over points
  for(size_t i=0;i<points.size();i++) {
    gridpoint_t point;

    point.r.x=rad*points[i].x;
    point.r.y=rad*points[i].y;
    point.r.z=rad*points[i].z;
    // Displace to center
    point.r=point.r+g.cen;

    // Compute quadrature weight
    // (Becke weight not included)
    point.w=wrad*points[i].w;

    // Add point
    grid.push_back(point);
  }
  
  // Store number of points on this shell
  g.sh[ir].np=np;
}

void AtomGrid::becke_weights(const BasisSet & bas, const atomgrid_t & g, size_t ir) {
  // Compute weights of points.

  // Number of atoms in system
  const size_t Nat=bas.get_Nnuc();

  // Helper arrays
  std::vector<double> atom_dist;
  std::vector<double> atom_weight;
  std::vector< std::vector<double> > mu_ab;
  std::vector< std::vector<double> > smu_ab;

  // Initialize memory
  atom_dist.resize(Nat);
  atom_weight.resize(Nat);
  mu_ab.resize(Nat);
  smu_ab.resize(Nat);
  for(size_t i=0;i<Nat;i++) {
    mu_ab[i].resize(Nat);
    smu_ab[i].resize(Nat);
  }

  // Loop over points on wanted radial shell
  for(size_t ip=g.sh[ir].ind0;ip<g.sh[ir].ind0+g.sh[ir].np;ip++) {
    // Coordinates of the point are
    coords_t coord_p=grid[ip].r;
    
    // Compute distance of point to atoms
    for(size_t iat=0;iat<Nat;iat++)
      atom_dist[iat]=norm(bas.get_coords(iat)-coord_p);
    
    // Compute mu_ab
    for(size_t iat=0;iat<Nat;iat++) {
      // Diagonal
      mu_ab[iat][iat]=0.0;
      // Off-diagonal
      for(size_t jat=0;jat<iat;jat++) {
	mu_ab[iat][jat]=(atom_dist[iat]-atom_dist[jat])/bas.nuclear_distance(iat,jat);
	mu_ab[jat][iat]=-mu_ab[iat][jat];
      }
    }

    // Compute s(mu_ab)
    for(size_t iat=0;iat<Nat;iat++)
      for(size_t jat=0;jat<Nat;jat++) {
	smu_ab[iat][jat]=f_s(mu_ab[iat][jat],0.7);
      }    

    // Then, compute atomic weights
    for(size_t iat=0;iat<Nat;iat++) {
      atom_weight[iat]=1.0;

      for(size_t jat=0;jat<iat;jat++)
	atom_weight[iat]*=smu_ab[iat][jat];
      for(size_t jat=iat+1;jat<Nat;jat++)
	atom_weight[iat]*=smu_ab[iat][jat];
    }

    // Compute sum of weights
    double awsum=0.0;
    for(size_t iat=0;iat<Nat;iat++)
      awsum+=atom_weight[iat];

    // The Becke weight is
    grid[ip].w*=atom_weight[g.atind]/awsum;
  }
}

void AtomGrid::prune_points(double tolv, const radshell_t & rg) {
  // Prune points with small weight.

  // First point on radial shell
  size_t ifirst=rg.ind0;
  // Last point on radial shell
  size_t ilast=ifirst+rg.np;

  for(size_t i=ilast;(i>=ifirst && i<grid.size());i--)
    if(grid[i].w<tolv)
      grid.erase(grid.begin()+i);
}


void AtomGrid::free() {
  // Free integration points
  grid.clear();

  // Free values of basis functions
  flist.clear();
  glist.clear();

  // Free LDA stuff
  rho.clear();
  exc.clear();
  vxc.clear();

  // Free GGA stuff
  sigma.clear();
  vsigma.clear();

  // Free mGGA stuff
  lapl_rho.clear();
  vlapl_rho.clear();
  tau.clear();
  vtau.clear();
}

void AtomGrid::update_density(const arma::mat & P) {
  // Update values of densitty 

  if(!P.n_elem) {
    ERROR_INFO();
    throw std::runtime_error("Error - density matrix is empty!\n");
  }

  // Non-polarized calculation.
  polarized=0;

  // Check consistency of allocation
  if(rho.size()!=grid.size())
    rho.resize(grid.size());

  // Loop over points
  for(size_t ip=0;ip<grid.size();ip++) {
    // Calculate density
    rho[ip]=eval_dens(P,ip);
  }

  if(do_grad) {
    // Adjust size of grid
    if(grho.size()!=3*rho.size())
      grho.resize(3*rho.size());
    if(sigma.size()!=grid.size())
      sigma.resize(grid.size());

    double grad[3];
    for(size_t ip=0;ip<grid.size();ip++) {
      // Calculate gradient
      eval_grad(P,ip,grad);
      // Store it
      for(int ic=0;ic<3;ic++)
	grho[3*ip+ic]=grad[ic];

      // Compute sigma as well
      sigma[ip]=grad[0]*grad[0] + grad[1]*grad[1] + grad[2]*grad[2];
    }
  }

  if(do_lapl) {
    // Adjust size of grid
    if(lapl_rho.size()!=rho.size())
      lapl_rho.resize(rho.size());
    if(tau.size()!=rho.size())
      tau.resize(rho.size());

    double lapl, kin;

    for(size_t ip=0;ip<grid.size();ip++) {
      // Evaluate laplacian and kinetic energy densities
      eval_lapl_kin_dens(P,ip,lapl,kin);
      // and add them to the stack
      lapl_rho[ip]=lapl;
      tau[ip]=kin;
    }
  }
}

void AtomGrid::update_density(const arma::mat & Pa, const arma::mat & Pb) {
  // Update values of densitty 

  if(!Pa.n_elem || !Pb.n_elem) {
    ERROR_INFO();
    throw std::runtime_error("Error - density matrix is empty!\n");
  }

  // Polarized calculation.
  polarized=1;

  // Check consistency of allocation
  if(rho.size()!=2*grid.size())
    rho.resize(2*grid.size());

  // Loop over points
  for(size_t ip=0;ip<grid.size();ip++) {
    // Compute densities
    rho[2*ip]=eval_dens(Pa,ip);
    rho[2*ip+1]=eval_dens(Pb,ip);
  }

  if(do_grad) {
    if(grho.size()!=6*rho.size())
      grho.resize(6*rho.size());
    if(sigma.size()!=3*grid.size())
      sigma.resize(3*grid.size());

    double grada[3];
    double gradb[3];

    for(size_t ip=0;ip<grid.size();ip++) {
      // Compute gradients
      eval_grad(Pa,ip,grada);
      eval_grad(Pb,ip,gradb);
      // and store them
      for(int ic=0;ic<3;ic++) {
	grho[6*ip+ic]=grada[ic];
	grho[6*ip+3+ic]=gradb[ic];
      }
      
      // Compute values of sigma
      sigma[3*ip]  =grada[0]*grada[0] + grada[1]*grada[1] + grada[2]*grada[2];
      sigma[3*ip+1]=grada[0]*gradb[0] + grada[1]*gradb[1] + grada[2]*gradb[2];
      sigma[3*ip+2]=gradb[0]*gradb[0] + gradb[1]*gradb[1] + gradb[2]*gradb[2];
    }
  }

  if(do_lapl) {
    // Adjust size of grid
    if(lapl_rho.size()!=rho.size())
      lapl_rho.resize(rho.size());
    if(tau.size()!=rho.size())
      tau.resize(rho.size());

    double lapla, kina;
    double laplb, kinb;

    for(size_t ip=0;ip<grid.size();ip++) {
      // Evaluate laplacian and kinetic energy densities
      eval_lapl_kin_dens(Pa,ip,lapla,kina);
      eval_lapl_kin_dens(Pb,ip,laplb,kinb);
      // and add them to the stack
      lapl_rho[2*ip]=lapla;
      lapl_rho[2*ip+1]=laplb;

      tau[2*ip]=kina;
      tau[2*ip+1]=kinb;
    }
  }
}

double AtomGrid::eval_dens(const arma::mat & P, size_t ip) const {
  // Density
  double d=0.0;
  
  // Loop over functions on point
  size_t first=grid[ip].f0;
  size_t last=first+grid[ip].nf;

  size_t i, j;

  for(size_t ii=first;ii<last;ii++) {
    // Index of function is
    i=flist[ii].ind;
    for(size_t jj=first;jj<last;jj++) {
      j=flist[jj].ind;
      d+=P(i,j)*flist[ii].f*flist[jj].f;      
    }
  }
  
  return d;
}

void AtomGrid::eval_grad(const arma::mat & P, size_t ip, double *g) const {
  // Initialize gradient
  for(int i=0;i<3;i++)
    g[i]=0.0;

  // Loop over functions on point
  size_t first=grid[ip].f0;
  size_t last=first+grid[ip].nf;

  size_t i, j;

  for(size_t ii=first;ii<last;ii++) {
    // Index of function is
    i=flist[ii].ind;
    for(size_t jj=first;jj<last;jj++) {
      j=flist[jj].ind;

      for(int ic=0;ic<3;ic++)
	g[ic]+=P(i,j)*(flist[ii].f*glist[3*jj+ic] + glist[3*ii+ic]*flist[jj].f);
    }
  }
}

void AtomGrid::eval_lapl_kin_dens(const arma::mat & P, size_t ip, double & lapl, double & kin) const {
  // Loop over functions on point
  size_t first=grid[ip].f0;
  size_t last=first+grid[ip].nf;

  size_t i, j;

  double bf_lapl;
  double bf_gdot;

  // Initialize output
  lapl=0.0;
  kin=0.0;

  for(size_t ii=first;ii<last;ii++) {
    // Index of function is
    i=flist[ii].ind;
    for(size_t jj=first;jj<last;jj++) {
      j=flist[jj].ind;
      
      // Laplacian and kinetic energy density
      bf_lapl=flist[ii].f*llist[jj] + llist[ii]*flist[jj].f;
      bf_gdot=glist[3*ii]*glist[3*jj] + glist[3*ii+1]*glist[3*jj+1] + glist[3*ii+2]*glist[3*jj+2];
      
      // Increment output
      lapl+=P(i,j)*(bf_lapl + 2.0*bf_gdot);
      kin+=P(i,j)*bf_gdot; // Used in libxc without factor 0.5
    }
  }
}

double AtomGrid::compute_Nel() const {
  double nel=0.0;

  if(!polarized)
    for(size_t ip=0;ip<grid.size();ip++)
      nel+=grid[ip].w*rho[ip];
  else
    for(size_t ip=0;ip<grid.size();ip++)
      nel+=grid[ip].w*(rho[2*ip]+rho[2*ip+1]);

  return nel;
}

void AtomGrid::init_xc() {
  // Size of grid.
  const size_t N=grid.size();

  // Check allocation of arrays.
  if(exc.size()!=N)
    exc.resize(N);

  if(!polarized) {
    // Restricted case - all arrays of length N

    if(vxc.size()!=N)
      vxc.resize(N);

    if(do_grad) {
      if(vsigma.size()!=N)
	vsigma.resize(N);
    }

    if(do_lapl) {
      if(vtau.size()!=N)
	vtau.resize(N);
      if(vlapl_rho.size()!=N)
	vlapl_rho.resize(N);
    }
  } else {
    // Unrestricted case - arrays of length 2N or 3N
    if(vxc.size()!=2*N)
      vxc.resize(2*N);

    if(do_grad) {
      if(vsigma.size()!=3*N)
	vsigma.resize(3*N);
    }

    if(do_lapl) {
      if(vlapl_rho.size()!=2*N)
	vlapl_rho.resize(2*N);
      if(vtau.size()!=2*N)
	vtau.resize(2*N);
    }
  }

  // Fill arrays with zeros.
  do_gga=0;
  do_mgga=0;

  for(size_t i=0;i<exc.size();i++)
    exc[i]=0.0;
  for(size_t i=0;i<vxc.size();i++)
    vxc[i]=0.0;
  for(size_t i=0;i<vsigma.size();i++)
    vsigma[i]=0.0;
  for(size_t i=0;i<vlapl_rho.size();i++)
    vlapl_rho[i]=0.0;
  for(size_t i=0;i<vtau.size();i++)
    vtau[i]=0.0;  
}

void AtomGrid::compute_xc(int func_id) {
  // Compute exchange-correlation functional

  int nspin;
  if(!polarized)
    nspin=XC_UNPOLARIZED;
  else
    nspin=XC_POLARIZED;
  
  // Correlation and exchange functionals
  xc_func_type func;
  if(xc_func_init(&func, func_id, nspin) != 0) {
    ERROR_INFO();
    std::ostringstream oss;
    oss << "Functional "<<func_id<<" not found!"; 
    throw std::runtime_error(oss.str());
  }

  // Which functional is in question?
  bool gga=0, mgga=0;
  
  // Determine the family
  switch(func.info->family)
    {
    case XC_FAMILY_LDA:
      gga=0;
      mgga=0;
      break;
      
    case XC_FAMILY_GGA:
    case XC_FAMILY_HYB_GGA:
      gga=1;
      mgga=0;
      break;
      
    case XC_FAMILY_MGGA:
      gga=0;
      mgga=1;
      break;

    default:
      {
	ERROR_INFO();
	std::ostringstream oss;
	oss << "Functional family " << func.info->family << " not currently supported in ERKALE!\n";
	throw std::runtime_error(oss.str());
      }
    }
  
  // Update controlling flags for eval_Fxc.
  do_gga=do_gga || gga || mgga;
  do_mgga=do_mgga || mgga;

  /* Work arrays. */
  const size_t N=grid.size();

  // Energy density
  std::vector<double> excwrk(N);

  // LDA term
  std::vector<double> vxcwrk;
  // GGA term
  std::vector<double> vsigmawrk;
  // mGGA terms
  std::vector<double> vlapl_rhowrk;
  std::vector<double> vtauwrk;

  // Allocate memory
  if(!polarized) {
    vxcwrk.resize(N);
    if(gga||mgga)
      vsigmawrk.resize(N);
    if(mgga) {
      vlapl_rhowrk.resize(N);
      vtauwrk.resize(N);
    }
  } else {
    vxcwrk.resize(2*N);
    if(gga||mgga)
      vsigmawrk.resize(3*N);
    if(mgga) {
      vlapl_rhowrk.resize(2*N);
      vtauwrk.resize(2*N);
    }
  }

  // Evaluate functionals.
  if(mgga) // meta-GGA
    xc_mgga_exc_vxc(&func, N, &rho[0], &sigma[0], &lapl_rho[0], &tau[0], &excwrk[0], &vxcwrk[0], &vsigmawrk[0], &vlapl_rhowrk[0], &vtauwrk[0]);
  else if(gga) // GGA
    xc_gga_exc_vxc(&func, N, &rho[0], &sigma[0], &excwrk[0], &vxcwrk[0], &vsigmawrk[0]);
  else // LDA
    xc_lda_exc_vxc(&func, N, &rho[0], &excwrk[0], &vxcwrk[0]);

  // Sum results
  for(size_t i=0;i<excwrk.size();i++)
    exc[i]+=excwrk[i];
  for(size_t i=0;i<vxcwrk.size();i++)
    vxc[i]+=vxcwrk[i];
  if(gga || mgga)
    for(size_t i=0;i<vsigmawrk.size();i++)
      vsigma[i]+=vsigmawrk[i];
  if(mgga)
    for(size_t i=0;i<vtau.size();i++) {
      vtau[i]+=vtauwrk[i];
      vlapl_rho[i]+=vlapl_rho[i];
    }
  
  // Free functional
  xc_func_end(&func);
}

double AtomGrid::eval_Exc() const {
  double Exc=0.0;

  if(!polarized)
    for(size_t i=0;i<grid.size();i++)
      Exc+=exc[i]*rho[i]*grid[i].w;
  else
    for(size_t i=0;i<grid.size();i++)
      Exc+=exc[i]*(rho[2*i]+rho[2*i+1])*grid[i].w;
  
  return Exc;
}

void AtomGrid::eval_Fxc(arma::mat & H) const {
  double xcfac;

  if(polarized) {
    ERROR_INFO();
    throw std::runtime_error("Refusing to compute restricted Fock matrix with unrestricted density.\n");
  }

  // LDA part
  for(size_t ip=0;ip<grid.size();ip++) {
    // Factor in common for basis functions
    xcfac=grid[ip].w*vxc[ip];

    // Loop over functions on grid point
    size_t first=grid[ip].f0;
    size_t last=first+grid[ip].nf;

    for(size_t ii=first;ii<last;ii++) {
      // Get index of function
      size_t i=flist[ii].ind;
      for(size_t jj=first;jj<last;jj++) {
	size_t j=flist[jj].ind;

	H(i,j)+=xcfac*flist[ii].f*flist[jj].f;
      }
    }
  }

  // GGA part
  if(do_gga) {
    double xcvec[3];

    for(size_t ip=0;ip<grid.size();ip++) {
      // Factor in common for basis functions
      for(int ic=0;ic<3;ic++)
	xcvec[ic]=2.0*grid[ip].w*vsigma[ip]*grho[3*ip+ic];
   
      // Loop over functions on grid point
      size_t first=grid[ip].f0;
      size_t last=first+grid[ip].nf;
      
      for(size_t ii=first;ii<last;ii++) {
	// Get index of function
	size_t i=flist[ii].ind;
	for(size_t jj=first;jj<last;jj++) {
	  size_t j=flist[jj].ind;
	  
	  for(int ic=0;ic<3;ic++) {	  
	    H(i,j)+=xcvec[ic]*(glist[3*ii+ic]*flist[jj].f + flist[ii].f*glist[3*jj+ic]);
	  }
	}
      }
    }
  }

  // Meta-GGA
  if(do_mgga) {
    double bf_lapl;
    double bf_gdot;

    double lfac, kfac;

    for(size_t ip=0;ip<grid.size();ip++) {
      // Factors in common for basis functions
      lfac=grid[ip].w*vlapl_rho[ip];
      kfac=grid[ip].w*vtau[ip];
   
      // Loop over functions on grid point
      size_t first=grid[ip].f0;
      size_t last=first+grid[ip].nf;
      
      for(size_t ii=first;ii<last;ii++) {
	// Get index of function
	size_t i=flist[ii].ind;
	for(size_t jj=first;jj<last;jj++) {
	  size_t j=flist[jj].ind;

	  // Laplacian and kinetic energy density
	  bf_lapl=flist[ii].f*llist[jj] + llist[ii]*flist[jj].f;
	  bf_gdot=glist[3*ii]*glist[3*jj] + glist[3*ii+1]*glist[3*jj+1] + glist[3*ii+2]*glist[3*jj+2];
	  
	  // TODO - check factor
	  H(i,j)+=0.5*kfac*bf_gdot + lfac*(bf_lapl + 2.0*bf_gdot);
	}
      }
    }
  }
}

void AtomGrid::eval_Fxc(std::vector<double> & H) const {
  double xcfac;

  if(polarized) {
    ERROR_INFO();
    throw std::runtime_error("Refusing to compute restricted Fock matrix with unrestricted density.\n");
  }

  // LDA part
  for(size_t ip=0;ip<grid.size();ip++) {
    // Factor in common for basis functions
    xcfac=grid[ip].w*vxc[ip];

    // Loop over functions on grid point
    size_t first=grid[ip].f0;
    size_t last=first+grid[ip].nf;

    for(size_t ii=first;ii<last;ii++) {
      // Get index of function
      size_t i=flist[ii].ind;
      
      H[i]+=xcfac*flist[ii].f*flist[ii].f;
    }
  }

  // GGA part
  if(do_gga) {
    double xcvec[3];

    for(size_t ip=0;ip<grid.size();ip++) {
      // Factor in common for basis functions
      for(int ic=0;ic<3;ic++)
	xcvec[ic]=2.0*grid[ip].w*vsigma[ip]*grho[3*ip+ic];
   
      // Loop over functions on grid point
      size_t first=grid[ip].f0;
      size_t last=first+grid[ip].nf;
      
      for(size_t ii=first;ii<last;ii++) {
	// Get index of function
	size_t i=flist[ii].ind;
	
	for(int ic=0;ic<3;ic++) {
	  H[i]+=xcvec[ic]*2.0*glist[3*ii+ic]*flist[ii].f;
	}
      }
    }
  }

  // Meta-GGA
  if(do_mgga) {
    double bf_lapl;
    double bf_gdot;

    double kfac;
    double lfac;

    for(size_t ip=0;ip<grid.size();ip++) {
      // Factors in common for basis functions
      lfac=grid[ip].w*vlapl_rho[ip];
      kfac=grid[ip].w*vtau[ip];
   
      // Loop over functions on grid point
      size_t first=grid[ip].f0;
      size_t last=first+grid[ip].nf;
      
      for(size_t ii=first;ii<last;ii++) {
	// Get index of function
	size_t i=flist[ii].ind;
	
	// Laplacian and kinetic energy density
	bf_lapl=2.0*flist[ii].f*llist[ii];
	bf_gdot=glist[3*ii]*glist[3*ii] + glist[3*ii+1]*glist[3*ii+1] + glist[3*ii+2]*glist[3*ii+2];
	
	// TODO - check factor
	H[i]+=0.5*kfac*bf_gdot + lfac*(bf_lapl + 2.0*bf_gdot);
      }
    }
  }
}

void AtomGrid::eval_Fxc(arma::mat & Ha, arma::mat & Hb) const {
  double xcfaca, xcfacb;

  if(!polarized) {
    ERROR_INFO();
    throw std::runtime_error("Refusing to compute unrestricted Fock matrix with restricted density.\n");
  }

  // LDA part
  for(size_t ip=0;ip<grid.size();ip++) {
    // Factor in common for basis functions
    xcfaca=grid[ip].w*vxc[2*ip];
    xcfacb=grid[ip].w*vxc[2*ip+1];

    // Loop over functions on grid point
    size_t first=grid[ip].f0;
    size_t last=first+grid[ip].nf;

    for(size_t ii=first;ii<last;ii++) {
      // Get index of function
      size_t i=flist[ii].ind;
      for(size_t jj=first;jj<last;jj++) {
	size_t j=flist[jj].ind;

	Ha(i,j)+=xcfaca*flist[ii].f*flist[jj].f;
	Hb(i,j)+=xcfacb*flist[ii].f*flist[jj].f;
      }
    }
  }

  // GGA part
  if(do_gga) {
    double xcveca[3], xcvecb[3];
    
    for(size_t ip=0;ip<grid.size();ip++) {
      // Factor in common for basis functions
      for(int ic=0;ic<3;ic++) {
	xcveca[ic]=grid[ip].w*(2.0*vsigma[3*ip]  *grho[6*ip+ic]   + vsigma[3*ip+1]*grho[6*ip+3+ic]);
	xcvecb[ic]=grid[ip].w*(2.0*vsigma[3*ip+2]*grho[6*ip+3+ic] + vsigma[3*ip+1]*grho[6*ip+ic]);
      }

      // Loop over functions on grid point
      size_t first=grid[ip].f0;
      size_t last=first+grid[ip].nf;
      
      for(size_t ii=first;ii<last;ii++) {
	// Get index of function
	size_t i=flist[ii].ind;
	for(size_t jj=first;jj<last;jj++) {
	  size_t j=flist[jj].ind;
	  
	  for(int ic=0;ic<3;ic++) {	  
	    Ha(i,j)+=xcveca[ic]*(glist[3*ii+ic]*flist[jj].f + flist[ii].f*glist[3*jj+ic]);
	    Hb(i,j)+=xcvecb[ic]*(glist[3*ii+ic]*flist[jj].f + flist[ii].f*glist[3*jj+ic]);
	  }
	}
      }
    }
  }

  // Meta-GGA
  if(do_mgga) {
    double bf_lapl;
    double bf_gdot;

    double kfaca, kfacb;
    double lfaca, lfacb;

    for(size_t ip=0;ip<grid.size();ip++) {
      // Factors in common for basis functions
      lfaca=grid[ip].w*vlapl_rho[2*ip];
      lfacb=grid[ip].w*vlapl_rho[2*ip+1];

      kfaca=grid[ip].w*vtau[ip];
      kfacb=grid[ip].w*vtau[ip+1];
   
      // Loop over functions on grid point
      size_t first=grid[ip].f0;
      size_t last=first+grid[ip].nf;
      
      for(size_t ii=first;ii<last;ii++) {
	// Get index of function
	size_t i=flist[ii].ind;
	for(size_t jj=first;jj<last;jj++) {
	  size_t j=flist[jj].ind;

	  // Laplacian and kinetic energy density
	  bf_lapl=flist[ii].f*llist[jj] + llist[ii]*flist[jj].f;
	  bf_gdot=glist[3*ii]*glist[3*jj] + glist[3*ii+1]*glist[3*jj+1] + glist[3*ii+2]*glist[3*jj+2];
	  
	  Ha(i,j)+=0.5*kfaca*bf_gdot + lfaca*(bf_lapl + 2.0*bf_gdot);
	  Hb(i,j)+=0.5*kfacb*bf_gdot + lfacb*(bf_lapl + 2.0*bf_gdot);
	}
      }
    }
  }
}

void AtomGrid::eval_Fxc(std::vector<double> & Ha, std::vector<double> & Hb) const {
  double xcfaca, xcfacb;

  if(!polarized) {
    ERROR_INFO();
    throw std::runtime_error("Refusing to compute unrestricted Fock matrix with restricted density.\n");
  }

  // LDA part
  for(size_t ip=0;ip<grid.size();ip++) {
    // Factor in common for basis functions
    xcfaca=grid[ip].w*vxc[2*ip];
    xcfacb=grid[ip].w*vxc[2*ip+1];

    // Loop over functions on grid point
    size_t first=grid[ip].f0;
    size_t last=first+grid[ip].nf;

    for(size_t ii=first;ii<last;ii++) {
      // Get index of function
      size_t i=flist[ii].ind;
      
      Ha[i]+=xcfaca*flist[ii].f*flist[ii].f;
      Hb[i]+=xcfacb*flist[ii].f*flist[ii].f;
    }
  }

  // GGA part
  if(do_gga) {
    double xcveca[3], xcvecb[3];

    for(size_t ip=0;ip<grid.size();ip++) {
      // Factor in common for basis functions
      for(int ic=0;ic<3;ic++) {
	xcveca[ic]=grid[ip].w*(2.0*vsigma[3*ip]  *grho[6*ip+ic]   + vsigma[3*ip+1]*grho[6*ip+3+ic]);
	xcvecb[ic]=grid[ip].w*(2.0*vsigma[3*ip+2]*grho[6*ip+3+ic] + vsigma[3*ip+1]*grho[6*ip+ic]);
      }
   
      // Loop over functions on grid point
      size_t first=grid[ip].f0;
      size_t last=first+grid[ip].nf;
      
      for(size_t ii=first;ii<last;ii++) {
	// Get index of function
	size_t i=flist[ii].ind;
	
	for(int ic=0;ic<3;ic++) {
	  Ha[i]+=xcveca[ic]*2.0*glist[3*ii+ic]*flist[ii].f;
	  Hb[i]+=xcvecb[ic]*2.0*glist[3*ii+ic]*flist[ii].f;
	}
      }
    }
  }

  // Meta-GGA
  if(do_mgga) {
    double bf_lapl;
    double bf_gdot;

    double kfaca, kfacb;
    double lfaca, lfacb;

    for(size_t ip=0;ip<grid.size();ip++) {
      // Factors in common for basis functions
      lfaca=grid[ip].w*vlapl_rho[2*ip];
      lfacb=grid[ip].w*vlapl_rho[2*ip+1];

      kfaca=grid[ip].w*vtau[ip];
      kfacb=grid[ip].w*vtau[ip+1];
   
      // Loop over functions on grid point
      size_t first=grid[ip].f0;
      size_t last=first+grid[ip].nf;
      
      for(size_t ii=first;ii<last;ii++) {
	// Get index of function
	size_t i=flist[ii].ind;

	// Laplacian and kinetic energy density
	bf_lapl=2.0*flist[ii].f*llist[ii];
	bf_gdot=glist[3*ii]*glist[3*ii] + glist[3*ii+1]*glist[3*ii+1] + glist[3*ii+2]*glist[3*ii+2];
	
	Ha[i]+=0.5*kfaca*bf_gdot + lfaca*(bf_lapl + 2.0*bf_gdot);
	Hb[i]+=0.5*kfacb*bf_gdot + lfacb*(bf_lapl + 2.0*bf_gdot);
      }
    }
  }
}

AtomGrid::AtomGrid(bool lobatto, double toler) {
  use_lobatto=lobatto;

  // These should really be set separately using the routines below.
  tol=toler;
  do_grad=false;
  do_lapl=false;
}  

void AtomGrid::set_tolerance(double toler) {
  tol=toler;
}

void AtomGrid::check_grad_lapl(int x_func, int c_func) {
  // Do we need gradients?
  do_grad=false;
  if(x_func>0)
    do_grad=do_grad || gradient_needed(x_func);
  if(c_func>0)
    do_grad=do_grad || gradient_needed(c_func);

  // Do we need laplacians?
  do_lapl=false;
  if(x_func>0)
    do_lapl=do_lapl || laplacian_needed(x_func);
  if(c_func>0)
    do_lapl=do_lapl || laplacian_needed(c_func);
}

atomgrid_t AtomGrid::construct(const BasisSet & bas, const arma::mat & P, size_t cenind, int x_func, int c_func, bool verbose) {
  // Construct a grid centered on (x0,y0,z0)
  // with nrad radial shells                         
  // See Köster et al for specifics.

  // Returned info
  atomgrid_t ret;
  ret.ngrid=0;
  ret.nfunc=0;

  Timer t;

  // Store index of center
  ret.atind=cenind;
  // and its coordinates
  ret.cen=bas.get_coords(cenind);

  // Compute necessary number of radial points
  size_t nrad=std::max(20,(int) round(-5*(3*log10(tol)+6-element_row[bas.get_Z(ret.atind)])));

  // Get Chebyshev nodes and weights for radial part
  std::vector<double> xc, wc;
  chebyshev(nrad,xc,wc);

  // Allocate memory
  ret.sh.resize(nrad);

  // Loop over radii
  double rad, jac;
  for(size_t ir=0;ir<xc.size();ir++) {
    // Calculate value of radius
    rad=1.0/M_LN2*log(2.0/(1.0-xc[ir]));

    // Jacobian of transformation is
    jac=1.0/M_LN2/(1.0-xc[ir]);
    // so total quadrature weight is
    double weight=wc[ir]*rad*rad*jac;

    // Store shell data
    ret.sh[ir].r=rad;
    ret.sh[ir].w=weight;
    ret.sh[ir].l=3;
  }
  
  // Number of basis functions
  size_t Nbf=bas.get_Nbf();

  // Determine limit for angular quadrature
  int lmax=(int) ceil(5.0-6.0*log10(tol));

  // Old and new diagonal elements of Hamiltonian
  std::vector<double> Hold(Nbf), Hnew(Nbf);
  zero(Hold);

  // Maximum difference of diagonal elements of Hamiltonian
  double maxdiff;

  // Now, determine actual quadrature limits shell by shell
  for(size_t ir=0;ir<ret.sh.size();ir++) {

    do {
      // Clear current grid points and function values
      free();

      // Form radial shell
      if(use_lobatto)
	add_lobatto_shell(ret,ir);
      else
	add_lebedev_shell(ret,ir);
      // Compute Becke weights for radial shell
      becke_weights(bas,ret,ir);
      // Prune points with small weight
      prune_points(1e-8*tol,ret.sh[ir]);

      // Compute values of basis functions
      compute_bf(bas,ret,ir);

      // Compute density
      update_density(P);

      // Clean out Hamiltonian
      zero(Hnew);

      // Compute exchange and correlation.
      init_xc();
      // Compute the functionals
      if(x_func>0)
	compute_xc(x_func);
      if(c_func>0)
	compute_xc(c_func);
      // and construct the Fock matrix
      eval_Fxc(Hnew);
      
      // Compute maximum difference of diagonal elements of Fock matrix
      maxdiff=0.0;
      for(size_t i=0;i<Nbf;i++)
	if(fabs(Hold[i]-Hnew[i])>maxdiff)
	  maxdiff=fabs(Hold[i]-Hnew[i]);

      // Copy contents
      for(size_t i=0;i<Nbf;i++)
	Hold[i]=Hnew[i];

      // Increment order if tolerance not achieved.
      if(maxdiff>tol/xc.size()) {
	if(use_lobatto)
	  ret.sh[ir].l+=2;
	else {
	  // Need to determine what is next order of Lebedev
	  // quadrature that is supported.
	  ret.sh[ir].l=next_lebedev(ret.sh[ir].l);
	}
      }
    } while(maxdiff>tol/xc.size() && ret.sh[ir].l<=lmax);

    // Increase number of points and function values
    ret.ngrid+=grid.size();
    ret.nfunc+=flist.size();
  }
  
  // Free memory once more
  free();
  
  if(verbose) {
    printf("\t%4u %7u %8u %s\n",(unsigned int) ret.atind+1,(unsigned int) ret.ngrid,(unsigned int) ret.nfunc,t.elapsed().c_str());
    fflush(stdout);
  }

  return ret;
}

atomgrid_t AtomGrid::construct(const BasisSet & bas, const arma::mat & Pa, const arma::mat & Pb, size_t cenind, int x_func, int c_func, bool verbose) {
  // Construct a grid centered on (x0,y0,z0)
  // with nrad radial shells                         
  // See Köster et al for specifics.

  Timer t;

  // Returned info
  atomgrid_t ret;
  ret.ngrid=0;
  ret.nfunc=0;

  // Store index of center
  ret.atind=cenind;
  // and its coordinates
  ret.cen=bas.get_coords(cenind);

  // Compute necessary number of radial points
  size_t nrad=std::max(20,(int) round(-5*(3*log10(tol)+6-element_row[bas.get_Z(ret.atind)])));

  // Get Chebyshev nodes and weights for radial part
  std::vector<double> xc, wc;
  chebyshev(nrad,xc,wc);

  // Allocate memory
  ret.sh.resize(nrad);

  // Loop over radii
  double rad, jac;
  for(size_t ir=0;ir<xc.size();ir++) {
    // Calculate value of radius
    rad=1.0/M_LN2*log(2.0/(1.0-xc[ir]));

    // Jacobian of transformation is
    jac=1.0/M_LN2/(1.0-xc[ir]);
    // so total quadrature weight is
    double weight=wc[ir]*rad*rad*jac;

    // Store shell data
    ret.sh[ir].r=rad;
    ret.sh[ir].w=weight;
    ret.sh[ir].l=3;
  }
  
  // Number of basis functions
  size_t Nbf=bas.get_Nbf();

  // Determine limit for angular quadrature
  int lmax=(int) ceil(5.0-6.0*log10(tol));

  // Old and new diagonal elements of Hamiltonian
  std::vector<double> Haold(Nbf), Hanew(Nbf);
  std::vector<double> Hbold(Nbf), Hbnew(Nbf);

  zero(Haold);
  zero(Hanew);
  zero(Hbold);
  zero(Hbnew);

  // Maximum difference of diagonal elements of Hamiltonian
  double maxdiff;

  // Now, determine actual quadrature limits shell by shell
  for(size_t ir=0;ir<ret.sh.size();ir++) {

    do {
      // Clear current grid points and function values
      free();

      // Form radial shell
      if(use_lobatto)
	add_lobatto_shell(ret,ir);
      else
	add_lebedev_shell(ret,ir);
      // Compute Becke weights for radial shell
      becke_weights(bas,ret,ir);
      // Prune points with small weight
      prune_points(1e-8*tol,ret.sh[ir]);

      // Compute values of basis functions
      compute_bf(bas,ret,ir);

      // Compute density
      update_density(Pa,Pb);

      // Clean out Hamiltonian
      zero(Hanew);
      zero(Hbnew);

      // Compute exchange and correlation.
      init_xc();
      // Compute the functionals
      if(x_func>0)
	compute_xc(x_func);
      if(c_func>0)
	compute_xc(c_func);
      // and construct the Fock matrices
      eval_Fxc(Hanew,Hbnew);
      
      // Compute maximum difference of diagonal elements of Fock matrix
      maxdiff=0.0;
      for(size_t i=0;i<Nbf;i++) {
	double tmp=std::max(fabs(Hanew[i]-Haold[i]),fabs(Hbnew[i]-Hbold[i]));
	if(tmp>maxdiff)
	  maxdiff=tmp;
      }

      // Copy contents
      for(size_t i=0;i<Nbf;i++) {
	Haold[i]=Hanew[i];
	Hbold[i]=Hbnew[i];
      }

      // Increment order if tolerance not achieved.
      if(maxdiff>tol/xc.size()) {
	if(use_lobatto)
	  ret.sh[ir].l+=2;
	else {
	  // Need to determine what is next order of Lebedev
	  // quadrature that is supported.
	  ret.sh[ir].l=next_lebedev(ret.sh[ir].l);
	}
      }
    } while(maxdiff>tol/xc.size() && ret.sh[ir].l<=lmax);

    // Increase number of points and function values
    ret.ngrid+=grid.size();
    ret.nfunc+=flist.size();
  }

  // Free memory once more
  free();
  
  if(verbose) {
    printf("\t%4u %7u %8u %s\n",(unsigned int) ret.atind+1,(unsigned int) ret.ngrid,(unsigned int) ret.nfunc,t.elapsed().c_str());
    fflush(stdout);
  }

  return ret;
}

AtomGrid::~AtomGrid() {
}

void AtomGrid::form_grid(const BasisSet & bas, atomgrid_t & g) {
  // Clear anything that already exists
  free();

  // Check allocation
  grid.reserve(g.ngrid);

  // Loop over radial shells
  for(size_t ir=0;ir<g.sh.size();ir++) {
    // Add grid points
    if(use_lobatto)
      add_lobatto_shell(g,ir);			
    else
      add_lebedev_shell(g,ir);
    // Do Becke weights
    becke_weights(bas,g,ir);
    // Prune points with small weight
    prune_points(1e-8*tol,g.sh[ir]);
  }
}

void AtomGrid::compute_bf(const BasisSet & bas, const atomgrid_t & g) {
  // Check allocation
  flist.reserve(g.nfunc);
  if(do_grad)
    glist.reserve(3*g.nfunc);
  if(do_lapl)
    llist.reserve(g.nfunc);

  // Loop over radial shells
  for(size_t ir=0;ir<g.sh.size();ir++) {
    compute_bf(bas,g,ir);
  }
}
  

void AtomGrid::compute_bf(const BasisSet & bas, const atomgrid_t & g, size_t irad) {
  // Compute values of relevant basis functions on irad:th shell

  //  fprintf(stderr,"Computing bf of rad shell %i of atom %i\n",(int) g.atind,(int) irad);

  // Get distances of other nuclei
  std::vector<double> nucdist=bas.get_nuclear_distances(g.atind);

  // Current radius
  double rad=g.sh[irad].r;

  // Get ranges of shells
  std::vector<double> shran=bas.get_shell_ranges();

  // Indices of shells to compute
  std::vector<size_t> compute_shells;

  // Determine which shells might contribute
  for(size_t inuc=0;inuc<bas.get_Nnuc();inuc++) {
    // Determine closest distance of nucleus
    double dist=fabs(nucdist[inuc]-rad);
    // Get indices of shells centered on nucleus
    std::vector<size_t> shellinds=bas.get_shell_inds(inuc);

    // Loop over shells on nucleus
    for(size_t ish=0;ish<shellinds.size();ish++) {
      
      // Shell is relevant if range is larger than minimal distance
      if(dist<=shran[shellinds[ish]]) {
	// Add shell to list of shells to compute
	compute_shells.push_back(shellinds[ish]);
      }
    }
  }

  if(do_lapl) {
    // Loop over points
    for(size_t ip=g.sh[irad].ind0;ip<g.sh[irad].ind0+g.sh[irad].np;ip++) {
      // Store index of first function on grid point
      grid[ip].f0=flist.size();
      // Number of functions on point
      grid[ip].nf=0;
      
      // Loop over shells
      for(size_t ish=0;ish<compute_shells.size();ish++) {
	// Center of shell is
	coords_t shell_center=bas.get_center(compute_shells[ish]);
	// Compute distance of point to center of shell
	double shell_dist=norm(shell_center-grid[ip].r);
	// Add shell to point if it is within the range of the shell
	if(shell_dist<shran[compute_shells[ish]]) {
	  // Index of first function on shell is
	  size_t ind0=bas.get_first_ind(compute_shells[ish]);
	  
	  // Compute values of basis functions
	  arma::vec fval=bas.eval_func(compute_shells[ish],grid[ip].r.x,grid[ip].r.y,grid[ip].r.z);
	  // and their gradients
	  arma::mat gval=bas.eval_grad(compute_shells[ish],grid[ip].r.x,grid[ip].r.y,grid[ip].r.z);
	  // and their laplacians
	  arma::vec lval=bas.eval_lapl(compute_shells[ish],grid[ip].r.x,grid[ip].r.y,grid[ip].r.z);
	  
	  // and add them to the list
	  bf_f_t hlp;
	  for(size_t ifunc=0;ifunc<fval.n_elem;ifunc++) {
	    // Index of function is
	    hlp.ind=ind0+ifunc;
	    // Value is
	    hlp.f=fval(ifunc);
	    // Add to stack
	    flist.push_back(hlp);
	    // and gradient
	    for(int ic=0;ic<3;ic++)
	      glist.push_back(gval(ifunc,ic));
	    // and laplacian
	    llist.push_back(lval(ifunc));
	    
	    // Increment number of functions in point
	    grid[ip].nf++;
	  }
	}
      }
    }
  } else if(do_grad) {
    // Loop over points
    for(size_t ip=g.sh[irad].ind0;ip<g.sh[irad].ind0+g.sh[irad].np;ip++) {
      // Store index of first function on grid point
      grid[ip].f0=flist.size();
      // Number of functions on point
      grid[ip].nf=0;
      
      // Loop over shells
      for(size_t ish=0;ish<compute_shells.size();ish++) {
	// Center of shell is
	coords_t shell_center=bas.get_center(compute_shells[ish]);
	// Compute distance of point to center of shell
	double shell_dist=norm(shell_center-grid[ip].r);
	// Add shell to point if it is within the range of the shell
	if(shell_dist<shran[compute_shells[ish]]) {
	  // Index of first function on shell is
	  size_t ind0=bas.get_first_ind(compute_shells[ish]);
	  
	  // Compute values of basis functions
	  arma::vec fval=bas.eval_func(compute_shells[ish],grid[ip].r.x,grid[ip].r.y,grid[ip].r.z);
	  // and their gradients
	  arma::mat gval=bas.eval_grad(compute_shells[ish],grid[ip].r.x,grid[ip].r.y,grid[ip].r.z);
	  
	  // and add them to the list
	  bf_f_t hlp;
	  for(size_t ifunc=0;ifunc<fval.n_elem;ifunc++) {
	    // Index of function is
	    hlp.ind=ind0+ifunc;
	    // Value is
	    hlp.f=fval(ifunc);
	    // Add to stack
	    flist.push_back(hlp);
	    // and gradient, too
	    for(int ic=0;ic<3;ic++)
	      glist.push_back(gval(ifunc,ic));
	    
	    // Increment number of functions in point
	    grid[ip].nf++;
	  }
	}
      }
    }
  } else {
    // Loop over points
    for(size_t ip=g.sh[irad].ind0;ip<g.sh[irad].ind0+g.sh[irad].np;ip++) {
      // Store index of first function on grid point
      grid[ip].f0=flist.size();
      // Number of functions on point
      grid[ip].nf=0;
      
      // Loop over shells
      for(size_t ish=0;ish<compute_shells.size();ish++) {
	// Center of shell is
	coords_t shell_center=bas.get_center(compute_shells[ish]);
	// Compute distance of point to center of shell
	double shell_dist=norm(shell_center-grid[ip].r);
	// Add shell to point if it is within the range of the shell
	if(shell_dist<shran[compute_shells[ish]]) {
	  // Index of first function on shell is
	  size_t ind0=bas.get_first_ind(compute_shells[ish]);

	  // Compute values of basis functions
	  arma::vec fval=bas.eval_func(compute_shells[ish],grid[ip].r.x,grid[ip].r.y,grid[ip].r.z);
	  
	  // and add them to the list
	  bf_f_t hlp;
	  for(size_t ifunc=0;ifunc<fval.n_elem;ifunc++) {
	    // Index of function is
	    hlp.ind=ind0+ifunc;
	    // Value is
	    hlp.f=fval(ifunc);

	    // Add to stack
	    flist.push_back(hlp);
	    // Increment number of functions in point
	    grid[ip].nf++;
	  }
	}
      }
    }
  }
}

DFTGrid::DFTGrid(const BasisSet * bas, bool ver, bool lobatto) {
  basp=bas;
  verbose=ver;
  use_lobatto=lobatto;

  // Allocate atomic grids
  grids.resize(bas->get_Nnuc());

  // Allocate work grids
#ifdef _OPENMP
  int nth=omp_get_max_threads();
  for(int i=0;i<nth;i++)
    wrk.push_back(AtomGrid(lobatto));
#else
  wrk.push_back(AtomGrid(lobatto));
#endif
}

DFTGrid::~DFTGrid() {
}

void DFTGrid::construct(const arma::mat & P, double tol, int x_func, int c_func) {

  // Add all atoms
  if(verbose) {
    printf("Constructing DFT grid.\n");
    printf("\t%4s %7s %8s %s\n","atom","Npoints","Nfuncs","t");
    fflush(stdout);
  }

  // Set tolerances
  for(size_t i=0;i<wrk.size();i++)
    wrk[i].set_tolerance(tol);
  // Check necessity of gradients and laplacians
  for(size_t i=0;i<wrk.size();i++)
    wrk[i].check_grad_lapl(x_func,c_func);

  Timer t;

  size_t Nat=basp->get_Nnuc();

#ifdef _OPENMP
#pragma omp parallel
  {
    int ith=omp_get_thread_num();
#pragma omp for schedule(dynamic,1)
    for(size_t i=0;i<Nat;i++) {
      grids[i]=wrk[ith].construct(*basp,P,i,x_func,c_func,verbose);
    }
  }
#else
  for(size_t i=0;i<Nat;i++)
    grids[i]=wrk[0].construct(*basp,P,i,x_func,c_func,verbose);
#endif
  
  if(verbose) {
    printf("DFT grid constructed in %s.\n",t.elapsed().c_str());
    fflush(stdout);
  }
}

void DFTGrid::construct(const arma::mat & Pa, const arma::mat & Pb, double tol, int x_func, int c_func) {
  // Add all atoms
  if(verbose) {
    printf("\t%4s %7s %8s %s\n","atom","Npoints","Nfuncs","t");
    fflush(stdout);
  }

  // Set tolerances
  for(size_t i=0;i<wrk.size();i++)
    wrk[i].set_tolerance(tol);
  // Check necessity of gradients and laplacians
  for(size_t i=0;i<wrk.size();i++)
    wrk[i].check_grad_lapl(x_func,c_func);

  Timer t;

  size_t Nat=basp->get_Nnuc();

#ifdef _OPENMP
#pragma omp parallel
  {
    int ith=omp_get_thread_num();
#pragma omp for schedule(dynamic,1)
    for(size_t i=0;i<Nat;i++)
      grids[i]=wrk[ith].construct(*basp,Pa,Pb,i,x_func,c_func,verbose);
  }
#else
  for(size_t i=0;i<Nat;i++)
    grids[i]=wrk[0].construct(*basp,Pa,Pb,i,x_func,c_func,verbose);
#endif  

  if(verbose) {
    printf("DFT grid constructed in %s.\n",t.elapsed().c_str());
    fflush(stdout);
  }
}

size_t DFTGrid::get_Npoints() const {
  size_t np=0;
  for(size_t i=0;i<grids.size();i++)
    np+=grids[i].ngrid;
  return np;
}

size_t DFTGrid::get_Nfuncs() const {
  size_t nf=0;
  for(size_t i=0;i<grids.size();i++)
    nf+=grids[i].nfunc;
  return nf;
}

#ifdef CONSISTENCYCHECK
void DFTGrid::eval_Fxc(int x_func, int c_func, const arma::mat & P, arma::mat & H, double & Exc, double & Nel) {
  // Work arrays
  arma::mat Ha, Hb;
  Ha=H;
  Hb=H;

  Ha.zeros(P.n_rows,P.n_cols);
  Hb.zeros(P.n_rows,P.n_cols);

  eval_Fxc(x_func,c_func,P/2.0,P/2.0,Ha,Hb,Exc,Nel);
  H=(Ha+Hb)/2.0;
}
#else
void DFTGrid::eval_Fxc(int x_func, int c_func, const arma::mat & P, arma::mat & H, double & Exc, double & Nel) {
  // Clear Hamiltonian
  H.zeros(P.n_rows,P.n_cols);
  // Clear exchange-correlation energy
  Exc=0.0;
  // Clear number of electrons
  Nel=0.0;

#ifdef _OPENMP
  // Get (maximum) number of threads
  int maxt=omp_get_max_threads();

  // Stack of work arrays
  std::vector<arma::mat> Hwrk;
  std::vector<double> Nelwrk;
  std::vector<double> Excwrk;

  for(int i=0;i<maxt;i++) {
    Hwrk.push_back(arma::mat(H.n_rows,H.n_cols));
    Hwrk[i].zeros();
    Nelwrk.push_back(0.0);
    Excwrk.push_back(0.0);
  }

#pragma omp parallel shared(Hwrk,Nelwrk,Excwrk)
  { // Begin parallel region
    
    // Current thread is
    int ith=omp_get_thread_num();

#pragma omp for schedule(dynamic,1)
    // Loop over atoms
    for(size_t i=0;i<grids.size();i++) {
      // Change atom and create grid
      wrk[ith].form_grid(*basp,grids[i]);
      // Compute basis functions
      wrk[ith].compute_bf(*basp,grids[i]);
      
      // Update density
      wrk[ith].update_density(P);
      // Update number of electrons
      Nelwrk[ith]+=wrk[ith].compute_Nel();
      
      // Initialize the arrays
      wrk[ith].init_xc();
      // Compute the functionals
      if(x_func>0)
	wrk[ith].compute_xc(x_func);
      if(c_func>0)
	wrk[ith].compute_xc(c_func);

      // Evaluate the energy
      Excwrk[ith]+=wrk[ith].eval_Exc();
      // and construct the Fock matrices
      wrk[ith].eval_Fxc(Hwrk[ith]);
      // Free memory
      wrk[ith].free();
    }
  } // End parallel region

  // Sum results
  for(int i=0;i<maxt;i++) {
    H+=Hwrk[i];
    Nel+=Nelwrk[i];
    Exc+=Excwrk[i];
  }
#else
  for(size_t i=0;i<grids.size();i++) {
    // Change atom and create grid
    wrk[0].form_grid(*basp,grids[i]);
    // Compute basis functions
    wrk[0].compute_bf(*basp,grids[i]);

    // Update density
    wrk[0].update_density(P);
    // Update number of electrons
    Nel+=wrk[0].compute_Nel();
    
    // Initialize the arrays
    wrk[0].init_xc();
    // Compute the functionals
    if(x_func>0)
      wrk[0].compute_xc(x_func);
    if(c_func>0)
      wrk[0].compute_xc(c_func);

    // Evaluate the energy
    Exc+=wrk[0].eval_Exc();
    // and construct the Fock matrices
    wrk[0].eval_Fxc(H);
    // Free memory
    wrk[0].free();
  }
#endif
}
#endif

void DFTGrid::eval_Fxc(int x_func, int c_func, const arma::mat & Pa, const arma::mat & Pb, arma::mat & Ha, arma::mat & Hb, double & Exc, double & Nel) {
  // Clear Hamiltonian
  Ha.zeros(Pa.n_rows,Pa.n_cols);
  Hb.zeros(Pb.n_rows,Pb.n_cols);
  // Clear exchange-correlation energy
  Exc=0.0;
  // Clear number of electrons
  Nel=0.0;

#ifdef _OPENMP
  // Get (maximum) number of threads
  int maxt=omp_get_max_threads();

  // Stack of work arrays
  std::vector<arma::mat> Hawrk, Hbwrk;
  std::vector<double> Nelwrk;
  std::vector<double> Excwrk;

  for(int i=0;i<maxt;i++) {
    Hawrk.push_back(arma::mat(Ha.n_rows,Ha.n_cols));
    Hawrk[i].zeros();

    Hbwrk.push_back(arma::mat(Hb.n_rows,Hb.n_cols));
    Hbwrk[i].zeros();

    Nelwrk.push_back(0.0);
    Excwrk.push_back(0.0);
  }

#pragma omp parallel shared(Hawrk,Hbwrk,Nelwrk,Excwrk)
  { // Begin parallel region
    
    // Current thread is
    int ith=omp_get_thread_num();
    
#pragma omp for schedule(dynamic,1)
    // Loop over atoms
    for(size_t i=0;i<grids.size();i++) {

      // Change atom and create grid
      wrk[ith].form_grid(*basp,grids[i]);
      // Compute basis functions
      wrk[ith].compute_bf(*basp,grids[i]);
      
      // Update density
      wrk[ith].update_density(Pa,Pb);
      // Update number of electrons
      Nel+=wrk[ith].compute_Nel();

      // Initialize the arrays
      wrk[ith].init_xc();
      // Compute the functionals
      if(x_func>0)
        wrk[ith].compute_xc(x_func);
      if(c_func>0)
        wrk[ith].compute_xc(c_func);

      // Evaluate the energy
      Exc+=wrk[ith].eval_Exc();
      // and construct the Fock matrices
      wrk[ith].eval_Fxc(Hawrk[ith],Hbwrk[ith]);
           
      // Free memory
      wrk[ith].free();
    }
  } // End parallel region
  
  // Sum results
  for(int i=0;i<maxt;i++) {
    Ha+=Hawrk[i];
    Hb+=Hbwrk[i];
    Nel+=Nelwrk[i];
    Exc+=Excwrk[i];
  }
#else
  // Loop over atoms
  for(size_t i=0;i<grids.size();i++) {
    // Change atom and create grid
    wrk[0].form_grid(*basp,grids[i]);
    // Compute basis functions
    wrk[0].compute_bf(*basp,grids[i]);
    
    // Update density
    wrk[0].update_density(Pa,Pb);
    // Update number of electrons
    Nel+=wrk[0].compute_Nel();

    // Initialize the arrays
    wrk[0].init_xc();
    // Compute the functionals
    if(x_func>0)
      wrk[0].compute_xc(x_func);
    if(c_func>0)
      wrk[0].compute_xc(c_func);

    // Evaluate the energy
    Exc+=wrk[0].eval_Exc();
    // and construct the Fock matrices
    wrk[0].eval_Fxc(Ha,Hb);
 
    // Free memory
    wrk[0].free();
  }
#endif
}
