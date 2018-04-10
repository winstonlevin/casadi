 /*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "conic_activeset.hpp"
#include "casadi/core/nlpsol.hpp"

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_CONIC_ACTIVESET_EXPORT
  casadi_register_conic_activeset(Conic::Plugin* plugin) {
    plugin->creator = ConicActiveSet::creator;
    plugin->name = "activeset";
    plugin->doc = ConicActiveSet::meta_doc.c_str();
    plugin->version = CASADI_VERSION;
    plugin->options = &ConicActiveSet::options_;
    return 0;
  }

  extern "C"
  void CASADI_CONIC_ACTIVESET_EXPORT casadi_load_conic_activeset() {
    Conic::registerPlugin(casadi_register_conic_activeset);
  }

  ConicActiveSet::ConicActiveSet(const std::string& name, const std::map<std::string, Sparsity> &st)
    : Conic(name, st) {
  }

  ConicActiveSet::~ConicActiveSet() {
    clear_mem();
  }

  Options ConicActiveSet::options_
  = {{&Conic::options_},
     {{"max_iter",
       {OT_INT,
        "Maximum number of iterations [1000]."}},
      {"tol",
       {OT_DOUBLE,
        "Tolerance [1e-8]."}},
      {"du_to_pr",
       {OT_DOUBLE,
        "How much larger dual than primal error is acceptable [1000]"}},
      {"print_header",
       {OT_BOOL,
        "Print header [true]."}},
      {"print_iter",
       {OT_BOOL,
        "Print iterations [true]."}}
     }
  };

  void ConicActiveSet::init(const Dict& opts) {
    // Initialize the base classes
    Conic::init(opts);

    // Default options
    max_iter_ = 1000;
    tol_ = 1e-8;
    print_iter_ = true;
    print_header_ = true;
    du_to_pr_ = 1000.;

    // Read user options
    for (auto&& op : opts) {
      if (op.first=="max_iter") {
        max_iter_ = op.second;
      } else if (op.first=="tol") {
        tol_ = op.second;
      } else if (op.first=="print_iter") {
        print_iter_ = op.second;
      } else if (op.first=="print_header") {
        print_header_ = op.second;
      } else if (op.first=="du_to_pr") {
        du_to_pr_ = op.second;
      }
    }

    // Transpose of the Jacobian
    AT_ = A_.T();

    // Assemble KKT system sparsity
    kkt_ = Sparsity::kkt(H_, A_, true, true);

    // Symbolic QR factorization
    kkt_.qr_sparse(sp_v_, sp_r_, prinv_, pc_);

    // Allocate memory
    alloc_w(kkt_.nnz(), true); // kkt
    alloc_w(nx_+na_, true); // z=[xk,gk]
    alloc_w(nx_+na_, true); // lbz
    alloc_w(nx_+na_, true); // ubz
    alloc_w(nx_+na_, true); // lam
    alloc_w(AT_.nnz(), true); // trans_a
    alloc_iw(nx_+na_); // casadi_trans, tau type
    alloc_w(nx_+na_); // casadi_project, tau memory
    alloc_w(nx_+na_, true); // dz
    alloc_w(nx_+na_, true); // dlam
    alloc_w(nx_, true); // infeas
    alloc_w(nx_, true); // tinfeas
    alloc_iw(nx_+na_, true); // neverzero
    alloc_iw(nx_+na_, true); // neverupper
    alloc_iw(nx_+na_, true); // neverlower
    alloc_iw(nx_+na_); // allzero

    // Memory for numerical solution
    alloc_w(max(sp_v_.nnz()+sp_r_.nnz(), kkt_.nnz()), true); // either v & r or trans(kkt)
    alloc_w(nx_+na_, true); // beta
    alloc_w(2*na_+2*nx_); // casadi_qr

    if (print_header_) {
      // Print summary
      print("-------------------------------------------\n");
      print("This is casadi::ConicActiveSet.\n");
      print("Number of variables:                       %9d\n", nx_);
      print("Number of constraints:                     %9d\n", na_);
      print("Work in progress!\n");
    }
  }

  template<typename T1>
  void casadi_set_sub(const T1* y, T1* x, const casadi_int* sp_x,
                      casadi_int rbeg, casadi_int rend,
                      casadi_int cbeg, casadi_int cend) {
    // Local variables
    casadi_int r, c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2, *row=sp_x+2+ncol+1;
    // Set elements in subblock
    for (c=cbeg; c<cend; ++c) {
      for (k=colind[c]; k<colind[c+1] && (r=row[k])<rend; ++k) {
        if (r>=rbeg) x[k] = *y++;
      }
    }
  }

  template<typename T1>
  void casadi_fill_sub(T1 y, T1* x, const casadi_int* sp_x,
                      casadi_int rbeg, casadi_int rend,
                      casadi_int cbeg, casadi_int cend) {
    // Local variables
    casadi_int r, c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2, *row=sp_x+2+ncol+1;
    // Set elements in subblock
    for (c=cbeg; c<cend; ++c) {
      for (k=colind[c]; k<colind[c+1] && (r=row[k])<rend; ++k) {
        if (r>=rbeg) x[k] = y;
      }
    }
  }

  template<typename T1>
  void casadi_row_scal(T1* x, const casadi_int* sp_x, const T1* d) {
    // Local variables
    casadi_int c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2, *row=sp_x+2+ncol+1;
    // Scale entries
    for (c=0; c<ncol; ++c) {
      for (k=colind[c]; k<colind[c+1]; ++k) {
        x[k] *= d[row[k]];
      }
    }
  }

  void ConicActiveSet::
  print_vector(const char* id, const double* x, casadi_int n) const {
    print("%s: [", id);
    for (casadi_int i=0; i<n; ++i) {
      if (i!=0) print(", ");
      print("%g", x[i]);
    }
    print("]\n");
  }

  void ConicActiveSet::
  print_ivector(const char* id, const casadi_int* x, casadi_int n) const {
    print("%s: [", id);
    for (casadi_int i=0; i<n; ++i) {
      if (i!=0) print(", ");
      print("%lld", x[i]);
    }
    print("]\n");
  }

  template<typename T1>
  void casadi_col_scal(T1* x, const casadi_int* sp_x, const T1* d) {
    // Local variables
    casadi_int c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2;
    // Scale entries
    for (c=0; c<ncol; ++c) {
      for (k=colind[c]; k<colind[c+1]; ++k) {
        x[k] *= d[c];
      }
    }
  }

  template<typename T1>
  void casadi_add_diag(T1* x, const casadi_int* sp_x, const T1* d) {
    // Local variables
    casadi_int c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2, *row=sp_x+2+ncol+1;
    // Add to diagonal entry
    for (c=0; c<ncol; ++c) {
      for (k=colind[c]; k<colind[c+1]; ++k) {
        if (row[k]==c) {
          x[k] += d[c];
          break;
        }
      }
    }
  }

  template<typename T1>
  struct casadi_qp_mem {
    // Cost
    T1 f;
    // Dimensions
    casadi_int nx, na, nz;
    // Sparsity patterns
    const casadi_int *sp_a, *sp_h, *sp_at, *sp_kkt;
    // QP data
    const T1 *nz_a, *nz_h, *g;
    // Vectors
    T1 *z, *lbz, *ubz, *infeas, *tinfeas, *lam, *w, *dz, *dlam;
    casadi_int *iw, *neverzero, *neverlower, *neverupper;
    // QR factorization
    const casadi_int *prinv, *pc, *sp_v, *sp_r;
    T1 *nz_at, *nz_kkt, *beta, *nz_v, *nz_r;
    // Smallest nonzero number
    T1 dmin;
    // Infinity
    T1 inf;
    // Dual to primal error
    T1 du_to_pr;
    // Message buffer
    char msg[40];
    // Print iterations
    int print_iter;
    // Stepsize
    T1 tau;
    // Singularity
    casadi_int sing;
    // Smallest diagonal value for the QR factorization
    T1 mina;
    casadi_int imina;
    // Primal and dual error, corresponding index
    T1 pr, du;
    casadi_int ipr, idu;
  };

  template<typename T1>
  void casadi_qp_pr(casadi_qp_mem<T1>* m) {
    // Calculate largest constraint violation
    casadi_int i;
    m->pr = 0;
    m->ipr = -1;
    for (i=0; i<m->nz; ++i) {
      if (m->z[i] > m->ubz[i]+m->pr) {
        m->pr = m->z[i]-m->ubz[i];
        m->ipr = i;
      } else if (m->z[i] < m->lbz[i]-m->pr) {
        m->pr = m->lbz[i]-m->z[i];
        m->ipr = i;
      }
    }
  }

  template<typename T1>
  void casadi_qp_du(casadi_qp_mem<T1>* m) {
    // Calculate largest constraint violation
    casadi_int i;
    m->du = 0;
    m->idu = -1;
    for (i=0; i<m->nx; ++i) {
      if (m->infeas[i] > m->du) {
        m->du = m->infeas[i];
        m->idu = i;
      } else if (m->infeas[i] < -m->du) {
        m->du = -m->infeas[i];
        m->idu = i;
      }
    }
  }

  template<typename T1>
  void casadi_qp_log(casadi_qp_mem<T1>* m, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(m->msg, sizeof(m->msg), fmt, args);
    va_end(args);
  }

  template<typename T1>
  casadi_int casadi_qp_pr_index(casadi_qp_mem<T1>* m, casadi_int* sign) {
    // Try to improve primal feasibility by adding a constraint
    if (m->lam[m->ipr]==0.) {
      // Add the most violating constraint
      *sign = m->z[m->ipr]<m->lbz[m->ipr] ? -1 : 1;
      casadi_qp_log(m, "Added %lld to reduce |pr|", m->ipr);
      return m->ipr;
    }
    return -1;
  }

  template<typename T1>
  T1 casadi_qp_du_check(casadi_qp_mem<T1>* m, casadi_int i) {
    // Local variables
    casadi_int k;
    T1 new_du;
    const casadi_int *at_colind, *at_row;
    // AT sparsity
    at_colind = m->sp_at + 2;
    at_row = at_colind + m->na + 1;
    // Maximum infeasibility from setting from setting lam[i]=0
    if (i<m->nx) {
      new_du = fabs(m->infeas[i]-m->lam[i]);
    } else {
      new_du = 0.;
      for (k=at_colind[i-m->nx]; k<at_colind[i-m->nx+1]; ++k) {
        new_du = fmax(new_du, fabs(m->infeas[at_row[k]]-m->nz_at[k]*m->lam[i]));
      }
    }
    return new_du;
  }

  template<typename T1>
  casadi_int casadi_qp_du_index(casadi_qp_mem<T1>* m, casadi_int* sign) {
    // Try to improve dual feasibility by removing a constraint
    // Local variables
    casadi_int best_ind, i;
    T1 best_w;
    // We need to increase or decrease infeas[idu]. Sensitivity:
    casadi_fill(m->w, m->nz, 0.);
    m->w[m->idu] = m->infeas[m->idu]>0 ? -1. : 1.;
    casadi_mv(m->nz_a, m->sp_a, m->w, m->w+m->nx, 0);
    // Find the best lam[i] to make zero
    best_ind = -1;
    best_w = 0.;
    for (i=0; i<m->nz; ++i) {
      // Make sure variable influences du
      if (m->w[i]==0.) continue;
      // Make sure removing the constraint decreases dual infeasibility
      if (m->w[i]>0. ? m->lam[i]>=0. : m->lam[i]<=0.) continue;
      // Skip if maximum infeasibility increases
      if (casadi_qp_du_check(m, i)>m->du) continue;
      // Check if best so far
      if (fabs(m->w[i])>best_w) {
        best_w = fabs(m->w[i]);
        best_ind = i;
      }
    }
    // Accept, if any
    if (best_ind>=0) {
      *sign = 0;
      casadi_qp_log(m, "Removed %lld to reduce |du|", best_ind);
      return best_ind;
    } else {
      return -1;
    }
  }

  template<typename T1>
  void casadi_qp_kkt(casadi_qp_mem<T1>* m) {
    // Local variables
    casadi_int i, k;
    const casadi_int *h_colind, *h_row, *a_colind, *a_row, *at_colind, *at_row,
                     *kkt_colind, *kkt_row;
    // Extract sparsities
    a_row = (a_colind = m->sp_a+2) + m->nx + 1;
    at_row = (at_colind = m->sp_at+2) + m->na + 1;
    h_row = (h_colind = m->sp_h+2) + m->nx + 1;
    kkt_row = (kkt_colind = m->sp_kkt+2) + m->nz + 1;
    // Reset w to zero
    casadi_fill(m->w, m->nz, 0.);
    // Loop over rows of the (transposed) KKT
    for (i=0; i<m->nz; ++i) {
      // Copy row of KKT to w
      if (i<m->nx) {
        if (m->lam[i]==0) {
          for (k=h_colind[i]; k<h_colind[i+1]; ++k) m->w[h_row[k]] = m->nz_h[k];
          for (k=a_colind[i]; k<a_colind[i+1]; ++k) m->w[m->nx+a_row[k]] = m->nz_a[k];
        } else {
          m->w[i] = 1.;
        }
      } else {
        if (m->lam[i]==0) {
          m->w[i] = -1.;
        } else {
          for (k=at_colind[i-m->nx]; k<at_colind[i-m->nx+1]; ++k) {
            m->w[at_row[k]] = m->nz_at[k];
          }
        }
      }
      // Copy row to KKT, zero out w
      for (k=kkt_colind[i]; k<kkt_colind[i+1]; ++k) {
        m->nz_kkt[k] = m->w[kkt_row[k]];
        m->w[kkt_row[k]] = 0;
      }
    }
  }

  template<typename T1>
  void casadi_qp_kkt_column(casadi_qp_mem<T1>* m, T1* kkt_i, casadi_int i,
                            casadi_int sign) {
    // Local variables
    casadi_int k;
    const casadi_int *h_colind, *h_row, *a_colind, *a_row, *at_colind, *at_row;
    // Extract sparsities
    a_row = (a_colind = m->sp_a+2) + m->nx + 1;
    at_row = (at_colind = m->sp_at+2) + m->na + 1;
    h_row = (h_colind = m->sp_h+2) + m->nx + 1;
    // Reset kkt_i to zero
    casadi_fill(kkt_i, m->nz, 0.);
    // Copy column of KKT to kkt_i
    if (i<m->nx) {
      if (sign==0) {
        for (k=h_colind[i]; k<h_colind[i+1]; ++k) kkt_i[h_row[k]] = m->nz_h[k];
        for (k=a_colind[i]; k<a_colind[i+1]; ++k) kkt_i[m->nx+a_row[k]] = m->nz_a[k];
      } else {
        kkt_i[i] = 1.;
      }
    } else {
      if (sign==0) {
        kkt_i[i] = -1.;
      } else {
        for (k=at_colind[i-m->nx]; k<at_colind[i-m->nx+1]; ++k) {
          kkt_i[at_row[k]] = m->nz_at[k];
        }
      }
    }
  }

  template<typename T1>
  T1 casadi_qp_kkt_dot(casadi_qp_mem<T1>* m, const T1* v, casadi_int i, casadi_int sign) {
    // Local variables
    casadi_int k;
    const casadi_int *h_colind, *h_row, *a_colind, *a_row, *at_colind, *at_row;
    T1 d;
    // Extract sparsities
    a_row = (a_colind = m->sp_a+2) + m->nx + 1;
    at_row = (at_colind = m->sp_at+2) + m->na + 1;
    h_row = (h_colind = m->sp_h+2) + m->nx + 1;
    // Scalar product with the desired column
    if (i<m->nx) {
      if (sign==0) {
        d = 0.;
        for (k=h_colind[i]; k<h_colind[i+1]; ++k) d += v[h_row[k]] * m->nz_h[k];
        for (k=a_colind[i]; k<a_colind[i+1]; ++k) d += v[m->nx+a_row[k]] * m->nz_a[k];
      } else {
        d = v[i];
      }
    } else {
      if (sign==0) {
        d = -v[i];
      } else {
        d = 0.;
        for (k=at_colind[i-m->nx]; k<at_colind[i-m->nx+1]; ++k) {
          d += v[at_row[k]] * m->nz_at[k];
        }
      }
    }
    return d;
  }

  template<typename T1>
  void casadi_qp_kkt_residual(casadi_qp_mem<T1>* m, T1* r) {
    casadi_int i;
    for (i=0; i<m->nz; ++i) {
      if (m->lam[i]>0.) {
        r[i] = m->ubz[i]-m->z[i];
      } else if (m->lam[i]<0.) {
        r[i] = m->lbz[i]-m->z[i];
      } else if (i<m->nx) {
        r[i] = m->lam[i]-m->infeas[i];
      } else {
        r[i] = m->lam[i];
      }
    }
  }

  template<typename T1>
  int casadi_qp_zero_blocking(casadi_qp_mem<T1>* m, T1 e,
                              casadi_int* index, casadi_int* sign) {
    // Local variables
    T1 dz_max;
    casadi_int i;
    int ret = 0;
    dz_max = 0.;
    for (i=0; i<m->nz; ++i) {
      if (-m->dz[i]>dz_max && m->z[i]<=m->lbz[i]-e) {
        ret = 1;
        if (index) *index = i;
        if (sign) *sign = -1;
        casadi_qp_log(m, "lbz[%lld] violated at 0", i);
      } else if (m->dz[i]>dz_max && m->z[i]>=m->ubz[i]+e) {
        ret = 1;
        if (index) *index = i;
        if (sign) *sign = 1;
        casadi_qp_log(m, "ubz[%lld] violated at 0", i);
      }
    }
    return ret;
  }

  template<typename T1>
  void casadi_qp_primal_blocking(casadi_qp_mem<T1>* m, T1 e,
                                 casadi_int* index, casadi_int* sign) {
    // Local variables
    casadi_int i;
    T1 trial_z;
    // Check if violation with tau=0 and not improving
    if (casadi_qp_zero_blocking(m, e, index, sign)) {
      m->tau = 0.;
      return;
    }
    // Loop over all primal variables
    for (i=0; i<m->nz; ++i) {
      if (m->dz[i]==0.) continue; // Skip zero steps
      // Trial primal step
      trial_z=m->z[i] + m->tau*m->dz[i];
      if (m->dz[i]<0 && trial_z<m->lbz[i]-e) {
        // Trial would increase maximum infeasibility
        m->tau = (m->lbz[i]-e-m->z[i])/m->dz[i];
        if (index) *index = m->lam[i]<0. ? -1 : i;
        if (sign) *sign = -1;
        casadi_qp_log(m, "Enforcing lbz[%lld]", i);
      } else if (m->dz[i]>0 && trial_z>m->ubz[i]+e) {
        // Trial would increase maximum infeasibility
        m->tau = (m->ubz[i]+e-m->z[i])/m->dz[i];
        if (index) *index = m->lam[i]>0. ? -1 : i;
        if (sign) *sign = 1;
        casadi_qp_log(m, "Enforcing ubz[%lld]", i);
      }
      if (m->tau<=0) return;
    }
  }

  template<typename T1>
  casadi_int casadi_qp_dual_breakpoints(casadi_qp_mem<T1>* m, T1* tau_list,
                                        casadi_int* ind_list, T1 e, T1 tau) {
    // Local variables
    casadi_int i, n_tau, loc, next_ind, tmp_ind, j;
    T1 trial_lam, new_tau, next_tau, tmp_tau;
    // Dual feasibility is piecewise linear. Start with one interval [0,tau]:
    tau_list[0] = tau;
    ind_list[0] = -1; // no associated index
    n_tau = 1;
    // Find the taus corresponding to lam crossing zero and insert into list
    for (i=0; i<m->nz; ++i) {
      if (m->dlam[i]==0.) continue; // Skip zero steps
      if (m->lam[i]==0.) continue; // Skip inactive constraints
      // Trial dual step
      trial_lam = m->lam[i] + tau*m->dlam[i];
      // Skip if no sign change
      if (m->lam[i]>0 ? trial_lam>=0 : trial_lam<=0) continue;
      // Location of the sign change
      new_tau = -m->lam[i]/m->dlam[i];
      // Where to insert the w[i]
      for (loc=0; loc<n_tau-1; ++loc) {
        if (new_tau<tau_list[loc]) break;
      }
      // Insert element
      n_tau++;
      next_tau=new_tau;
      next_ind=i;
      for (j=loc; j<n_tau; ++j) {
        tmp_tau = tau_list[j];
        tau_list[j] = next_tau;
        next_tau = tmp_tau;
        tmp_ind = ind_list[j];
        ind_list[j] = next_ind;
        next_ind = tmp_ind;
      }
    }
    return n_tau;
  }

  template<typename T1>
  casadi_int casadi_qp_dual_blocking(casadi_qp_mem<T1>* m, T1 e) {
    // Local variables
    casadi_int i, n_tau, j, k, du_index;
    T1 tau_k, dtau, new_infeas, tau1;
    const casadi_int *at_colind, *at_row;
    // Extract sparsities
    at_row = (at_colind = m->sp_at+2) + m->na + 1;
    // Dual feasibility is piecewise linear in tau. Get the intervals:
    n_tau = casadi_qp_dual_breakpoints(m, m->w, m->iw, e, m->tau);
    // No dual blocking yet
    du_index = -1;
    // How long step can we take without exceeding e?
    tau_k = 0.;
    for (j=0; j<n_tau; ++j) {
      // Distance to the next tau (may be zero)
      dtau = m->w[j] - tau_k;
      // Check if maximum dual infeasibilty gets exceeded
      for (k=0; k<m->nx; ++k) {
        new_infeas = m->infeas[k]+dtau*m->tinfeas[k];
        if (fabs(new_infeas)>e) {
          tau1 = fmax(0., tau_k + ((new_infeas>0 ? e : -e)-m->infeas[k])/m->tinfeas[k]);
          if (tau1 < m->tau) {
            // Smallest tau found so far
            m->tau = tau1;
            du_index = k;
          }
        }
      }
      // Update infeasibility
      casadi_axpy(m->nx, fmin(m->tau - tau_k, dtau), m->tinfeas, m->infeas);
      // Stop here if dual blocking constraint
      if (du_index>=0) return du_index;
      // Continue to the next tau
      tau_k = m->w[j];
      // Get component, break if last
      i = m->iw[j];
      if (i<0) break;
      // Update sign or tinfeas
      if (!m->neverzero[i]) {
        // lam becomes zero, update the infeasibility tangent
        if (i<m->nx) {
          // Set a lam_x to zero
          m->tinfeas[i] -= m->dlam[i];
        } else {
          // Set a lam_a to zero
          for (k=at_colind[i-m->nx]; k<at_colind[i-m->nx+1]; ++k) {
            m->tinfeas[at_row[k]] -= m->nz_at[k]*m->dlam[i];
          }
        }
      }
    }
    return du_index;
  }

  template<typename T1>
  void casadi_qp_take_step(casadi_qp_mem<T1>* m) {
    // Local variables
    casadi_int i;
    // Get current sign
    for (i=0; i<m->nz; ++i) m->iw[i] = m->lam[i]>0. ? 1 : m->lam[i]<0 ? -1 : 0;
    // Take primal-dual step
    casadi_axpy(m->nz, m->tau, m->dz, m->z);
    casadi_axpy(m->nz, m->tau, m->dlam, m->lam);
    // Update sign
    for (i=0; i<m->nz; ++i) {
      // Allow sign changes for certain components
      if (m->neverzero[i] && (m->iw[i]<0 ? m->lam[i]>0 : m->lam[i]<0)) {
        m->iw[i]=-m->iw[i];
      }
      // Ensure correct sign
      switch (m->iw[i]) {
        case -1: m->lam[i] = fmin(m->lam[i], -m->dmin); break;
        case  1: m->lam[i] = fmax(m->lam[i],  m->dmin); break;
        case  0: m->lam[i] = 0.; break;
      }
    }
  }

  template<typename T1>
  int casadi_qp_flip_check(casadi_qp_mem<T1>* m, casadi_int index, casadi_int sign,
                           casadi_int* r_index, casadi_int* r_sign, T1 e) {
    // Local variables
    casadi_int new_sign, i;
    T1 best_slack, new_slack;
    // New column that we're trying to add
    casadi_qp_kkt_column(m, m->dz, index, sign);
    // Express it using the other columns
    casadi_qr_solve(m->dz, 1, 0, m->sp_v, m->nz_v, m->sp_r, m->nz_r, m->beta,
                    m->prinv, m->pc, m->w);
    // Quick return if columns are linearly independent
    if (fabs(m->dz[index])>=1e-12) return 0;
    // Column that we're removing
    casadi_qp_kkt_column(m, m->w, index, !sign);
    // Find best constraint we can flip, if any
    *r_index=-1;
    *r_sign=0;
    best_slack = -inf;
    for (i=0; i<m->nz; ++i) {
      // Can't be the same
      if (i==index) continue;
      // Make sure constraint is flippable
      if (m->lam[i]==0 ? m->neverlower[i] && m->neverupper[i] : m->neverzero[i]) continue;
      // If dz[i]!=0, column i is redundant
      if (fabs(m->dz[i])<1e-12) continue;
      // We want to make sure that the new, flipped column i is linearly
      // independent with other columns. We have:
      // flipped_column[index] = dz[0]*column[0] + ... + dz[N-1]*column[N-1]
      // We also require that flipped_column[i] isn't othogonal to
      // (old) column[index], as this will surely lead to singularity
      // This will not cover all cases of singularity, but many important
      // ones. General singularity handling is done below.
      if (fabs(casadi_qp_kkt_dot(m, m->w, i, m->lam[i]==0.)) < 1e-12) continue;
      // Dual infeasibility
      // Check if the best so far
      if (m->lam[i]==0.) {
        // Which bound is closer?
        new_sign = m->lbz[i]-m->z[i] >= m->z[i]-m->ubz[i] ? -1 : 1;
        // Better than negative slack, worse than positive slack
        new_slack = 0;
      } else {
        // Skip if flipping would result in too large |du|
        if (casadi_qp_du_check(m, i)>e) continue;
        // Slack to the bound
        new_slack = m->lam[i]>0 ? m->ubz[i]-m->z[i] : m->z[i]-m->lbz[i];
        new_sign = 0;
      }
      // Best so far?
      if (new_slack > best_slack) {
        best_slack = new_slack;
        *r_index = i;
        *r_sign = new_sign;
      }
    }
    // Accept, if any
    return *r_index>=0 ? 0 : 1;
  }

  template<typename T1>
  void casadi_qp_factorize(casadi_qp_mem<T1>* m) {
    // Construct the KKT matrix
    casadi_qp_kkt(m);
    // QR factorization
    casadi_qr(m->sp_kkt, m->nz_kkt, m->w, m->sp_v, m->nz_v, m->sp_r,
              m->nz_r, m->beta, m->prinv, m->pc);
    // Check singularity
    m->sing = casadi_qr_singular(&m->mina, &m->imina, m->nz_r, m->sp_r, m->pc, 1e-12);
  }

  template<typename T1>
  int casadi_qp_scale_step(casadi_qp_mem<T1>* m, casadi_int* r_index, casadi_int* r_sign) {
    // Local variables
    T1 tpr, tdu, terr, tau_test, minat_tr, tau;
    int pos_ok, neg_ok;
    casadi_int nnz_kkt, nullity_tr, nulli, imina_tr, i;
    // Reset r_index, r_sign, quick return if non-singular
    *r_index = -1;
    *r_sign = 0;
    if (!m->sing) {
      return 0;
    }
    // Change in pr, du in the search direction
    tpr = m->ipr<0 ? 0.
                   : m->z[m->ipr]>m->ubz[m->ipr] ? m->dz[m->ipr]/m->pr
                                                 : -m->dz[m->ipr]/m->pr;
    tdu = m->idu<0 ? 0. : m->tinfeas[m->idu]/m->infeas[m->idu];
    // Change in max(pr, du) in the search direction
    pos_ok=1, neg_ok=1;
    if (m->pr>m->du) {
      // |pr|>|du|
      if (tpr<0) {
        neg_ok = 0;
      } else if (tpr>0) {
        pos_ok = 0;
      }
      terr = tpr;
    } else if (m->pr<m->du) {
      // |pr|<|du|
      if (tdu<0) {
        neg_ok = 0;
      } else if (tdu>0) {
        pos_ok = 0;
      }
      terr = tdu;
    } else {
      // |pr|==|du|
      if ((tpr>0 && tdu<0) || (tpr<0 && tdu>0)) {
        // |pr|==|du| cannot be decreased along the search direction
        pos_ok = neg_ok = 0;
        terr = 0;
      } else if (fmin(tpr, tdu)<0) {
        // |pr|==|du| decreases for positive tau
        neg_ok = 0;
        terr = fmax(tpr, tdu);
      } else if (fmax(tpr, tdu)>0) {
        // |pr|==|du| decreases for negative tau
        pos_ok = 0;
        terr = fmin(tpr, tdu);
      } else {
        terr = 0;
      }
    }
    // If primal error is dominating and constraint is active,
    // then only allow the multiplier to become larger
    if (m->du_to_pr*m->pr>=m->du && m->lam[m->ipr]!=0 && fabs(m->dlam[m->ipr])>1e-12) {
      if ((m->lam[m->ipr]>0)==(m->dlam[m->ipr]>0)) {
        neg_ok = 0;
      } else {
        pos_ok = 0;
      }
    }
    // QR factorization of the transpose
    casadi_trans(m->nz_kkt, m->sp_kkt, m->nz_v, m->sp_kkt, m->iw);
    nnz_kkt = m->sp_kkt[2+m->nz]; // kkt_colind[nz]
    casadi_copy(m->nz_v, nnz_kkt, m->nz_kkt);
    casadi_qr(m->sp_kkt, m->nz_kkt, m->w, m->sp_v, m->nz_v, m->sp_r, m->nz_r,
              m->beta, m->prinv, m->pc);
    // Best flip
    tau = m->inf;
    // For all nullspace vectors
    nullity_tr = casadi_qr_singular(&minat_tr, &imina_tr, m->nz_r, m->sp_r,
                                    m->pc, 1e-12);
    for (nulli=0; nulli<nullity_tr; ++nulli) {
      // Get a linear combination of the rows in kkt
      casadi_qr_colcomb(m->w, m->nz_r, m->sp_r, m->pc, imina_tr, nulli);
      // Look for the best constraint for increasing rank
      for (i=0; i<m->nz; ++i) {
        // Check if old column can be removed without decreasing rank
        if (fabs(i<m->nx ? m->dz[i] : m->dlam[i])<1e-12) continue;
        // If dot(w, kkt(i)-kkt_flipped(i))==0, rank won't increase
        if (fabs(casadi_qp_kkt_dot(m, m->w, i, 0)
                 - casadi_qp_kkt_dot(m, m->w, i, 1))<1e-12) continue;
        // Is constraint active?
        if (m->lam[i]==0.) {
          // Make sure that step is nonzero
          if (fabs(m->dz[i])<1e-12) continue;
          // Step needed to bring z to lower bound
          if (!m->neverlower[i]) {
            tau_test = (m->lbz[i]-m->z[i])/m->dz[i];
            // Ensure nonincrease in max(pr, du)
            if (!((terr>0. && tau_test>0.) || (terr<0. && tau_test<0.))) {
              // Only allow removing constraints if tau_test==0
              if (fabs(tau_test)>=1e-16) {
                // Check if best so far
                if (fabs(tau_test)<fabs(tau)) {
                  tau = tau_test;
                  *r_index = i;
                  *r_sign = -1;
                  casadi_qp_log(m, "Enforced lbz[%lld] for regularity", i);
                }
              }
            }
          }
          // Step needed to bring z to upper bound
          if (!m->neverupper[i]) {
            tau_test = (m->ubz[i]-m->z[i])/m->dz[i];
            // Ensure nonincrease in max(pr, du)
            if (!((terr>0. && tau_test>0.) || (terr<0. && tau_test<0.))) {
              // Only allow removing constraints if tau_test==0
              if (fabs(tau_test)>=1e-16) {
                // Check if best so far
                if (fabs(tau_test)<fabs(tau)) {
                  tau = tau_test;
                  *r_index = i;
                  *r_sign = 1;
                  casadi_qp_log(m, "Enforced ubz[%lld] for regularity", i);
                }
              }
            }
          }
        } else {
          // Make sure that step is nonzero
          if (fabs(m->dlam[i])<1e-12) continue;
          // Step needed to bring lam to zero
          if (!m->neverzero[i]) {
            tau_test = -m->lam[i]/m->dlam[i];
            // Ensure nonincrease in max(pr, du)
            if ((terr>0. && tau_test>0.) || (terr<0. && tau_test<0.)) continue;
            // Make sure direction is permitted
            if ((tau_test>0 && !pos_ok) || (tau_test<0 && !neg_ok)) continue;
            // Check if best so far
            if (fabs(tau_test)<fabs(tau)) {
              tau = tau_test;
              *r_index = i;
              *r_sign = 0;
              casadi_qp_log(m, "Dropped %s[%lld] for regularity",
                     m->lam[i]>0 ? "lbz" : "ubz", i);
            }
          }
        }
      }
    }
    // Can we restore feasibility?
    if (*r_index<0) return 1;
    // Scale step so that that tau=1 corresponds to a full step
    casadi_scal(m->nz, tau, m->dz);
    casadi_scal(m->nz, tau, m->dlam);
    casadi_scal(m->nx, tau, m->tinfeas);
    return 0;
  }

  template<typename T1>
  int casadi_qp_calc_step(casadi_qp_mem<T1>* m, casadi_int* r_index, casadi_int* r_sign) {
    // Local variables
    casadi_int i;
    // Calculate step in z[:nx] and lam[nx:]
    if (!m->sing) {
      // Negative KKT residual
      casadi_qp_kkt_residual(m, m->dz);
      // Solve to get primal-dual step
      casadi_qr_solve(m->dz, 1, 1, m->sp_v, m->nz_v, m->sp_r, m->nz_r, m->beta,
                      m->prinv, m->pc, m->w);
    } else {
      // Get a linear combination of the columns in KKT
      casadi_qr_colcomb(m->dz, m->nz_r, m->sp_r, m->pc, m->imina, 0);
    }
    // Calculate change in Lagrangian gradient
    casadi_fill(m->dlam, m->nx, 0.);
    casadi_mv(m->nz_h, m->sp_h, m->dz, m->dlam, 0); // gradient of the objective
    casadi_mv(m->nz_a, m->sp_a, m->dz+m->nx, m->dlam, 1); // gradient of the Lagrangian
    // Step in lam[:nx]
    casadi_scal(m->nx, -1., m->dlam);
    // For inactive constraints, lam(x) step is zero
    for (i=0; i<m->nx; ++i) if (m->lam[i]==0.) m->dlam[i] = 0.;
    // Step in lam[nx:]
    casadi_copy(m->dz+m->nx, m->na, m->dlam+m->nx);
    // Step in z[nx:]
    casadi_fill(m->dz+m->nx, m->na, 0.);
    casadi_mv(m->nz_a, m->sp_a, m->dz, m->dz+m->nx, 0);
    // Avoid steps that are nonzero due to numerics
    for (i=0; i<m->nz; ++i) if (fabs(m->dz[i])<1e-14) m->dz[i] = 0.;
    // Tangent of the dual infeasibility at tau=0
    casadi_fill(m->tinfeas, m->nx, 0.);
    casadi_mv(m->nz_h, m->sp_h, m->dz, m->tinfeas, 0);
    casadi_mv(m->nz_a, m->sp_a, m->dlam+m->nx, m->tinfeas, 1);
    casadi_axpy(m->nx, 1., m->dlam, m->tinfeas);
    // Calculate step length
    return casadi_qp_scale_step(m, r_index, r_sign);
  }

  template<typename T1>
  void casadi_qp_calc_dependent(casadi_qp_mem<T1>* m) {
    // Local variables
    casadi_int i;
    // Calculate f
    m->f = casadi_bilin(m->nz_h, m->sp_h, m->z, m->z)/2.
         + casadi_dot(m->nx, m->z, m->g);
    // Calculate z[nx:]
    casadi_fill(m->z+m->nx, m->na, 0.);
    casadi_mv(m->nz_a, m->sp_a, m->z, m->z+m->nx, 0);
    // Calculate gradient of the Lagrangian
    casadi_copy(m->g, m->nx, m->infeas);
    casadi_mv(m->nz_h, m->sp_h, m->z, m->infeas, 0);
    casadi_mv(m->nz_a, m->sp_a, m->lam+m->nx, m->infeas, 1);
    // Calculate lam[:nx] without changing the sign, dual infeasibility
    for (i=0; i<m->nx; ++i) {
      if (m->lam[i]>0) {
        m->lam[i] = fmax(-m->infeas[i], m->dmin);
      } else if (m->lam[i]<0) {
        m->lam[i] = fmin(-m->infeas[i], -m->dmin);
      }
      m->infeas[i] += m->lam[i];
    }
    // Calculate primal and dual error
    casadi_qp_pr(m);
    casadi_qp_du(m);
  }

  template<typename T1>
  void casadi_qp_linesearch(casadi_qp_mem<T1>* m, casadi_int* index, casadi_int* sign) {
    // Start with a full step and no active set change
    *sign=0;
    *index=-1;
    m->tau = 1.;
    // Find largest possible step without exceeding acceptable |pr|
    casadi_qp_primal_blocking(m, fmax(m->pr, m->du/m->du_to_pr), index, sign);
    // Find largest possible step without exceeding acceptable |du|
    if (casadi_qp_dual_blocking(m, fmax(m->pr*m->du_to_pr, m->du))>=0) {
      *index = -1;
      *sign=0;
    }
    // Take primal-dual step, avoiding accidental sign changes for lam
    casadi_qp_take_step(m);
  }

  template<typename T1>
  void casadi_qp_flip(casadi_qp_mem<T1>* m, casadi_int *index, casadi_int *sign,
                                            casadi_int r_index, casadi_int r_sign) {
    // Local variables
    T1 e;
    // acceptable dual error
    e = fmax(m->du_to_pr*m->pr, m->du);
    // Try to restore regularity if possible
    if (r_index>=0 && (r_sign!=0 || casadi_qp_du_check(m, r_index)<=e)) {
      *index = r_index;
      *sign = r_sign;
      casadi_qp_log(m, "%lld->%lld for regularity", *index, *sign);
    }
    // Improve primal or dual feasibility
    if (*index==-1 && m->tau>1e-16 && (m->ipr>=0 || m->idu>=0)) {
      if (m->du_to_pr*m->pr >= m->du) {
        // Try to improve primal feasibility
        *index = casadi_qp_pr_index(m, sign);
      } else {
        // Try to improve dual feasibility
        *index = casadi_qp_du_index(m, sign);
      }
    }
    // If a constraint was added
    if (*index>=0) {
      // Try to maintain non-singularity if possible
      if (!m->sing && casadi_qp_flip_check(m, *index, *sign, &r_index, &r_sign, e)) {
        if (r_index>=0) {
          // Also flip r_index to avoid singularity
          m->lam[r_index] = r_sign==0 ? 0 : r_sign>0 ? m->dmin : -m->dmin;
          casadi_qp_log(m, "%lld->%lld, %lld->%lld", *index, *sign, r_index, r_sign);
        }
      }
      m->lam[*index] = *sign==0 ? 0 : *sign>0 ? m->dmin : -m->dmin;
      // Recalculate primal and dual infeasibility
      casadi_qp_calc_dependent(m);
      // Reset index
      *index=-2;
    }
  }


  int ConicActiveSet::init_mem(void* mem) const {
    //auto m = static_cast<ConicActiveSetMemory*>(mem);
    return 0;
  }

  int ConicActiveSet::
  eval(const double** arg, double** res, casadi_int* iw, double* w, void* mem) const {
    auto m = static_cast<ConicActiveSetMemory*>(mem);

    // Statistics
    for (auto&& s : m->fstats) s.second.reset();

    if (inputs_check_) {
      check_inputs(arg[CONIC_LBX], arg[CONIC_UBX], arg[CONIC_LBA], arg[CONIC_UBA]);
    }

    // Local variables
    int flag;
    casadi_int i;
    // Get input pointers
    const double *h, *g, *a, *lba, *uba, *lbx, *ubx, *x0, *lam_x0, *lam_a0;
    h = arg[CONIC_H];
    g = arg[CONIC_G];
    a = arg[CONIC_A];
    lba = arg[CONIC_LBA];
    uba = arg[CONIC_UBA];
    lbx = arg[CONIC_LBX];
    ubx = arg[CONIC_UBX];
    x0 = arg[CONIC_X0];
    lam_x0 = arg[CONIC_LAM_X0];
    lam_a0 = arg[CONIC_LAM_A0];

    // Get output pointers
    double *x, *f, *lam_a, *lam_x;
    x = res[CONIC_X];
    f = res[CONIC_COST];
    lam_a = res[CONIC_LAM_A];
    lam_x = res[CONIC_LAM_X];

    // Work vectors
    double *kkt, *z, *lam, *v, *r, *beta, *dz, *dlam, *lbz, *ubz,
           *trans_a, *infeas, *tinfeas, *vr;
    kkt = w; w += kkt_.nnz();
    z = w; w += nx_+na_;
    lbz = w; w += nx_+na_;
    ubz = w; w += nx_+na_;
    lam = w; w += nx_+na_;
    dz = w; w += nx_+na_;
    dlam = w; w += nx_+na_;
    vr = w; w += max(sp_v_.nnz()+sp_r_.nnz(), kkt_.nnz());
    v = vr;
    r = vr + sp_v_.nnz();
    beta = w; w += nx_+na_;
    trans_a = w; w += AT_.nnz();
    infeas = w; w += nx_;
    tinfeas = w; w += nx_;
    casadi_int *neverzero, *neverupper, *neverlower;
    neverzero = iw; iw += nx_+na_;
    neverupper = iw; iw += nx_+na_;
    neverlower = iw; iw += nx_+na_;

    // Smallest strictly positive number
    const double DMIN = std::numeric_limits<double>::min();

    // Bounds on z
    casadi_copy(lbx, nx_, lbz);
    casadi_copy(lba, na_, lbz+nx_);
    casadi_copy(ubx, nx_, ubz);
    casadi_copy(uba, na_, ubz+nx_);

    if (verbose_) {
      print_vector("lbz", lbz, nx_+na_);
      print_vector("ubz", ubz, nx_+na_);
      print_vector("nz_h", h, H_.nnz());
      print_vector("nz_a", a, A_.nnz());
    }

    // Pass initial guess
    casadi_copy(x0, nx_, z);
    casadi_copy(lam_x0, nx_, lam);
    casadi_copy(lam_a0, na_, lam+nx_);

    // Correct lam if needed, determine permitted signs
    for (i=0; i<nx_+na_; ++i) {
      // Permitted signs for lam
      neverzero[i] = lbz[i]==ubz[i];
      neverupper[i] = isinf(ubz[i]);
      neverlower[i] = isinf(lbz[i]);
      casadi_assert(!neverzero[i] || !neverupper[i] || !neverlower[i],
                    "No sign possible for " + str(i));
      // Correct initial active set if required
      if (neverzero[i] && lam[i]==0.) {
        lam[i] = neverupper[i] || z[i]-lbz[i] <= ubz[i]-z[i] ? -DMIN : DMIN;
      } else if (neverupper[i] && lam[i]>0.) {
        lam[i] = neverzero[i] ? -DMIN : 0.;
      } else if (neverlower[i] && lam[i]<0.) {
        lam[i] = neverzero[i] ? DMIN : 0.;
      }
    }

    // Transpose A
    casadi_trans(a, A_, trans_a, AT_, iw);

    // Setup memory structure
    casadi_qp_mem<double> qp_m;
    qp_m.nx = nx_;
    qp_m.na = na_;
    qp_m.nz = nx_+na_;
    qp_m.z = z;
    qp_m.lam = lam;
    qp_m.lbz = lbz;
    qp_m.ubz = ubz;
    qp_m.infeas = infeas;
    qp_m.tinfeas = tinfeas;
    qp_m.dz = dz;
    qp_m.dlam = dlam;
    qp_m.w = w;
    qp_m.iw = iw;
    qp_m.neverzero = neverzero;
    qp_m.neverlower = neverlower;
    qp_m.neverupper = neverupper;
    // Sparsity patterns
    qp_m.sp_a = A_;
    qp_m.sp_h = H_;
    qp_m.sp_at = AT_;
    qp_m.sp_kkt = kkt_;
    // Matrix nonzeros
    qp_m.g = g;
    qp_m.nz_a = a;
    qp_m.nz_at = trans_a;
    qp_m.nz_h = h;
    qp_m.nz_kkt = kkt;
    // QR factorization
    qp_m.nz_v = v;
    qp_m.nz_r = r;
    qp_m.sp_v = sp_v_;
    qp_m.sp_r = sp_r_;
    qp_m.beta = beta;
    qp_m.prinv = get_ptr(prinv_);
    qp_m.pc = get_ptr(pc_);
    // Misc
    qp_m.dmin = DMIN;
    qp_m.inf = inf;
    qp_m.print_iter = print_iter_;
    qp_m.msg[0] = '\0';
    qp_m.tau = 0.;
    qp_m.sing = 0; // set to avoid false positive warning
    qp_m.du_to_pr = du_to_pr_;
    // Constraint to be flipped, if any
    casadi_int index=-2, sign=0, r_index=-2, r_sign=0;
    // QP iterations
    casadi_int iter = 0;
    while (true) {
      // Calculate dependent quantities
      casadi_qp_calc_dependent(&qp_m);
      // Make an active set change
      casadi_qp_flip(&qp_m, &index, &sign, r_index, r_sign);

      // Debugging
      if (verbose_) {
        print_vector("z", z, nx_+na_);
        print_vector("lam", lam, nx_+na_);
      }

      // Form and factorize the KKT system
      casadi_qp_factorize(&qp_m);
      if (verbose_) {
        print_vector("nz_kkt", kkt, kkt_.nnz());
        print_vector("nz_r", r, sp_r_.nnz());
      }

      // Print iteration progress:
      if (print_iter_) {
        if (iter % 10 == 0) {
          print("%5s %5s %9s %9s %5s %9s %5s %9s %5s %9s %40s\n",
                "Iter", "Sing", "fk", "|pr|", "con", "|du|", "var",
                "min_R", "con", "last_tau", "Note");
        }
        print("%5d %5d %9.2g %9.2g %5d %9.2g %5d %9.2g %5d %9.2g %40s\n",
              iter, qp_m.sing, qp_m.f, qp_m.pr, qp_m.ipr, qp_m.du, qp_m.idu,
              qp_m.mina, qp_m.imina, qp_m.tau, qp_m.msg);
      }

      // Successful return if still no change
      if (index==-1) {
        flag = 0;
        break;
      }

      // Too many iterations?
      if (iter>=max_iter_) {
        casadi_warning("Maximum number of iterations reached");
        flag = 1;
        break;
      }

      // Start new iteration
      iter++;
      qp_m.msg[0] = '\0';

      // Calculate search direction
      if (casadi_qp_calc_step(&qp_m, &r_index, &r_sign)) {
        casadi_warning("Failed to calculate search direction");
        flag = 1;
        break;
      }

      if (verbose_) {
        print_vector("dz", dz, nx_+na_);
        print_vector("dlam", dlam, nx_+na_);
      }

      // Line search in the calculated direction
      casadi_qp_linesearch(&qp_m, &index, &sign);
    }

    // Calculate optimal cost
    if (f) *f = qp_m.f;

    // Get solution
    casadi_copy(z, nx_, x);
    casadi_copy(lam, nx_, lam_x);
    casadi_copy(lam+nx_, na_, lam_a);

    return flag;
  }

} // namespace casadi
