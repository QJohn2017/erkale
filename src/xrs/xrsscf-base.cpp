/*
 *                This source code is part of
 *
 *                     E  R  K  A  L  E
 *                             -
 *                       HF/DFT from Hel
 *
 * Written by Jussi Lehtola, 2010-2011
 * Copyright (c) 2010-2011, Jussi Lehtola
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */


#include <cfloat>
#include "xrsscf.h"
#include "adiis.h"
#include "diis.h"
#include "broyden.h"
#include "dftfuncs.h"
#include "dftgrid.h"
#include "linalg.h"
#include "mathf.h"
#include "stringutil.h"
#include "timer.h"

 // Initial mixing factor
#define INITMIX 0.25

#define ROUGHTOL 1e-8


XRSSCF::XRSSCF(const BasisSet & basis, const Settings & set, Checkpoint & chkpt, bool sp) : SCF(basis,set,chkpt) {
  spin=sp;

  // Get number of alpha and beta electrons
  get_Nel_alpha_beta(basis.Ztot()-set.get_int("Charge"),set.get_int("Multiplicity"),nocca,noccb);
}

XRSSCF::~XRSSCF() {
}

void XRSSCF::set_frozen(const arma::mat & C, size_t ind) {
  if(ind-1>=freeze.size())
    freeze.resize(ind+1);

  freeze[ind].resize(C.n_cols);
  for(size_t i=0;i<C.n_cols;i++)
    freeze[ind][i]=C.col(i);
}

/// Get excited atom from atomlist
size_t get_excited_atom_idx(std::vector<atom_t> & at) {
  // Indices of atoms with frozen core
  size_t ind=0;
  int found=0;

  for(size_t i=0;i<at.size();i++) {
    if(at[i].el.size()>3 && at[i].el.substr(at[i].el.size()-3,3)=="-Xc") {
      // Excited atom.
      at[i].el=at[i].el.substr(0,at[i].el.size()-3);
      ind=i;
      found++;
    }
  }

  if(found==0) {
    throw std::runtime_error("Need an atom to excite for XRS calculation!\n");
    return 0;
  } else if(found==1) {
    // Found a single excited atom.
    return(ind);
  } else {
    // Many atoms excited
    throw std::runtime_error("Error - cannot excite many atoms!\n");
    return 0;
  }
}

/// Compute center and rms width of orbitals.
arma::mat compute_center_width(const arma::mat & C, const BasisSet & basis, size_t nocc) {
  // ret(io,0-2) = x,y,z, ret(io,3) = width
  arma::mat ret(nocc,4);

  // Moment matrix
  std::vector<arma::mat> mom1=basis.moment(1);

  // Compute centers and widths
  for(size_t io=0;io<nocc;io++) {
    // Electron density for orbital
    arma::mat dens=C.col(io)*arma::trans(C.col(io));

    // Compute center of orbital
    ret(io,0)=arma::trace(dens*mom1[getind(1,0,0)]);
    ret(io,1)=arma::trace(dens*mom1[getind(0,1,0)]);
    ret(io,2)=arma::trace(dens*mom1[getind(0,0,1)]);

    // Then, compute r_{rms}=\sqrt{<r^2>} around center
    std::vector<arma::mat> mom2=basis.moment(2,ret(io,0),ret(io,1),ret(io,2));
    ret(io,3)=sqrt(arma::trace(dens*(mom2[getind(2,0,0)]+mom2[getind(0,2,0)]+mom2[getind(0,0,2)])));
  }

  return ret;
}


