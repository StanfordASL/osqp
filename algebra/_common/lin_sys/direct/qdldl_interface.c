#include "glob_opts.h"
#include "algebra_impl.h"

#include "qdldl.h"
#include "qdldl_interface.h"

#ifndef EMBEDDED
#include "amd.h"
#endif

#if EMBEDDED != 1
#include "kkt.h"
#endif

void update_settings_linsys_solver_qdldl(qdldl_solver       *s,
                                         const OSQPSettings *settings) {
  return;
}

// Warm starting not used by direct solvers
void warm_start_linsys_solver_qdldl(qdldl_solver      *s,
                                    const OSQPVectorf *x) {
  return;
}

#ifndef EMBEDDED

// Free LDL Factorization structure
void free_linsys_solver_qdldl(qdldl_solver *s) {
    if (s) {
        if (s->L) {
            if (s->L->p) c_free(s->L->p);
            if (s->L->i) c_free(s->L->i);
            if (s->L->x) c_free(s->L->x);
            c_free(s->L);
        }

        if (s->P)           c_free(s->P);
        if (s->Dinv)        c_free(s->Dinv);
        if (s->bp)          c_free(s->bp);
        if (s->sol)         c_free(s->sol);
        if (s->rho_inv_vec) c_free(s->rho_inv_vec);

        // These are required for matrix updates
        if (s->KKT)       csc_spfree(s->KKT);
        if (s->PtoKKT)    c_free(s->PtoKKT);
        if (s->AtoKKT)    c_free(s->AtoKKT);
        if (s->rhotoKKT)  c_free(s->rhotoKKT);

        // QDLDL workspace
        if (s->D)         c_free(s->D);
        if (s->etree)     c_free(s->etree);
        if (s->Lnz)       c_free(s->Lnz);
        if (s->iwork)     c_free(s->iwork);
        if (s->bwork)     c_free(s->bwork);
        if (s->fwork)     c_free(s->fwork);
        c_free(s);

    }
}


/**
 * Compute LDL factorization of matrix A
 * @param  A    Matrix to be factorized
 * @param  p    Private workspace
 * @param  nvar Number of QP variables
 * @return      exitstatus (0 is good)
 */
static c_int LDL_factor(csc          *A,
                        qdldl_solver *p,
                        c_int         nvar){

    c_int sum_Lnz;
    c_int factor_status;

    // Compute elimination tree
    sum_Lnz = QDLDL_etree(A->n, A->p, A->i, p->iwork, p->Lnz, p->etree);

    if (sum_Lnz < 0){
      // Error
      c_eprint("Error in KKT matrix LDL factorization when computing the elimination tree.");
      if(sum_Lnz == -1){
        c_eprint("Matrix is not perfectly upper triangular.");
      }
      else if(sum_Lnz == -2){
        c_eprint("Integer overflow in L nonzero count.");
      }
      return sum_Lnz;
    }

    // Allocate memory for Li and Lx
    p->L->i = (c_int *)c_malloc(sizeof(c_int)*sum_Lnz);
    p->L->x = (c_float *)c_malloc(sizeof(c_float)*sum_Lnz);
    p->L->nzmax = sum_Lnz;

    // Factor matrix
    factor_status = QDLDL_factor(A->n, A->p, A->i, A->x,
                                 p->L->p, p->L->i, p->L->x,
                                 p->D, p->Dinv, p->Lnz,
                                 p->etree, p->bwork, p->iwork, p->fwork);

    if (factor_status < 0){
      // Error
      c_eprint("Error in KKT matrix LDL factorization when computing the nonzero elements. There are zeros in the diagonal matrix");
      return factor_status;
    } else if (factor_status < nvar) {
      // Error: Number of positive elements of D should be equal to nvar
      c_eprint("Error in KKT matrix LDL factorization when computing the nonzero elements. The problem seems to be non-convex");
      return -2;
    }

    return 0;

}


