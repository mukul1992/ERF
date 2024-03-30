#include <AMReX.H>
#include <AMReX_TableData.H>

#include <TI_slow_headers.H>

using namespace amrex;

/**
 * Function for computing the slow RHS for the evolution equations for the scalars other than density or potential temperature
 *
 * @param[in]  level level of resolution
 * @param[in]  finest_level finest level of resolution
 * @param[in]  nrk   which RK stage
 * @param[in]  dt    slow time step
 * @param[out]  S_rhs RHS computed here
 * @param[in]  S_old solution at start of time step
 * @param[in]  S_new solution at end of current RK stage
 * @param[in]  S_data current solution
 * @param[in]  S_prim primitive variables (i.e. conserved variables divided by density)
 * @param[in]  S_scratch scratch space
 * @param[in]  xvel x-component of velocity
 * @param[in]  yvel y-component of velocity
 * @param[in]  zvel z-component of velocity
 * @param[in] source source terms for conserved variables
 * @param[in] SmnSmn strain rate magnitude
 * @param[in] eddyDiffs diffusion coefficients for LES turbulence models
 * @param[in] Hfx3 heat flux in z-dir
 * @param[in] Diss dissipation of turbulent kinetic energy
 * @param[in]  geom   Container for geometric information
 * @param[in]  solverChoice  Container for solver parameters
 * @param[in]  most  Pointer to MOST class for Monin-Obukhov Similarity Theory boundary condition
 * @param[in]  domain_bcs_type_d device vector for domain boundary conditions
 * @param[in] z_phys_nd height coordinate at nodes
 * @param[in] detJ     Jacobian of the metric transformation at start of time step (= 1 if use_terrain is false)
 * @param[in] detJ_new Jacobian of the metric transformation at new RK stage time (= 1 if use_terrain is false)
 * @param[in] mapfac_m map factor at cell centers
 * @param[in] mapfac_u map factor at x-faces
 * @param[in] mapfac_v map factor at y-faces
 * @param[inout] fr_as_crse YAFluxRegister at level l at level l   / l+1 interface
 * @param[inout] fr_as_fine YAFluxRegister at level l at level l-1 / l   interface
 */