/// Find excited core orbital, located on atom idx
size_t find_excited_orb(const arma::mat & C, const BasisSet & basis, size_t atom_idx, size_t nocc) {
  // Coordinates of the excited atom
  coords_t xccoord=basis.get_coords(atom_idx);

  // Measures of goodness for orbitals.
  arma::vec mog(nocc);
  // Compute centers and widths
  arma::mat cenwidth=compute_center_width(C,basis,nocc);

  for(size_t io=0;io<nocc;io++) {
    double dx=xccoord.x-cenwidth(io,0);
    double dy=xccoord.y-cenwidth(io,1);
    double dz=xccoord.z-cenwidth(io,2);
    double w=cenwidth(io,3);

    // Compute measure of goodness - this should be quite small.
    mog(io)=w*sqrt(dx*dx+dy*dy+dz*dz);
  }

  // Find minimum mog
  double minmog=mog(0);
  size_t minind=0;
  for(size_t io=1;io<nocc;io++)
    if(mog(io)<minmog) {
      minmog=mog(io);
      minind=io;
    }

  // Amount of orbitals localized on atom.
  std::vector<size_t> loc_idx;

  // Loop over orbitals.
  for(size_t io=0;io<nocc;io++) {
    // Index of localization.
    double mindist=DBL_MAX;
    size_t locind=0;

    // Determine localization of orbital. Loop over nuclei
    for(size_t iat=0;iat<basis.get_Nnuc();iat++) {
      // Coordinates of nucleus
      coords_t atcoord=basis.get_coords(iat);

      double dx=atcoord.x-cenwidth(io,0);
      double dy=atcoord.y-cenwidth(io,1);
      double dz=atcoord.z-cenwidth(io,2);
      double dist=sqrt(dx*dx+dy*dy+dz*dz);

      if(dist<mindist) {
	mindist=dist;
	locind=iat;
      }
    }

    // Determine localization
    if(mindist<1e-3 && cenwidth(io,3)<1) {
      printf("Orbital %i is centered on nucleus %i with rms width %e Å.\n",(int) io+1,(int) locind+1,cenwidth(io,3)/ANGSTROMINBOHR);

      if(locind==atom_idx)
	loc_idx.push_back(io);
    } else {
      printf("Orbital %i is unlocalized.\n",(int) io+1);
    }
  }

  if(loc_idx.size()==0) {
    throw std::runtime_error("Error - could not localize core orbital!\n");
  } else if(loc_idx.size()>1)
    printf("Warning - there is more than one orbital localized on wanted atom.\n");
  
  // Return index of orbital localized on atom
  return minind;
}

void print_info(const arma::mat & C, const arma::vec & E, const BasisSet & basis) {
  // Compute centers and widths
  arma::vec cw=compute_center_width(C,basis,C.n_cols);

  arma::mat S=basis.overlap();

  for(size_t i=0;i<C.n_cols;i++) {
    printf("Orbital %i with energy %f is centered at (% f,% f,% f) Å with rms width %e Å and has norm %e.\n",(int) i+1,E(i),cw(i,0)/ANGSTROMINBOHR,cw(i,1)/ANGSTROMINBOHR,cw(i,2)/ANGSTROMINBOHR,cw(i,3)/ANGSTROMINBOHR,arma::as_scalar(arma::trans(C.col(i))*S*C.col(i)));
  }
}

/// Normal Aufbau occupation
std::vector<double> norm_occ(size_t nocc) {
  std::vector<double> ret(nocc);
  for(size_t i=0;i<nocc;i++) {
    ret[i]=1.0;
  }

  return ret;
}

/// Set fractional occupation on excited orbital
std::vector<double> frac_occ(size_t excited, size_t nocc) {
  std::vector<double> ret(nocc);
  for(size_t i=0;i<nocc;i++) {
    ret[i]=1.0;
  }
  ret[excited]=0.5;

  return ret;
}

/// First excited state; core orbital is not occupied
std::vector<double> exc_occ(size_t excited, size_t nocc) {
  std::vector<double> ret(nocc+1);
  for(size_t i=0;i<nocc+1;i++) {
    ret[i]=1.0;
  }
  ret[excited]=0;

  return ret;
}

bool operator<(const locdist_t & lhs, const locdist_t & rhs) {
  // Sort in increasing value
  return lhs.dist < rhs.dist;
}