static c_int permute_KKT(csc          **KKT,
                         qdldl_solver  *p,
                         c_int          Pnz,
                         c_int          Anz,
                         c_int          m,
                         c_int         *PtoKKT,
                         c_int         *AtoKKT,
                         c_int         *rhotoKKT){
    c_float *info;
    c_int amd_status;
    c_int * Pinv;
    csc *KKT_temp;
    c_int * KtoPKPt;
    c_int i; // Indexing

    info = (c_float *)c_malloc(AMD_INFO * sizeof(c_float));

    // Compute permutation matrix P using AMD
#ifdef DLONG
    amd_status = amd_l_order((*KKT)->n, (*KKT)->p, (*KKT)->i, p->P, (c_float *)OSQP_NULL, info);
#else
    amd_status = amd_order((*KKT)->n, (*KKT)->p, (*KKT)->i, p->P, (c_float *)OSQP_NULL, info);
#endif
    if (amd_status < 0) {
        // Free Amd info and return an error
        c_free(info);
        return amd_status;
    }


    // Inverse of the permutation vector
    Pinv = csc_pinv(p->P, (*KKT)->n);

    // Permute KKT matrix
    if (!PtoKKT && !AtoKKT && !rhotoKKT){  // No vectors to be stored
        // Assign values of mapping
        KKT_temp = csc_symperm((*KKT), Pinv, OSQP_NULL, 1);
    }
    else {
        // Allocate vector of mappings from unpermuted to permuted
        KtoPKPt = c_malloc((*KKT)->p[(*KKT)->n] * sizeof(c_int));
        KKT_temp = csc_symperm((*KKT), Pinv, KtoPKPt, 1);

        // Update vectors PtoKKT, AtoKKT and rhotoKKT
        if (PtoKKT){
            for (i = 0; i < Pnz; i++){
                PtoKKT[i] = KtoPKPt[PtoKKT[i]];
            }
        }
        if (AtoKKT){
            for (i = 0; i < Anz; i++){
                AtoKKT[i] = KtoPKPt[AtoKKT[i]];
            }
        }
        if (rhotoKKT){
            for (i = 0; i < m; i++){
                rhotoKKT[i] = KtoPKPt[rhotoKKT[i]];
            }
        }

        // Cleanup vector of mapping
        c_free(KtoPKPt);
    }

    // Cleanup
    // Free previous KKT matrix and assign pointer to new one
    csc_spfree((*KKT));
    (*KKT) = KKT_temp;
    // Free Pinv
    c_free(Pinv);
    // Free Amd info
    c_free(info);

    return 0;
}