void erf_slow_rhs_post (int level, int finest_level,
                        int nrk,
                        Real dt,
                        Vector<MultiFab>& S_rhs,
                        Vector<MultiFab>& S_old,
                        Vector<MultiFab>& S_new,
                        Vector<MultiFab>& S_data,
                        const MultiFab& S_prim,
                        Vector<MultiFab>& S_scratch,
                        const MultiFab& xvel,
                        const MultiFab& yvel,
                        const MultiFab& /*zvel*/,
                        const MultiFab& source,
                        const MultiFab* SmnSmn,
                        const MultiFab* eddyDiffs,
                        MultiFab* Hfx3, MultiFab* Diss,
                        const Geometry geom,
                        const SolverChoice& solverChoice,
                        std::unique_ptr<ABLMost>& most,
                        const Gpu::DeviceVector<BCRec>& domain_bcs_type_d,
                        const Vector<BCRec>& domain_bcs_type_h,
                        std::unique_ptr<MultiFab>& z_phys_nd,
                        std::unique_ptr<MultiFab>& detJ,
                        std::unique_ptr<MultiFab>& detJ_new,
                        std::unique_ptr<MultiFab>& mapfac_m,
                        std::unique_ptr<MultiFab>& mapfac_u,
                        std::unique_ptr<MultiFab>& mapfac_v,
#ifdef ERF_USE_EB
                        amrex::EBFArrayBoxFactory const& ebfact,
#endif
#if defined(ERF_USE_NETCDF)
                        const bool& moist_set_rhs,
                        const Real& bdy_time_interval,
                        const Real& start_bdy_time,
                        const Real& new_stage_time,
                        int  width,
                        int  set_width,
                        Vector<Vector<FArrayBox>>& bdy_data_xlo,
                        Vector<Vector<FArrayBox>>& bdy_data_xhi,
                        Vector<Vector<FArrayBox>>& bdy_data_ylo,
                        Vector<Vector<FArrayBox>>& bdy_data_yhi,
#endif
                        const Real* dptr_rhoqt_src,
                        const Real* dptr_wbar_sub,
                        YAFluxRegister* fr_as_crse,
                        YAFluxRegister* fr_as_fine)
{
    BL_PROFILE_REGION("erf_slow_rhs_post()");

    const BCRec* bc_ptr_d = domain_bcs_type_d.data();
    const BCRec* bc_ptr_h = domain_bcs_type_h.data();

    AdvChoice  ac = solverChoice.advChoice;
    DiffChoice dc = solverChoice.diffChoice;
    TurbChoice tc = solverChoice.turbChoice[level];

    const MultiFab* t_mean_mf = nullptr;
    if (most) t_mean_mf = most->get_mac_avg(0,2);

    const bool l_use_terrain      = solverChoice.use_terrain;
    const bool l_reflux = (solverChoice.coupling_type != CouplingType::OneWay);
    const bool l_moving_terrain   = (solverChoice.terrain_type == TerrainType::Moving);
    if (l_moving_terrain) AMREX_ALWAYS_ASSERT(l_use_terrain);

    const bool l_use_ndiff      = solverChoice.use_NumDiff;
    const bool l_use_QKE        = tc.use_QKE && tc.advect_QKE;
    const bool l_use_deardorff  = (tc.les_type == LESType::Deardorff);
    const bool l_use_diff       = ((dc.molec_diff_type != MolecDiffType::None) ||
                                   (tc.les_type        !=       LESType::None) ||
                                   (tc.pbl_type        !=       PBLType::None) );
    const bool l_use_turb       = ( tc.les_type == LESType::Smagorinsky ||
                                    tc.les_type == LESType::Deardorff   ||
                                    tc.pbl_type == PBLType::MYNN25      ||
                                    tc.pbl_type == PBLType::YSU );

    // Open bc will be imposed upon all vars (we only access cons here for simplicity)
    const bool xlo_open = (bc_ptr_h[BCVars::cons_bc].lo(0) == ERFBCType::open);
    const bool xhi_open = (bc_ptr_h[BCVars::cons_bc].hi(0) == ERFBCType::open);
    const bool ylo_open = (bc_ptr_h[BCVars::cons_bc].lo(1) == ERFBCType::open);
    const bool yhi_open = (bc_ptr_h[BCVars::cons_bc].hi(1) == ERFBCType::open);

    const Box& domain = geom.Domain();

    const GpuArray<Real, AMREX_SPACEDIM> dxInv = geom.InvCellSizeArray();
    const Real* dx = geom.CellSize();

    // *************************************************************************
    // Set gravity as a vector
    // *************************************************************************
    const    Array<Real,AMREX_SPACEDIM> grav{0.0, 0.0, -solverChoice.gravity};
    const GpuArray<Real,AMREX_SPACEDIM> grav_gpu{grav[0], grav[1], grav[2]};

    // *************************************************************************
    // Planar averages for subsidence terms
    // *************************************************************************
    Table1D<Real> dptr_q_plane;
    TableData<Real, 1> q_plane_tab;
    if (dptr_wbar_sub && (solverChoice.moisture_type != MoistureType::None)) {
        PlaneAverage q_ave(&S_prim, geom, solverChoice.ave_plane, true);
        q_ave.compute_averages(ZDir(), q_ave.field());

        int ncell = q_ave.ncell_line();
        Gpu::HostVector<  Real> q_plane_h(ncell);
        Gpu::DeviceVector<Real> q_plane_d(ncell);

        q_ave.line_average(PrimQ1_comp, q_plane_h);
        Gpu::copyAsync(Gpu::hostToDevice, q_plane_h.begin(), q_plane_h.end(), q_plane_d.begin());

        Real* dptr_q = q_plane_d.data();

        IntVect ng_c = S_prim.nGrowVect();
        Box qdomain  = domain; qdomain.grow(2,ng_c[2]);
        q_plane_tab.resize({qdomain.smallEnd(2)}, {qdomain.bigEnd(2)});

        int q_offset = ng_c[2];
        dptr_q_plane = q_plane_tab.table();
        ParallelFor(ncell, [=] AMREX_GPU_DEVICE (int k) noexcept
        {
            dptr_q_plane(k-q_offset) = dptr_q[k];
        });
    }

    // *************************************************************************
    // Pre-computed quantities
    // *************************************************************************
    int nvars                     = S_data[IntVars::cons].nComp();
    const BoxArray& ba            = S_data[IntVars::cons].boxArray();
    const DistributionMapping& dm = S_data[IntVars::cons].DistributionMap();

    std::unique_ptr<MultiFab> dflux_x;
    std::unique_ptr<MultiFab> dflux_y;
    std::unique_ptr<MultiFab> dflux_z;

    if (l_use_diff) {
        dflux_x = std::make_unique<MultiFab>(convert(ba,IntVect(1,0,0)), dm, nvars, 0);
        dflux_y = std::make_unique<MultiFab>(convert(ba,IntVect(0,1,0)), dm, nvars, 0);
        dflux_z = std::make_unique<MultiFab>(convert(ba,IntVect(0,0,1)), dm, nvars, 0);
    } else {
        dflux_x = nullptr;
        dflux_y = nullptr;
        dflux_z = nullptr;
    }

    // Valid Open BC vars
    Vector<int> is_valid_slow_var; is_valid_slow_var.resize(nvars,0);
    if (l_use_deardorff) is_valid_slow_var[RhoKE_comp]  = 1;
    if (l_use_QKE)       is_valid_slow_var[RhoQKE_comp] = 1;
    for (int ivar(RhoScalar_comp); ivar<nvars; ++ivar) { is_valid_slow_var[ivar] = 1; }

    // *************************************************************************
    // Calculate cell-centered eddy viscosity & diffusivities
    //
    // Notes -- we fill all the data in ghost cells before calling this so
    //    that we can fill the eddy viscosity in the ghost regions and
    //    not have to call a boundary filler on this data itself
    //
    // LES - updates both horizontal and vertical eddy viscosity components
    // PBL - only updates vertical eddy viscosity components so horizontal
    //       components come from the LES model or are left as zero.
    // *************************************************************************

    // *************************************************************************
    // Define updates and fluxes in the current RK stage
    // *************************************************************************
#ifdef _OPENMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
    {
      std::array<FArrayBox,AMREX_SPACEDIM> flux;

      int start_comp;
      int   num_comp;

      for ( MFIter mfi(S_data[IntVars::cons],TilingIfNotGPU()); mfi.isValid(); ++mfi) {

        Box tbx  = mfi.tilebox();

        // Only advection operations in bndry normal direction with OPEN BC
        Box tbx_xlo, tbx_xhi, tbx_ylo, tbx_yhi;
        if (xlo_open) {
            if (tbx.smallEnd(0) == domain.smallEnd(0)) { tbx_xlo = makeSlab(tbx,0,domain.smallEnd(0)); }
        }
        if (xhi_open) {
            if (tbx.bigEnd(0) == domain.bigEnd(0))     { tbx_xhi = makeSlab(tbx,0,domain.bigEnd(0));   }
        }
        if (ylo_open) {
            if (tbx.smallEnd(1) == domain.smallEnd(1)) { tbx_ylo = makeSlab(tbx,1,domain.smallEnd(1)); }
        }
        if (yhi_open) {
            if (tbx.bigEnd(1) == domain.bigEnd(1))     { tbx_yhi = makeSlab(tbx,1,domain.bigEnd(1));   }
        }

        // *************************************************************************
        // Define flux arrays for use in advection
        // *************************************************************************
        for (int dir = 0; dir < AMREX_SPACEDIM; ++dir) {
            flux[dir].resize(surroundingNodes(tbx,dir),nvars);
            flux[dir].setVal<RunOn::Device>(0.);
        }
        const GpuArray<const Array4<Real>, AMREX_SPACEDIM>
            flx_arr{{AMREX_D_DECL(flux[0].array(), flux[1].array(), flux[2].array())}};

        // *************************************************************************
        // Define Array4's
        // *************************************************************************
        const Array4<      Real> & old_cons   = S_old[IntVars::cons].array(mfi);
        const Array4<      Real> & cell_rhs   = S_rhs[IntVars::cons].array(mfi);

        const Array4<      Real> & new_cons  = S_new[IntVars::cons].array(mfi);
        const Array4<      Real> & new_xmom  = S_new[IntVars::xmom].array(mfi);
        const Array4<      Real> & new_ymom  = S_new[IntVars::ymom].array(mfi);
        const Array4<      Real> & new_zmom  = S_new[IntVars::zmom].array(mfi);

        const Array4<      Real> & cur_cons  = S_data[IntVars::cons].array(mfi);
        const Array4<const Real> & cur_prim  = S_prim.array(mfi);
        const Array4<      Real> & cur_xmom  = S_data[IntVars::xmom].array(mfi);
        const Array4<      Real> & cur_ymom  = S_data[IntVars::ymom].array(mfi);
        const Array4<      Real> & cur_zmom  = S_data[IntVars::zmom].array(mfi);

        Array4<Real> avg_xmom = S_scratch[IntVars::xmom].array(mfi);
        Array4<Real> avg_ymom = S_scratch[IntVars::ymom].array(mfi);
        Array4<Real> avg_zmom = S_scratch[IntVars::zmom].array(mfi);

        const Array4<const Real> & u = xvel.array(mfi);
        const Array4<const Real> & v = yvel.array(mfi);

        const Array4<Real const>& mu_turb = l_use_turb ? eddyDiffs->const_array(mfi) : Array4<const Real>{};

        // Metric terms
        const Array4<const Real>& z_nd         = l_use_terrain    ? z_phys_nd->const_array(mfi) : Array4<const Real>{};
        const Array4<const Real>& detJ_arr     = l_use_terrain    ? detJ->const_array(mfi)        : Array4<const Real>{};
        const Array4<const Real>& detJ_new_arr = l_moving_terrain ? detJ_new->const_array(mfi)    : Array4<const Real>{};

        // Map factors
        const Array4<const Real>& mf_m = mapfac_m->const_array(mfi);
        const Array4<const Real>& mf_u = mapfac_u->const_array(mfi);
        const Array4<const Real>& mf_v = mapfac_v->const_array(mfi);

        // SmnSmn for KE src with Deardorff
        const Array4<const Real>& SmnSmn_a = l_use_deardorff ? SmnSmn->const_array(mfi) : Array4<const Real>{};

        // **************************************************************************
        // Here we fill the "current" data with "new" data because that is the result of the previous RK stage
        // **************************************************************************
        int nsv = S_old[IntVars::cons].nComp() - 2;
        const GpuArray<int, IntVars::NumTypes> scomp_slow = {  2,0,0,0};
        const GpuArray<int, IntVars::NumTypes> ncomp_slow = {nsv,0,0,0};

        {
        BL_PROFILE("rhs_post_7");
        ParallelFor(tbx, ncomp_slow[IntVars::cons],
        [=] AMREX_GPU_DEVICE (int i, int j, int k, int nn) {
            const int n = scomp_slow[IntVars::cons] + nn;
            cur_cons(i,j,k,n) = new_cons(i,j,k,n);
        });
        } // end profile

        // We have projected the velocities stored in S_data but we will use
        //    the velocities stored in S_scratch to update the scalars, so
        //    we need to copy from S_data (projected) into S_scratch
        if (solverChoice.incompressible) {
            Box tbx_inc = mfi.nodaltilebox(0);
            Box tby_inc = mfi.nodaltilebox(1);
            Box tbz_inc = mfi.nodaltilebox(2);

            ParallelFor(tbx_inc, tby_inc, tbz_inc,
            [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                avg_xmom(i,j,k) = cur_xmom(i,j,k);
            },
            [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                avg_ymom(i,j,k) = cur_ymom(i,j,k);
            },
            [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
                avg_zmom(i,j,k) = cur_zmom(i,j,k);
            });
        }

        // **************************************************************************
        // Define updates in the RHS of continuity, temperature, and scalar equations
        // **************************************************************************
#ifdef ERF_USE_EB
        const auto& vf_arr = ebfact.getVolFrac().const_array(mfi);
#endif

        AdvType    horiz_adv_type = ac.dryscal_horiz_adv_type;
        AdvType     vert_adv_type = ac.dryscal_vert_adv_type;
        const Real horiz_upw_frac = ac.dryscal_horiz_upw_frac;
        const Real  vert_upw_frac = ac.dryscal_vert_upw_frac;

        if (ac.use_efficient_advection){
             horiz_adv_type = EfficientAdvType(nrk,ac.dryscal_horiz_adv_type);
              vert_adv_type = EfficientAdvType(nrk,ac.dryscal_vert_adv_type);
        }

        if (l_use_deardorff) {
            start_comp = RhoKE_comp;
              num_comp = 1;
            AdvectionSrcForScalars(tbx, start_comp, num_comp, avg_xmom, avg_ymom, avg_zmom,
                                   cur_prim, cell_rhs, detJ_arr, dxInv, mf_m,
                                   horiz_adv_type, vert_adv_type,
                                   horiz_upw_frac, vert_upw_frac,
#ifdef ERF_USE_EB
                                   vf_arr,
#endif
                                   l_use_terrain, flx_arr);
        }
        if (l_use_QKE) {
            start_comp = RhoQKE_comp;
              num_comp = 1;
            AdvectionSrcForScalars(tbx, start_comp, num_comp, avg_xmom, avg_ymom, avg_zmom,
                                   cur_prim, cell_rhs, detJ_arr, dxInv, mf_m,
                                   horiz_adv_type, vert_adv_type,
                                   horiz_upw_frac, vert_upw_frac,
#ifdef ERF_USE_EB
                                   vf_arr,
#endif
                                   l_use_terrain, flx_arr);
        }

        // This is simply an advected scalar for convenience
        start_comp = RhoScalar_comp;
        num_comp = 1;
        AdvectionSrcForScalars(tbx, start_comp, num_comp, avg_xmom, avg_ymom, avg_zmom,
                               cur_prim, cell_rhs, detJ_arr, dxInv, mf_m,
                               horiz_adv_type, vert_adv_type,
                               horiz_upw_frac, vert_upw_frac,
#ifdef ERF_USE_EB
                               vf_arr,
#endif
                               l_use_terrain, flx_arr);

        if (solverChoice.moisture_type != MoistureType::None)
        {
            start_comp = RhoQ1_comp;
              num_comp = nvars - start_comp;

            AdvType    moist_horiz_adv_type = ac.moistscal_horiz_adv_type;
            AdvType     moist_vert_adv_type = ac.moistscal_vert_adv_type;
            const Real moist_horiz_upw_frac = ac.moistscal_horiz_upw_frac;
            const Real  moist_vert_upw_frac = ac.moistscal_vert_upw_frac;

            if (ac.use_efficient_advection){
                 moist_horiz_adv_type = EfficientAdvType(nrk,ac.moistscal_horiz_adv_type);
                 moist_vert_adv_type  = EfficientAdvType(nrk,ac.moistscal_vert_adv_type);
            }

            AdvectionSrcForScalars(tbx, start_comp, num_comp, avg_xmom, avg_ymom, avg_zmom,
                                   cur_prim, cell_rhs, detJ_arr, dxInv, mf_m,
                                   moist_horiz_adv_type, moist_vert_adv_type,
                                   moist_horiz_upw_frac, moist_vert_upw_frac,
#ifdef ERF_USE_EB
                                   vf_arr,
#endif
                                   l_use_terrain, flx_arr);
        }

        // Special advection operator for open BC (bndry normal operations)
        for (int ivar(RhoKE_comp); ivar<nvars; ++ivar) {
            if (is_valid_slow_var[ivar]) {
                if (xlo_open) {
                    bool do_lo = true;
                    AdvectionSrcForOpenBC_Tangent_Cons(tbx_xlo, 0, ivar, 1, cell_rhs, cur_prim,
                                                       avg_xmom, avg_ymom, avg_zmom,
                                                       detJ_arr, dxInv, l_use_terrain, do_lo);
                }
                if (xhi_open) {
                    AdvectionSrcForOpenBC_Tangent_Cons(tbx_xhi, 0, ivar, 1, cell_rhs, cur_prim,
                                                       avg_xmom, avg_ymom, avg_zmom,
                                                       detJ_arr, dxInv, l_use_terrain);
                }
                if (ylo_open) {
                    bool do_lo = true;
                    AdvectionSrcForOpenBC_Tangent_Cons(tbx_ylo, 1, ivar, 1, cell_rhs, cur_prim,
                                                       avg_xmom, avg_ymom, avg_zmom,
                                                       detJ_arr, dxInv, l_use_terrain, do_lo);
                }
                if (yhi_open) {
                    AdvectionSrcForOpenBC_Tangent_Cons(tbx_yhi, 1, ivar, 1, cell_rhs, cur_prim,
                                                       avg_xmom, avg_ymom, avg_zmom,
                                                       detJ_arr, dxInv, l_use_terrain);
                }
            } // valid slow var
        } // loop ivar

        if (l_use_diff) {
            Array4<Real> diffflux_x = dflux_x->array(mfi);
            Array4<Real> diffflux_y = dflux_y->array(mfi);
            Array4<Real> diffflux_z = dflux_z->array(mfi);

            Array4<Real> hfx_z = Hfx3->array(mfi);
            Array4<Real> diss  = Diss->array(mfi);

            const Array4<const Real> tm_arr = t_mean_mf ? t_mean_mf->const_array(mfi) : Array4<const Real>{};

            const bool use_most = (most != nullptr);

            if (l_use_deardorff) {
                start_comp = RhoKE_comp;
                  num_comp = 1;
                if (l_use_terrain) {
                    DiffusionSrcForState_T(tbx, domain, start_comp, num_comp, u, v,
                                           cur_cons, cur_prim, cell_rhs,
                                           diffflux_x, diffflux_y, diffflux_z, z_nd, detJ_arr,
                                           dxInv, SmnSmn_a, mf_m, mf_u, mf_v,
                                           hfx_z, diss,
                                           mu_turb, dc, tc, tm_arr, grav_gpu, bc_ptr_d, use_most);
                } else {
                    DiffusionSrcForState_N(tbx, domain, start_comp, num_comp, u, v,
                                           cur_cons, cur_prim, cell_rhs,
                                           diffflux_x, diffflux_y, diffflux_z,
                                           dxInv, SmnSmn_a, mf_m, mf_u, mf_v,
                                           hfx_z, diss,
                                           mu_turb, dc, tc, tm_arr, grav_gpu, bc_ptr_d, use_most);
                }
                if (l_use_ndiff) {
                    NumericalDiffusion(tbx, start_comp, num_comp, dt, solverChoice.NumDiffCoeff,
                                       new_cons, cell_rhs, mf_u, mf_v, false, false);
                }
            }
            if (l_use_QKE) {
                start_comp = RhoQKE_comp;
                  num_comp = 1;
                if (l_use_terrain) {
                    DiffusionSrcForState_T(tbx, domain, start_comp, num_comp, u, v,
                                           cur_cons, cur_prim, cell_rhs,
                                           diffflux_x, diffflux_y, diffflux_z, z_nd, detJ_arr,
                                           dxInv, SmnSmn_a, mf_m, mf_u, mf_v,
                                           hfx_z, diss,
                                           mu_turb, dc, tc,tm_arr, grav_gpu, bc_ptr_d, use_most);
                } else {
                    DiffusionSrcForState_N(tbx, domain, start_comp, num_comp, u, v,
                                           cur_cons, cur_prim, cell_rhs,
                                           diffflux_x, diffflux_y, diffflux_z,
                                           dxInv, SmnSmn_a, mf_m, mf_u, mf_v,
                                           hfx_z, diss,
                                           mu_turb, dc, tc, tm_arr, grav_gpu, bc_ptr_d, use_most);
                }
                if (l_use_ndiff) {
                    NumericalDiffusion(tbx, start_comp, num_comp, dt, solverChoice.NumDiffCoeff,
                                       new_cons, cell_rhs, mf_u, mf_v, false, false);
                }
            }

            start_comp = RhoScalar_comp;
              num_comp = nvars - start_comp;
            if (l_use_terrain) {
                DiffusionSrcForState_T(tbx, domain, start_comp, num_comp, u, v,
                                       cur_cons, cur_prim, cell_rhs,
                                       diffflux_x, diffflux_y, diffflux_z, z_nd, detJ_arr,
                                       dxInv, SmnSmn_a, mf_m, mf_u, mf_v,
                                       hfx_z, diss,
                                       mu_turb, dc, tc, tm_arr, grav_gpu, bc_ptr_d, use_most);
            } else {
                DiffusionSrcForState_N(tbx, domain, start_comp, num_comp, u, v,
                                       cur_cons, cur_prim, cell_rhs,
                                       diffflux_x, diffflux_y, diffflux_z,
                                       dxInv, SmnSmn_a, mf_m, mf_u, mf_v,
                                       hfx_z, diss,
                                       mu_turb, dc, tc, tm_arr, grav_gpu, bc_ptr_d, use_most);
            }
            if (l_use_ndiff) {
                NumericalDiffusion(tbx, start_comp, num_comp, dt, solverChoice.NumDiffCoeff,
                                   new_cons, cell_rhs, mf_u, mf_v, false, false);
            }
        }

        if (solverChoice.custom_moisture_forcing && (solverChoice.moisture_type != MoistureType::None)) {
            const int n = RhoQ1_comp;
            if (solverChoice.custom_forcing_prim_vars) {
                ParallelFor(tbx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
                {
                    cell_rhs(i,j,k,n) += cur_cons(i,j,k,Rho_comp) * dptr_rhoqt_src[k];
                });
            } else {
                ParallelFor(tbx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
                {
                    cell_rhs(i,j,k,n) += dptr_rhoqt_src[k];
                });
            }
        }

        if (solverChoice.custom_w_subsidence && (solverChoice.moisture_type != MoistureType::None)) {
            const int n = RhoQ1_comp;
            ParallelFor(tbx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                cell_rhs(i,j,k,n) += dptr_wbar_sub[k] *
                    0.5 * (dptr_q_plane(k+1) - dptr_q_plane(k-1)) * dxInv[2];
            });
        }

#if defined(ERF_USE_NETCDF)
        if (moist_set_rhs) {

            // NOTE: We pass the full width into this routine.
            //       For relaxation, the last cell is a halo
            //       cell for the Laplacian. We remove that
            //       cell here if it is present.

            // The width to do RHS augmentation
            if (width > set_width+1) width -= 2;

            // Relaxation constants
            Real F1 = 1./(10.*dt);
            Real F2 = 1./(50.*dt);

            // Domain bounds
            const auto& dom_hi = ubound(domain);
            const auto& dom_lo = lbound(domain);

            // Time interpolation
            Real dT = bdy_time_interval;
            Real time_since_start = new_stage_time - start_bdy_time;
            int n_time = static_cast<int>( time_since_start /  dT);
            Real alpha = (time_since_start - n_time * dT) / dT;
            AMREX_ALWAYS_ASSERT( alpha >= 0. && alpha <= 1.0);
            Real oma   = 1.0 - alpha;

            /*
            // UNIT TEST DEBUG
            oma = 1.0; alpha = 0.0;
            */

            // Boundary data at fixed time intervals
            const auto& bdatxlo_n   = bdy_data_xlo[n_time  ][WRFBdyVars::QV].const_array();
            const auto& bdatxlo_np1 = bdy_data_xlo[n_time+1][WRFBdyVars::QV].const_array();
            const auto& bdatxhi_n   = bdy_data_xhi[n_time  ][WRFBdyVars::QV].const_array();
            const auto& bdatxhi_np1 = bdy_data_xhi[n_time+1][WRFBdyVars::QV].const_array();
            const auto& bdatylo_n   = bdy_data_ylo[n_time  ][WRFBdyVars::QV].const_array();
            const auto& bdatylo_np1 = bdy_data_ylo[n_time+1][WRFBdyVars::QV].const_array();
            const auto& bdatyhi_n   = bdy_data_yhi[n_time  ][WRFBdyVars::QV].const_array();
            const auto& bdatyhi_np1 = bdy_data_yhi[n_time+1][WRFBdyVars::QV].const_array();

            // Get Boxes for interpolation (one halo cell)
            IntVect ng_vect{1,1,0};
            Box gdom(domain); gdom.grow(ng_vect);
            Box bx_xlo, bx_xhi, bx_ylo, bx_yhi;
            compute_interior_ghost_bxs_xy(gdom, domain, width, 0,
                                          bx_xlo, bx_xhi,
                                          bx_ylo, bx_yhi,
                                          ng_vect, true);

            // Temporary FABs for storage (owned/filled on all ranks)
            FArrayBox QV_xlo, QV_xhi, QV_ylo, QV_yhi;
            QV_xlo.resize(bx_xlo,1,The_Async_Arena()); QV_xhi.resize(bx_xhi,1,The_Async_Arena());
            QV_ylo.resize(bx_ylo,1,The_Async_Arena()); QV_yhi.resize(bx_yhi,1,The_Async_Arena());

            // Get Array4 of interpolated values
            Array4<Real> arr_xlo = QV_xlo.array();  Array4<Real> arr_xhi = QV_xhi.array();
            Array4<Real> arr_ylo = QV_ylo.array();  Array4<Real> arr_yhi = QV_yhi.array();

            // NOTE: width is now one less than the total bndy width
            //       if we have a relaxation zone; so we can access
            //       dom_lo/hi +- width. If we do not have a relax
            //       zone, this offset is set_width - 1.
            int offset = set_width - 1;
            if (width > set_width) offset = width;

            // Populate with interpolation (protect from ghost cells)
            ParallelFor(bx_xlo, bx_xhi,
            [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                int ii = std::max(i , dom_lo.x);
                    ii = std::min(ii, dom_lo.x+offset);
                int jj = std::max(j , dom_lo.y);
                    jj = std::min(jj, dom_hi.y);
                arr_xlo(i,j,k) = oma   * bdatxlo_n  (ii,jj,k)
                               + alpha * bdatxlo_np1(ii,jj,k);
            },
            [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                int ii = std::max(i , dom_hi.x-offset);
                    ii = std::min(ii, dom_hi.x);
                int jj = std::max(j , dom_lo.y);
                    jj = std::min(jj, dom_hi.y);
                arr_xhi(i,j,k) = oma   * bdatxhi_n  (ii,jj,k)
                               + alpha * bdatxhi_np1(ii,jj,k);
            });

            ParallelFor(bx_ylo, bx_yhi,
            [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                int ii = std::max(i , dom_lo.x);
                    ii = std::min(ii, dom_hi.x);
                int jj = std::max(j , dom_lo.y);
                    jj = std::min(jj, dom_lo.y+offset);
                arr_ylo(i,j,k) = oma   * bdatylo_n  (ii,jj,k)
                               + alpha * bdatylo_np1(ii,jj,k);
            },
            [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                int ii = std::max(i , dom_lo.x);
                    ii = std::min(ii, dom_hi.x);
                int jj = std::max(j , dom_hi.y-offset);
                    jj = std::min(jj, dom_hi.y);
                arr_yhi(i,j,k) = oma   * bdatyhi_n  (ii,jj,k)
                               + alpha * bdatyhi_np1(ii,jj,k);
            });


            // NOTE: We pass 'old_cons' here since the tendencies are with
            //       respect to the start of the RK integration.

            // Compute RHS in specified region
            //==========================================================
            if (set_width > 0) {
                compute_interior_ghost_bxs_xy(tbx, domain, width, 0,
                                              bx_xlo, bx_xhi,
                                              bx_ylo, bx_yhi);
                wrfbdy_set_rhs_in_spec_region(dt, RhoQ1_comp, 1,
                                              width, set_width, dom_lo, dom_hi,
                                              bx_xlo,  bx_xhi,  bx_ylo,  bx_yhi,
                                              arr_xlo, arr_xhi, arr_ylo, arr_yhi,
                                              old_cons, cell_rhs);
            }


            // NOTE: We pass 'new_cons' here since it has its ghost cells
            //       populated and we are only operating on RhoQv; thus,
            //       we do not need the updated fast quantities.

            // Compute RHS in relaxation region
            //==========================================================
            if (width > set_width) {
                compute_interior_ghost_bxs_xy(tbx, domain, width, set_width,
                                              bx_xlo, bx_xhi,
                                              bx_ylo, bx_yhi);
                wrfbdy_compute_laplacian_relaxation(RhoQ1_comp, 1,
                                                    width, set_width, dom_lo, dom_hi, F1, F2,
                                                    bx_xlo, bx_xhi, bx_ylo, bx_yhi,
                                                    arr_xlo, arr_xhi, arr_ylo, arr_yhi,
                                                    new_cons, cell_rhs);
            }

            /*
            // UNIT TEST DEBUG
            compute_interior_ghost_bxs_xy(tbx, domain, width+1, 0,
                                          bx_xlo, bx_xhi,
                                          bx_ylo, bx_yhi);
            ParallelFor(bx_xlo, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                if (arr_xlo(i,j,k) != new_cons(i,j,k,RhoQ1_comp)) {
                    Print() << "ERROR XLO: " <<  RhoQ1_comp << ' ' << IntVect(i,j,k) << "\n";
                    exit(0);
                }
            });
            ParallelFor(bx_xhi, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                if (arr_xhi(i,j,k) != new_cons(i,j,k,RhoQ1_comp)) {
                    Print() << "ERROR XHI: " << RhoQ1_comp<< ' ' << IntVect(i,j,k) << "\n";
                    exit(0);
                }
            });
            ParallelFor(bx_ylo, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                if (arr_ylo(i,j,k) != new_cons(i,j,k,RhoQ1_comp)) {
                    Print() << "ERROR YLO: " << RhoQ1_comp << ' ' << IntVect(i,j,k) << "\n";
                    exit(0);
                }
            });
            ParallelFor(bx_yhi, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
            {
                if (arr_yhi(i,j,k) != new_cons(i,j,k,RhoQ1_comp)) {
                    Print() << "ERROR YHI: " << RhoQ1_comp << ' ' << IntVect(i,j,k) << "\n";
                    exit(0);
                }
            });
            */
        } // moist_set_rhs
#endif

        // This updates just the "slow" conserved variables
        {
        BL_PROFILE("rhs_post_8");

        if (l_moving_terrain)
        {
            start_comp = RhoScalar_comp;
            num_comp   = S_data[IntVars::cons].nComp() - start_comp;
            ParallelFor(tbx, num_comp,
            [=] AMREX_GPU_DEVICE (int i, int j, int k, int nn) noexcept {
                const int n = start_comp + nn;
                // NOTE: we don't include additional source terms when terrain is moving
                Real temp_val = detJ_arr(i,j,k) * old_cons(i,j,k,n) + dt * detJ_arr(i,j,k) * cell_rhs(i,j,k,n);
                cur_cons(i,j,k,n) = temp_val / detJ_new_arr(i,j,k);
            });

            if (l_use_deardorff) {
              start_comp = RhoKE_comp;
              num_comp   = 1;
              ParallelFor(tbx, num_comp,
              [=] AMREX_GPU_DEVICE (int i, int j, int k, int nn) noexcept {
                const int n = start_comp + nn;
                // NOTE: we don't include additional source terms when terrain is moving
                Real temp_val = detJ_arr(i,j,k) * old_cons(i,j,k,n) + dt * detJ_arr(i,j,k) * cell_rhs(i,j,k,n);
                cur_cons(i,j,k,n) = temp_val / detJ_new_arr(i,j,k);
              });
            }
            if (l_use_QKE) {
              start_comp = RhoQKE_comp;
              num_comp   = 1;
              ParallelFor(tbx, num_comp,
              [=] AMREX_GPU_DEVICE (int i, int j, int k, int nn) noexcept {
                const int n = start_comp + nn;
                // NOTE: we don't include additional source terms when terrain is moving
                Real temp_val = detJ_arr(i,j,k) * old_cons(i,j,k,n) + dt * detJ_arr(i,j,k) * cell_rhs(i,j,k,n);
                cur_cons(i,j,k,n) = temp_val / detJ_new_arr(i,j,k);
                cur_cons(i,j,k,n) = amrex::max(cur_cons(i,j,k,n), 1e-12);
#if 0           // Printing
                if (cur_cons(i,j,k,n) < Real(0.)) {
                    AllPrint() << "MAKING NEGATIVE QKE " << IntVect(i,j,k) << " NEW / OLD " <<
                        cur_cons(i,j,k,n) << " " << old_cons(i,j,k,n) << std::endl;
                    Abort();
                }
#endif
              });
            }
        } else {
            auto const& src_arr = source.const_array(mfi);
            start_comp = RhoScalar_comp;
            num_comp   = nvars - start_comp;
            ParallelFor(tbx, num_comp,
            [=] AMREX_GPU_DEVICE (int i, int j, int k, int nn) noexcept {
                const int n = start_comp + nn;
                cell_rhs(i,j,k,n) += src_arr(i,j,k,n);
                cur_cons(i,j,k,n) = old_cons(i,j,k,n) + dt * cell_rhs(i,j,k,n);
            });

            if (l_use_deardorff) {
              start_comp = RhoKE_comp;
              num_comp = 1;
              Real eps = std::numeric_limits<Real>::epsilon();
              ParallelFor(tbx, num_comp,
              [=] AMREX_GPU_DEVICE (int i, int j, int k, int nn) noexcept {
                const int n = start_comp + nn;
                cell_rhs(i,j,k,n) += src_arr(i,j,k,n);
                cur_cons(i,j,k,n) = old_cons(i,j,k,n) + dt * cell_rhs(i,j,k,n);
                // make sure rho*e is positive
                if (cur_cons(i,j,k,n) < eps) cur_cons(i,j,k,n) = eps;
              });
            }
            if (l_use_QKE) {
              start_comp = RhoQKE_comp;
              num_comp   = 1;
              ParallelFor(tbx, num_comp,
              [=] AMREX_GPU_DEVICE (int i, int j, int k, int nn) noexcept {
                const int n = start_comp + nn;
                cell_rhs(i,j,k,n) += src_arr(i,j,k,n);
                cur_cons(i,j,k,n) = old_cons(i,j,k,n) + dt * cell_rhs(i,j,k,n);
                cur_cons(i,j,k,n) = amrex::max(cur_cons(i,j,k,n), 1e-12);
#if 0           // Printing
                if (cur_cons(i,j,k,n) < Real(0.)) {
                    AllPrint() << "MAKING NEGATIVE QKE " << IntVect(i,j,k) << " NEW / OLD " <<
                        cur_cons(i,j,k,n) << " " << old_cons(i,j,k,n) << std::endl;
                    Abort();
                }
#endif
              });
            }
        }
        } // end profile

        {
        BL_PROFILE("rhs_post_9");
        // This updates all the conserved variables (not just the "slow" ones)
        int   num_comp_all = S_data[IntVars::cons].nComp();
        ParallelFor(tbx, num_comp_all,
        [=] AMREX_GPU_DEVICE (int i, int j, int k, int n) noexcept {
            new_cons(i,j,k,n)  = cur_cons(i,j,k,n);
        });
        } // end profile

        Box xtbx = mfi.nodaltilebox(0);
        Box ytbx = mfi.nodaltilebox(1);
        Box ztbx = mfi.nodaltilebox(2);

        {
        BL_PROFILE("rhs_post_10()");
        ParallelFor(xtbx, ytbx, ztbx,
        [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
            new_xmom(i,j,k) = cur_xmom(i,j,k);
        },
        [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
            new_ymom(i,j,k) = cur_ymom(i,j,k);
        },
        [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
            new_zmom(i,j,k) = cur_zmom(i,j,k);
        });
        } // end profile

        {
        BL_PROFILE("rhs_post_10");
        // We only add to the flux registers in the final RK step
        if (l_reflux && nrk == 2) {
            int strt_comp_reflux = RhoTheta_comp + 1;
            int  num_comp_reflux = nvars - strt_comp_reflux;
            if (level < finest_level) {
                fr_as_crse->CrseAdd(mfi,
                    {{AMREX_D_DECL(&(flux[0]), &(flux[1]), &(flux[2]))}},
                    dx, dt, strt_comp_reflux, strt_comp_reflux, num_comp_reflux, RunOn::Device);
            }
            if (level > 0) {
                fr_as_fine->FineAdd(mfi,
                    {{AMREX_D_DECL(&(flux[0]), &(flux[1]), &(flux[2]))}},
                    dx, dt, strt_comp_reflux, strt_comp_reflux, num_comp_reflux, RunOn::Device);
            }
        } // two-way coupling
        } // end profile
      } // mfi
    } // OMP
}
