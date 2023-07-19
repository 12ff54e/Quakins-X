#pragma once

#include <thrust/device_vector.h>
#include <thrust/execution_policy.h>

#include <fstream>
#include <iostream>

#include <cufft.h>

#include "../ReorderCopy.hpp"

namespace quakins {

namespace details {

template <typename idx_type,
          typename val_type,
          idx_type dim>
struct EfieldSolver {

  const std::array<idx_type,dim> n,n_bd;
  const std::array<val_type,dim> interval;
  idx_type ntot;

  EfieldSolver(std::array<idx_type,dim> n, 
               std::array<idx_type,dim> n_bd, 
               std::array<val_type,dim> interval) 
    : n(n), n_bd(n_bd), interval(interval){
      ntot = n[1]*n[0];
    }

  template <typename itor_type>
  void operator()(itor_type phi_begin, itor_type phi_end,
                  std::array<itor_type,dim> E_begin) {
    auto middle_begin = thrust::make_zip_iterator(thrust::make_tuple(
                                phi_begin+2-2,phi_begin+2-1,
                                phi_begin+2+1,phi_begin+2+2));

    
    using zipItor = thrust::tuple<val_type,val_type,val_type,val_type>;
    
    val_type C = 1./12./interval[0];
    thrust::transform(thrust::device, 
                      middle_begin, middle_begin+n[0]*n[1]-2, E_begin[0]+2,
                      [C]__host__ __device__(zipItor _tuple) {
                      return C*(-thrust::get<0>(_tuple) + 8.*thrust::get<1>(_tuple)
                                -8.*thrust::get<2>(_tuple) + thrust::get<3>(_tuple));
                      });
    strided_chunk_range<itor_type>
      Eleft(E_begin[0], E_begin[0]+n[0]*n[1], n[0], n_bd[0]);
    strided_chunk_range<itor_type>
      Eright(E_begin[0]+n[0]-n_bd[0], E_begin[0]+n[0]*n[1], n[0], n_bd[0]);

    thrust::fill(thrust::device,Eleft.begin(),Eleft.end(),0.);
    thrust::fill(thrust::device,Eright.begin(),Eright.end(),0.);

    ReorderCopy<idx_type,val_type,dim> reorder1(n,{1,0}), reorder2({n[1],n[0]},{1,0});

    thrust::device_vector<val_type> phi_re(ntot);
    reorder1(phi_begin,phi_begin+ntot,phi_re.begin());
    
    auto middle_begin1 = thrust::make_zip_iterator(thrust::make_tuple(
                                phi_re.begin()+2-2,phi_re.begin()+2-1,
                                phi_re.begin()+2+1,phi_re.begin()+2+2));
 
    val_type C1 = 1./12./interval[1];
    thrust::transform(thrust::device, 
                      middle_begin1, middle_begin1+ntot-2, E_begin[1]+2,
                      [C1]__host__ __device__(zipItor _tuple) {
                      return C1*(-thrust::get<0>(_tuple) + 8.*thrust::get<1>(_tuple)
                                -8.*thrust::get<2>(_tuple) + thrust::get<3>(_tuple));
                      });
    strided_chunk_range<itor_type>
      Eleft1(E_begin[1], E_begin[1]+ntot, n[1], n_bd[1]);
    strided_chunk_range<itor_type>
      Eright1(E_begin[1]+n[1]-n_bd[1], E_begin[1]+ntot, n[1], n_bd[1]);

    thrust::fill(thrust::device,Eleft1.begin(),Eleft1.end(),0.);
    thrust::fill(thrust::device,Eright1.begin(),Eright1.end(),0.);


  }

};

template <typename idx_type,
          typename val_type,
          idx_type dim,
          idx_type xdim, idx_type vdim>
class FourierSpectrumVeloSpace {

  cufftHandle plan_fwd, plan_bwd;

  const idx_type nx1,nx2, nx2loc, nv1, nv2, nx1bd, nx2bd;
  const val_type dv1, dv2, dx1, dx2;