// Initialize LDL Factorization structure
c_int init_linsys_solver_qdldl(qdldl_solver      **sp,
                               const OSQPMatrix   *P,
                               const OSQPMatrix   *A,
                               const OSQPVectorf  *rho_vec,
                               const OSQPSettings *settings,
                               c_int               polishing) {

    // Define Variables
    csc * KKT_temp;     // Temporary KKT pointer
    c_int i;            // Loop counter
    c_int m, n;         // Dimensions of A
    c_int n_plus_m;     // Define n_plus_m dimension
    c_float* rhov;      // used for direct access to rho_vec data when polishing=false

    c_float sigma = settings->sigma;

    // Allocate private structure to store KKT factorization
    qdldl_solver *s = c_calloc(1, sizeof(qdldl_solver));
    *sp = s;

    // Size of KKT
    n = P->csc->n;
    m = A->csc->m;
    s->n = n;
    s->m = m;
    n_plus_m = n + m;

    // Scalar parameters
    s->sigma = sigma;
    s->rho_inv = 1. / settings->rho;

    // Polishing flag
    s->polishing = polishing;

    // Link Functions
    s->name            = &name_qdldl;
    s->solve           = &solve_linsys_qdldl;
    s->update_settings = &update_settings_linsys_solver_qdldl;
    s->warm_start      = &warm_start_linsys_solver_qdldl;


#ifndef EMBEDDED
    s->free = &free_linsys_solver_qdldl;
#endif

#if EMBEDDED != 1
    s->update_matrices = &update_linsys_solver_matrices_qdldl;
    s->update_rho_vec  = &update_linsys_solver_rho_vec_qdldl;
#endif

    // Assign type
    s->type = OSQP_DIRECT_SOLVER;

    // Set number of threads to 1 (single threaded)
    s->nthreads = 1;

    // Sparse matrix L (lower triangular)
    // NB: We don not allocate L completely (CSC elements)
    //      L will be allocated during the factorization depending on the
    //      resulting number of elements.
    s->L = c_calloc(1, sizeof(csc));
    s->L->m  = n_plus_m;
    s->L->n  = n_plus_m;
    s->L->nz = -1;
    s->L->p  = (c_int *)c_malloc((n_plus_m+1) * sizeof(QDLDL_int));

    // Diagonal matrix stored as a vector D
    s->Dinv = (QDLDL_float *)c_malloc(sizeof(QDLDL_float) * n_plus_m);
    s->D    = (QDLDL_float *)c_malloc(sizeof(QDLDL_float) * n_plus_m);

    // Permutation vector P
    s->P    = (QDLDL_int *)c_malloc(sizeof(QDLDL_int) * n_plus_m);

    // Working vector
    s->bp   = (QDLDL_float *)c_malloc(sizeof(QDLDL_float) * n_plus_m);

    // Solution vector
    s->sol  = (QDLDL_float *)c_malloc(sizeof(QDLDL_float) * n_plus_m);

    // Parameter vector
    if (rho_vec)
      s->rho_inv_vec = (c_float *)c_malloc(sizeof(c_float) * m);
    // else it is NULL

    // Elimination tree workspace
    s->etree = (QDLDL_int *)c_malloc(n_plus_m * sizeof(QDLDL_int));
    s->Lnz   = (QDLDL_int *)c_malloc(n_plus_m * sizeof(QDLDL_int));

    // Lx and Li are sparsity dependent, so set them to
    // null initially so we don't try to free them prematurely
    s->L->i = OSQP_NULL;
    s->L->x = OSQP_NULL;

    // Preallocate workspace
    s->iwork = (QDLDL_int *)c_malloc(sizeof(QDLDL_int)*(3*n_plus_m));
    s->bwork = (QDLDL_bool *)c_malloc(sizeof(QDLDL_bool)*n_plus_m);
    s->fwork = (QDLDL_float *)c_malloc(sizeof(QDLDL_float)*n_plus_m);

    // Form and permute KKT matrix
    if (polishing){ // Called from polish()

        KKT_temp = form_KKT(P->csc,A->csc,
                            0, //format = 0 means CSC
                            sigma, s->rho_inv_vec, sigma,
                            OSQP_NULL, OSQP_NULL, OSQP_NULL);

        // Permute matrix
        if (KKT_temp)
            permute_KKT(&KKT_temp, s, OSQP_NULL, OSQP_NULL, OSQP_NULL, OSQP_NULL, OSQP_NULL, OSQP_NULL);
    }
    else { // Called from ADMM algorithm

        // Allocate vectors of indices
        s->PtoKKT = c_malloc(P->csc->p[n] * sizeof(c_int));
        s->AtoKKT = c_malloc(A->csc->p[n] * sizeof(c_int));
        s->rhotoKKT = c_malloc(m * sizeof(c_int));

        // Use p->rho_inv_vec for storing param2 = rho_inv_vec
        if (rho_vec) {
          rhov = rho_vec->values;
          for (i = 0; i < m; i++){
              s->rho_inv_vec[i] = 1. / rhov[i];
          }
        }
        else {
          s->rho_inv = 1. / settings->rho;
        }

        KKT_temp = form_KKT(P->csc,A->csc,
                            0, //format = 0 means CSC format
                            sigma, s->rho_inv_vec, s->rho_inv,
                            s->PtoKKT, s->AtoKKT,s->rhotoKKT);

        // Permute matrix
        if (KKT_temp){
            permute_KKT(&KKT_temp, s, P->csc->p[n], A->csc->p[n], m, s->PtoKKT, s->AtoKKT, s->rhotoKKT);
        }
    }

    // Check if matrix has been created
    if (!KKT_temp){
        c_eprint("Error forming and permuting KKT matrix");
        free_linsys_solver_qdldl(s);
        *sp = OSQP_NULL;
        return OSQP_LINSYS_SOLVER_INIT_ERROR;
    }

    // Factorize the KKT matrix
    if (LDL_factor(KKT_temp, s, n) < 0) {
        csc_spfree(KKT_temp);
        free_linsys_solver_qdldl(s);
        *sp = OSQP_NULL;
        return OSQP_NONCVX_ERROR;
    }

    if (polishing){ // If KKT passed, assign it to KKT_temp
        // Polish, no need for KKT_temp
        csc_spfree(KKT_temp);
    }
    else { // If not embedded option 1 copy pointer to KKT_temp. Do not free it.
        s->KKT = KKT_temp;
    }


    // No error
    return 0;
}