size_t localize(const BasisSet & basis, int nocc, size_t xcatom, arma::mat & C) {
  // First, figure out which centers need to be localized upon.

  std::vector<locdist_t> locind;
  // Localize on all the atoms of the same type than the excited atom
  for(size_t i=0;i<basis.get_Nnuc();i++)
    if(stricmp(basis.get_symbol(i),basis.get_symbol(xcatom))==0) {
      locdist_t tmp;
      tmp.ind=i;
      tmp.dist=norm(basis.get_coords(i)-basis.get_coords(xcatom));
      locind.push_back(tmp);
    }
  // Sort in increasing distance
  std::stable_sort(locind.begin(),locind.end());

  printf("Localizing on centers:");
  for(size_t i=0;i<locind.size();i++)
    printf(" %i",(int) locind[i].ind+1);
  printf("\n");

  // Amount of orbitals already localized
  size_t locd=0;
  // Amount of basis functions
  size_t Nbf=basis.get_Nbf();

  // Perform the localization.
  for(size_t i=0;i<locind.size();i++) {
    // The nucleus is
    size_t inuc=locind[i].ind;
    // and it is located at
    coords_t cen=basis.get_coords(inuc);

    // Compute moment integrals around the nucleus
    std::vector<arma::mat> momstack=basis.moment(2,cen.x,cen.y,cen.z);
    // Get matrix which transforms into occupied MO basis
    arma::mat transmat=C.submat(0,locd,Nbf-1,locd+nocc-1);

    // Sum together to get x^2 + y^2 + z^2
    arma::mat rsqmat=momstack[getind(2,0,0)]+momstack[getind(0,2,0)]+momstack[getind(0,0,2)];
    // and transform into the occupied MO basis
    rsqmat=arma::trans(transmat)*rsqmat*transmat;

    // Diagonalize rsq_mo
    arma::vec reig;
    arma::mat rvec;
    eig_sym_ordered(reig,rvec,rsqmat);

    // Rotate occupied orbitals
    C.submat(0,locd,Nbf-1,locd+nocc-1)=transmat*rvec;

    // Increase number of localized orbitals
    locd++;
    // and decrease that of occupied orbitals yet to localize

    nocc--;

    printf("Localized orbital around (%e,%e,%e) with Rrms=%e Å.\n",cen.x,cen.y,cen.z,sqrt(reig(0))/ANGSTROMINBOHR);
  }

  /*
  // Check orthonormality
  arma::mat MOovl=arma::trans(C)*basis.overlap()*C;
  printf("MO orthonormality\n");
  print_sym(MOovl);
  */

  return locd;
}

std::vector<int> symgroups(const arma::mat & C, const arma::mat& S, const std::vector< std::vector<arma::vec> > & freeze) {
  // Initialize groups.
  std::vector<int> gp(C.n_cols,0);

  // Loop over frozen core groups
  for(size_t igp=0;igp<freeze.size();igp++) {
    
    // Compute overlap of orbitals with frozen core orbitals
    std::vector<locdist_t> ovl(C.n_cols);
    for(size_t i=0;i<C.n_cols;i++) {
      
      // Store index
      ovl[i].ind=i;
      // Initialize overlap
      ovl[i].dist=0.0;
      
      // Helper vector
      arma::vec hlp=S*C.col(i);
      
      // Loop over frozen orbitals.
      for(size_t ifz=0;ifz<freeze[igp].size();ifz++) {
	// Compute projection
	double proj=arma::dot(hlp,freeze[igp][ifz]);
	// Increment overlap
	ovl[i].dist+=proj*proj;
      }
    }

    // Sort the projections
    std::sort(ovl.begin(),ovl.end());
    
    // Store the symmetries
    for(size_t i=0;i<freeze[igp].size();i++) {
      // The orbital with the maximum overlap is (remember ovl is now in increasing order)
      size_t maxind=ovl[C.n_cols-1-i].ind;
      // Change symmetry of orbital with maximum overlap
      gp[maxind]=igp+1;

      printf("Set symmetry of orbital %i to %i.\n",(int) maxind+1,gp[maxind]);
    }
    
  }
  
  return gp;
}

void freeze_orbs(const std::vector< std::vector<arma::vec> > & freeze, const arma::mat & C, const arma::mat & S, arma::mat & H) {
  // Freezes the orbitals corresponding to different symmetry groups.

  // Form H_MO
  arma::mat H_MO=arma::trans(C)*H*C;

  // Get symmetry groups
  std::vector<int> sg=symgroups(C,S,freeze);
  
  // Loop over H_MO and zero out elements where symmetry groups differ
  for(size_t i=0;i<H_MO.n_rows;i++)
    for(size_t j=0;j<=i;j++)
      if(sg[i]!=sg[j]) {
	H_MO(i,j)=0;
	H_MO(j,i)=0;
      }
  
  // Back-transform to AO
  arma::mat SC=S*C;

  H=SC*H_MO*arma::trans(SC);
}