  val_type dl1, dl2, dt;

  thrust::device_vector<val_type> Ex, Ey, lam1,lam2;

public:
  template <typename Parameters>
  FourierSpectrumVeloSpace(Parameters *p, val_type dt) :
  nv1(p->n[0]), nv2(p->n[1]),
  nx1(p->n_all[2]), nx2(p->n_all[3]), nx2loc(p->n_all[3]/p->n_dev),
  dx1(p->interval[2]), dx2(p->interval[3]), dt(dt),
  dv1(p->interval[0]), dv2(p->interval[1]), 
  nx1bd(p->n_ghost[2]), nx2bd(p->n_ghost[3]) {

    dl1 = 2.*M_PI/(nv1-2)/p->interval[0]; 
    dl2 = 2.*M_PI/(nv2)/p->interval[1]; 
    
    Ex.resize((nx1-2*nx1bd)*(nx2-4*nx2bd));    
    Ey.resize((nx1-2*nx1bd)*(nx2-4*nx2bd));

    lam1.resize(nv1*nv2/2); lam2.resize(nv1*nv2/2);

    for (int j=0; j<=nv2/2; j++) {
      for (int i=0; i<nv1/2; i++) {
        lam1[j*nv1/2+i] = i*dl1;
        lam2[j*nv1/2+i] = j*dl2;
      }
    }
    for (int j=nv2/2+1; j<nv2; j++) {
      for (int i=0; i<nv1/2; i++) {
        lam1[j*nv1/2+i] = i*dl1;
        lam2[j*nv1/2+i] = (j-static_cast<int>(nv2))*dl2;
      }
    }

  }

  template <typename itor_type, typename vitor_type>
  void advance(itor_type itor_begin, itor_type itor_end, 
               vitor_type v_begin, int gpu) {

    EfieldSolver<idx_type,val_type,2> solveEfield({nx1-2*nx1bd,nx2-4*nx2bd},{2,2},{dx1,dx2});
    solveEfield(v_begin,v_begin+(nx1-2*nx1bd)*(nx2-4*nx2bd),
                std::array<itor_type,2>{Ex.begin(),Ey.begin()});
    
    std::ofstream Eout("testE",std::ios::out);
    thrust::copy(Ex.begin(),Ex.end(),std::ostream_iterator<val_type>(Eout," "));
    Eout << std::endl;
    thrust::copy(Ey.begin(),Ey.end(),std::ostream_iterator<val_type>(Eout," "));
    Eout << std::endl;


    auto comp_ptr = reinterpret_cast<cufftComplex*>(
                    thrust::raw_pointer_cast(&(*itor_begin)));

    val_type time_step = this->dt;

    for (int i=0; i<nx1*nx2loc; i++) {

      thrust::transform(thrust::device,
                        comp_ptr + i*nv1*nv2/2,
                        comp_ptr + (i+1)*nv1*nv2/2,
                        lam1.begin(),
                        comp_ptr + i*nv1*nv2/2,
                        [time_step]__host__ __device__
                        (cufftComplex val, const val_type& lam) {
                          cufftComplex next_val;
                          val_type phase = .4*time_step*lam;
                          next_val.x =  val.x*cos(phase) - val.y*sin(phase);
                          next_val.y =  val.x*sin(phase) + val.y*cos(phase);
                          return next_val;
                        });

      thrust::transform(thrust::device,
                        comp_ptr + i*nv1*nv2/2,
                        comp_ptr + (i+1)*nv1*nv2/2,
                        lam2.begin(),
                        comp_ptr + i*nv1*nv2/2,
                        [time_step]__host__ __device__
                        (cufftComplex val, const val_type& lam) {
                          cufftComplex next_val;
                          val_type phase = .4*time_step*lam;
                          next_val.x =  val.x*cos(phase) - val.y*sin(phase);
                          next_val.y =  val.x*sin(phase) + val.y*cos(phase);
                          return next_val;
                        });

    }
   
  }

};


} // namespace details
  
} // namespace quakins