#endif  // EMBEDDED

const char* name_qdldl() {
  return "QDLDL";
}


/* solve P'LDL'P x = b for x */
static void LDLSolve(c_float       *x,
                     const c_float *b,
                     const csc     *L,
                     const c_float *Dinv,
                     const c_int   *P,
                     c_float       *bp) {

  c_int j;
  c_int n = L->n;

  // permute_x(L->n, bp, b, P);
  for (j = 0 ; j < n ; j++) bp[j] = b[P[j]];

  QDLDL_solve(L->n, L->p, L->i, L->x, Dinv, bp);

  // permutet_x(L->n, x, bp, P);
  for (j = 0 ; j < n ; j++) x[P[j]] = bp[j];
}


c_int solve_linsys_qdldl(qdldl_solver *s,
                         OSQPVectorf  *b,
                         c_int         admm_iter) {

  c_int j;
  c_int n = s->n;
  c_int m = s->m;
  c_float* bv = b->values;

#ifndef EMBEDDED
  if (s->polishing) {
    /* stores solution to the KKT system in b */
    LDLSolve(bv, bv, s->L, s->Dinv, s->P, s->bp);
  } else {
#endif
    /* stores solution to the KKT system in s->sol */
    LDLSolve(s->sol, bv, s->L, s->Dinv, s->P, s->bp);

    /* copy x_tilde from s->sol */
    for (j = 0 ; j < n ; j++) {
      bv[j] = s->sol[j];
    }

    /* compute z_tilde from b and s->sol */
    if (s->rho_inv_vec) {
      for (j = 0 ; j < m ; j++) {
        bv[j + n] += s->rho_inv_vec[j] * s->sol[j + n];
      }
    }
    else {
      for (j = 0 ; j < m ; j++) {
        bv[j + n] += s->rho_inv * s->sol[j + n];
      }
    }
#ifndef EMBEDDED
  }
#endif
  return 0;
}


#if EMBEDDED != 1

// Update private structure with new P and A
c_int update_linsys_solver_matrices_qdldl(qdldl_solver     *s,
                                          const OSQPMatrix *P,
                                          const c_int* Px_new_idx,
                                          c_int P_new_n,
                                          const OSQPMatrix *A,
                                          const c_int* Ax_new_idx,
                                          c_int A_new_n) {

    int pos_D_count;

    // Update KKT matrix with new P
    update_KKT_P(s->KKT, P->csc, Px_new_idx, P_new_n, s->PtoKKT, s->sigma, 0);

    // Update KKT matrix with new A
    update_KKT_A(s->KKT, A->csc, Ax_new_idx, A_new_n, s->AtoKKT);

    pos_D_count = QDLDL_factor(s->KKT->n, s->KKT->p, s->KKT->i, s->KKT->x,
        s->L->p, s->L->i, s->L->x, s->D, s->Dinv, s->Lnz,
        s->etree, s->bwork, s->iwork, s->fwork);

    //number of positive elements in D should match the
    //dimension of P if P + \sigma I is PD.   Error otherwise.
    return (pos_D_count == P->csc->n) ? 0 : 1;
}


c_int update_linsys_solver_rho_vec_qdldl(qdldl_solver      *s,
                                         const OSQPVectorf *rho_vec,
                                         c_float            rho_sc) {

    c_int i;
    c_int m = s->m;
    c_float* rhov;

    // Update internal rho_inv_vec
    if (s->rho_inv_vec) {
      rhov = rho_vec->values;
      for (i = 0; i < m; i++){
          s->rho_inv_vec[i] = 1. / rhov[i];
      }
    }
    else {
      s->rho_inv = 1. / rho_sc;
    }

    // Update KKT matrix with new rho_vec
    update_KKT_param2(s->KKT, s->rho_inv_vec, s->rho_inv, s->rhotoKKT, s->m);

    return (QDLDL_factor(s->KKT->n, s->KKT->p, s->KKT->i, s->KKT->x,
        s->L->p, s->L->i, s->L->x, s->D, s->Dinv, s->Lnz,
        s->etree, s->bwork, s->iwork, s->fwork) < 0);
}

#endif