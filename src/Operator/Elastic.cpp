#include "Model/Solid/LinearElastic/Isotropic.H"
#include "Model/Solid/LinearElastic/Cubic.H"
#include "Model/Solid/LinearElastic/Laplacian.H"
#include "Model/Solid/LinearElastic/Degradable/Isotropic.H"
#include "Model/Solid/Viscoelastic/Isotropic.H"
#include "Elastic.H"

#include "Numeric/Stencil.H"
namespace Operator
{
template<class T>
Elastic<T>::Elastic (const Vector<Geometry>& a_geom,
		     const Vector<BoxArray>& a_grids,
		     const Vector<DistributionMapping>& a_dmap,
		     const LPInfo& a_info)
{
	BL_PROFILE("Operator::Elastic::Elastic()");

	define(a_geom, a_grids, a_dmap, a_info);
}

template<class T>
Elastic<T>::~Elastic ()
{}

template<class T>
void
Elastic<T>::define (const Vector<Geometry>& a_geom,
		    const Vector<BoxArray>& a_grids,
		    const Vector<DistributionMapping>& a_dmap,
		    const LPInfo& a_info,
		    const Vector<FabFactory<FArrayBox> const*>& a_factory)
{
	BL_PROFILE("Operator::Elastic::define()");

	Operator::define(a_geom,a_grids,a_dmap,a_info,a_factory);

	int model_nghost = 2;

	model.resize(m_num_amr_levels);
	for (int amrlev = 0; amrlev < m_num_amr_levels; ++amrlev)
	{
		model[amrlev].resize(m_num_mg_levels[amrlev]);
		for (int mglev = 0; mglev < m_num_mg_levels[amrlev]; ++mglev)
		{
			model[amrlev][mglev].reset(new MultiTab(amrex::convert(m_grids[amrlev][mglev],
									       amrex::IntVect::TheNodeVector()),
								m_dmap[amrlev][mglev], 1, model_nghost));
		}
	}
}

template <class T>
void
Elastic<T>::SetModel (int amrlev, const amrex::FabArray<amrex::BaseFab<T> >& a_model)
{
	BL_PROFILE("Operator::Elastic::SetModel()");

	int nghost = model[amrlev][0]->nGrow();
	for (MFIter mfi(a_model, false); mfi.isValid(); ++mfi)
	{
		const Box& bx = mfi.validbox();
		amrex::BaseFab<T> &modelfab = (*(model[amrlev][0]))[mfi];
		const amrex::BaseFab<T> &a_modelfab = a_model[mfi];

		AMREX_D_TERM(for (int m1 = bx.loVect()[0]-nghost; m1<=bx.hiVect()[0]+nghost; m1++),
			     for (int m2 = bx.loVect()[1]-nghost; m2<=bx.hiVect()[1]+nghost; m2++),
			     for (int m3 = bx.loVect()[2]-nghost; m3<=bx.hiVect()[2]+nghost; m3++))
		{
			amrex::IntVect m(AMREX_D_DECL(m1,m2,m3));
			modelfab(m) = a_modelfab(m);
		}
	}
}

template<class T>
void
Elastic<T>::Fapply (int amrlev, int mglev, MultiFab& a_f, const MultiFab& a_u) const
{
	BL_PROFILE("Operator::Elastic::Fapply()");

	amrex::Box domain(m_geom[amrlev][mglev].Domain());
	domain.convert(amrex::IntVect::TheNodeVector());

	const Real* DX = m_geom[amrlev][mglev].CellSize();

	for (MFIter mfi(a_f, amrex::TilingIfNotGPU()); mfi.isValid(); ++mfi)
	{
		Box bx = mfi.tilebox();
		bx.grow(1);        // Expand to cover first layer of ghost nodes
		bx = bx & domain;  // Take intersection of box and the problem domain
			
		amrex::Array4<T> const& C                 = (*(model[amrlev][mglev])).array(mfi);
		amrex::Array4<const amrex::Real> const& U = a_u.array(mfi);
		amrex::Array4<amrex::Real> const& F       = a_f.array(mfi);

		const Dim3 lo= amrex::lbound(domain), hi = amrex::ubound(domain);
			
		amrex::ParallelFor (bx,[=] AMREX_GPU_DEVICE(int i, int j, int k) {
					
				Set::Vector f = Set::Vector::Zero();

				bool    AMREX_D_DECL(xmin = (i == lo.x), ymin = (j==lo.y), zmin = (k==lo.z)),
					AMREX_D_DECL(xmax = (i == hi.x), ymax = (j==hi.y), zmax = (k==hi.z));

				// Determine if a special stencil will be necessary for first derivatives
				Numeric::StencilType sten[AMREX_SPACEDIM];
				AMREX_D_TERM(sten[0] = (xmin ? Numeric::StencilType::Hi :
							xmax ? Numeric::StencilType::Lo :
							Numeric::StencilType::Central);,
					     sten[1] = (ymin ? Numeric::StencilType::Hi :
							ymax ? Numeric::StencilType::Lo :
							Numeric::StencilType::Central);,
					     sten[2] = (zmin ? Numeric::StencilType::Hi :
							zmax ? Numeric::StencilType::Lo :
							Numeric::StencilType::Central););

				// The displacement gradient tensor
				Set::Matrix gradu; // gradu(i,j) = u_{i,j)

				// Fill gradu
				for (int p = 0; p < AMREX_SPACEDIM; p++)
				{
 					AMREX_D_TERM(gradu(p,0) = (Numeric::Stencil<Set::Scalar,1,0,0>::D(U,i,j,k,p,DX,sten));,
					 	     gradu(p,1) = (Numeric::Stencil<Set::Scalar,0,1,0>::D(U,i,j,k,p,DX,sten));,
					 	     gradu(p,2) = (Numeric::Stencil<Set::Scalar,0,0,1>::D(U,i,j,k,p,DX,sten)););
				}
					
				// Stress tensor computed using the model fab
				Set::Matrix sig = C(i,j,k)(gradu);

				// Boundary conditions
				amrex::IntVect m(AMREX_D_DECL(i,j,k));
				if (AMREX_D_TERM(xmax || xmin, || ymax || ymin, || zmax || zmin)) 
				{
					for (int p = 0; p < AMREX_SPACEDIM; p++) // iterate over DIMENSIONS
					{
						for (int q = 0; q < AMREX_SPACEDIM; q++) // iterate over FACES
						{
							if (m[q] == domain.loVect()[q])
							{
								if      (m_bc_lo[q][p] == BC::Displacement) f(p) =   U(i,j,k,p);
								else if (m_bc_lo[q][p] == BC::Traction)     f(p) += -sig(p,q);
								else if (m_bc_lo[q][p] == BC::Neumann)      f(p) += -gradu(p,q);
								else Util::Abort(INFO, "Invalid BC");
							}
							if (m[q] == domain.hiVect()[q])
							{
								if      (m_bc_hi[q][p] == BC::Displacement) f(p) = U(i,j,k,p);
								else if (m_bc_hi[q][p] == BC::Traction)     f(p) += +sig(p,q);
								else if (m_bc_hi[q][p] == BC::Neumann)      f(p) += +gradu(p,q);
								else Util::Abort(INFO, "Invalid BC");

							}
						}
					}
				}
				else
				{
					// The gradient of the displacement gradient tensor
					std::array<Set::Matrix,AMREX_SPACEDIM> gradgradu; // gradgradu[k](l,j) = u_{k,lj}

					// Fill gradu and gradgradu
					for (int p = 0; p < AMREX_SPACEDIM; p++)
					{
						// Diagonal terms:
						AMREX_D_TERM(gradgradu[p](0,0) = (Numeric::Stencil<Set::Scalar,2,0,0>::D(U,i,j,k,p,DX));,
							     gradgradu[p](1,1) = (Numeric::Stencil<Set::Scalar,0,2,0>::D(U,i,j,k,p,DX));,
							     gradgradu[p](2,2) = (Numeric::Stencil<Set::Scalar,0,0,2>::D(U,i,j,k,p,DX)););

						// Off-diagonal terms:
						AMREX_D_TERM(,// 2D
							     gradgradu[p](0,1) = (Numeric::Stencil<Set::Scalar,1,1,0>::D(U, i,j,k,p, DX));
							     gradgradu[p](1,0) = gradgradu[p](0,1);
							     ,// 3D
							     gradgradu[p](0,2) = (Numeric::Stencil<Set::Scalar,1,0,1>::D(U, i,j,k,p, DX));
							     gradgradu[p](1,2) = (Numeric::Stencil<Set::Scalar,0,1,1>::D(U, i,j,k,p, DX));
							     gradgradu[p](2,0) = gradgradu[p](0,2);
							     gradgradu[p](2,1) = gradgradu[p](1,2););
					}
	
					//
					// Operator
					//
					// The return value is
					//    f = C(grad grad u) + grad(C)*grad(u)
					// In index notation
					//    f_i = C_{ijkl,j} u_{k,l}  +  C_{ijkl}u_{k,lj}
					//

					T AMREX_D_DECL(Cgrad1 = (Numeric::Stencil<T,1,0,0>::D(C,i,j,k,0,DX,sten)),
						       Cgrad2 = (Numeric::Stencil<T,0,1,0>::D(C,i,j,k,0,DX,sten)),
						       Cgrad3 = (Numeric::Stencil<T,0,0,1>::D(C,i,j,k,0,DX,sten)));

					f = C(i,j,k)(gradgradu) + 
						AMREX_D_TERM(Cgrad1(gradu).col(0),+Cgrad2(gradu).col(1),+Cgrad3(gradu).col(2));
				}
				AMREX_D_TERM(F(i,j,k,0) = f[0];, F(i,j,k,1) = f[1];, F(i,j,k,2) = f[2];);
			});
	}
}



template<class T>
void
Elastic<T>::Diagonal (int amrlev, int mglev, MultiFab& a_diag)
{
	BL_PROFILE("Operator::Elastic::Diagonal()");

	amrex::Box domain(m_geom[amrlev][mglev].Domain());
	domain.convert(amrex::IntVect::TheNodeVector());
	const Real* DX = m_geom[amrlev][mglev].CellSize();
	
	for (MFIter mfi(a_diag, amrex::TilingIfNotGPU()); mfi.isValid(); ++mfi)
	{
		Box bx = mfi.validbox();
		bx.grow(1);        // Expand to cover first layer of ghost nodes
		bx = bx & domain;  // Take intersection of box and the problem domain

		amrex::Array4<T> const& C                 = (*(model[amrlev][mglev])).array(mfi);
		amrex::Array4<amrex::Real> const& diag    = a_diag.array(mfi);

		const Dim3 lo= amrex::lbound(domain), hi = amrex::ubound(domain);
			
		amrex::ParallelFor (bx,[=] AMREX_GPU_DEVICE(int i, int j, int k) {

				bool    AMREX_D_DECL(xmin = (i == lo.x), ymin = (j==lo.y), zmin = (k==lo.z)),
					AMREX_D_DECL(xmax = (i == hi.x), ymax = (j==hi.y), zmax = (k==hi.z));

				Numeric::StencilType sten[AMREX_SPACEDIM];
				AMREX_D_TERM(sten[0] = (xmin ? Numeric::StencilType::Hi :
							xmax ? Numeric::StencilType::Lo :
							Numeric::StencilType::Central);,
					     sten[1] = (ymin ? Numeric::StencilType::Hi :
							ymax ? Numeric::StencilType::Lo :
							Numeric::StencilType::Central);,
					     sten[2] = (zmin ? Numeric::StencilType::Hi :
							zmax ? Numeric::StencilType::Lo :
							Numeric::StencilType::Central););


				Set::Matrix gradu; // gradu(i,j) = u_{i,j)
				std::array<Set::Matrix,AMREX_SPACEDIM> gradgradu; // gradgradu[k](l,j) = u_{k,lj}

				for (int p = 0; p < AMREX_SPACEDIM; p++)
				{
					diag(i,j,k,p) = 0.0;
					for (int q = 0; q < AMREX_SPACEDIM; q++)
					{
						AMREX_D_TERM(gradu(q,0) = ((!xmax ? 0.0 : (p==q ? 1.0 : 0.0)) - (!xmin ? 0.0 : (p==q ? 1.0 : 0.0)))/((xmin || xmax ? 1.0 : 2.0)*DX[0]);,
							     gradu(q,1) = ((!ymax ? 0.0 : (p==q ? 1.0 : 0.0)) - (!ymin ? 0.0 : (p==q ? 1.0 : 0.0)))/((ymin || ymax ? 1.0 : 2.0)*DX[1]);,
							     gradu(q,2) = ((!zmax ? 0.0 : (p==q ? 1.0 : 0.0)) - (!zmin ? 0.0 : (p==q ? 1.0 : 0.0)))/((zmin || zmax ? 1.0 : 2.0)*DX[2]););
			
						AMREX_D_TERM(gradgradu[q](0,0) = (p==q ? -2.0 : 0.0)/DX[0]/DX[0];
							     ,// 2D
							     gradgradu[q](0,1) = 0.0;
							     gradgradu[q](1,0) = 0.0;
							     gradgradu[q](1,1) = (p==q ? -2.0 : 0.0)/DX[1]/DX[1];
							     ,// 3D
							     gradgradu[q](0,2) = 0.0;
							     gradgradu[q](1,2) = 0.0;
							     gradgradu[q](2,0) = 0.0;
							     gradgradu[q](2,1) = 0.0;
							     gradgradu[q](2,2) = (p==q ? -2.0 : 0.0)/DX[2]/DX[2]);
					}

					Set::Matrix sig = C(i,j,k)(gradu);

					amrex::IntVect m(AMREX_D_DECL(i,j,k));
					if (AMREX_D_TERM(xmax || xmin, || ymax || ymin, || zmax || zmin)) 
					{
						for (int q = 0; q < AMREX_SPACEDIM; q++) // iterate over FACES
						{
							if (m[q] == domain.loVect()[q])
							{
								if      (m_bc_lo[q][p] == BC::Displacement) diag(i,j,k,p) += 1.0;
								else if (m_bc_lo[q][p] == BC::Traction)     diag(i,j,k,p) += -sig(p,q);
								else if (m_bc_lo[q][p] == BC::Neumann)      diag(i,j,k,p) += -gradu(p,q);
								else Util::Abort(INFO, "Invalid BC");
							}
							if (m[q] == domain.hiVect()[q])
							{
								if      (m_bc_hi[q][p] == BC::Displacement) diag(i,j,k,p) += 1.0;
								else if (m_bc_hi[q][p] == BC::Traction)     diag(i,j,k,p) += sig(p,q);
								else if (m_bc_hi[q][p] == BC::Neumann)      diag(i,j,k,p) += gradu(p,q);
								else Util::Abort(INFO, "Invalid BC");
							}
						}
					}
					else
					{
						T AMREX_D_DECL(Cgrad1 = (Numeric::Stencil<T,1,0,0>::D(C,i,j,k,0,DX,sten)),
							       Cgrad2 = (Numeric::Stencil<T,0,1,0>::D(C,i,j,k,0,DX,sten)),
							       Cgrad3 = (Numeric::Stencil<T,0,0,1>::D(C,i,j,k,0,DX,sten)));

						Set::Vector f = C(i,j,k)(gradgradu) + 
							AMREX_D_TERM(Cgrad1(gradu).col(0),+Cgrad2(gradu).col(1),+Cgrad3(gradu).col(2));

						diag(i,j,k,p) += f(p);
					}
					if (std::isnan(diag(i,j,k,p))) Util::Abort(INFO,"nan at (", i, ",", j , ",",k);

				}
			});
	}
}


template<class T>
void
Elastic<T>::Error0x (int amrlev, int mglev, MultiFab& R0x, const MultiFab& x) const
{
	BL_PROFILE("Operator::Elastic::Error0x()");
	Util::Message(INFO);

	int ncomp = x.nComp();//getNComp();
	int nghost = x.nGrow();

	if (!m_diagonal_computed)
		Util::Abort(INFO,"Operator::Diagonal() must be called before using normalize");

	amrex::MultiFab D0x(x.boxArray(), x.DistributionMap(), ncomp, nghost);
	amrex::MultiFab AD0x(x.boxArray(), x.DistributionMap(), ncomp, nghost);

	amrex::MultiFab::Copy(D0x,x,0,0,ncomp,nghost); // D0x = x
	amrex::MultiFab::Divide(D0x,*m_diag[amrlev][mglev],0,0,ncomp,0); // D0x = x/diag
	amrex::MultiFab::Copy(AD0x,D0x,0,0,ncomp,nghost); // AD0x = D0x

	Fapply(amrlev,mglev,AD0x,D0x);	// AD0x = A * D0 * x
	
	amrex::MultiFab::Copy(R0x,x,0,0,ncomp,nghost); // R0x = x
	amrex::MultiFab::Subtract(R0x,AD0x,0,0,ncomp,nghost); // R0x = x - AD0x
}


template<class T>
void
Elastic<T>::FFlux (int /*amrlev*/, const MFIter& /*mfi*/,
		const std::array<FArrayBox*,AMREX_SPACEDIM>& sigmafab,
		const FArrayBox& /*ufab*/, const int /*face_only*/) const
{
	BL_PROFILE("Operator::Elastic::FFlux()");
	Util::Message(INFO);
	amrex::BaseFab<amrex::Real> AMREX_D_DECL( &fxfab = *sigmafab[0],
	 					  &fyfab = *sigmafab[1],
	 					  &fzfab = *sigmafab[2] ) ;
	AMREX_D_TERM(fxfab.setVal(0.0);,
	 	     fyfab.setVal(0.0);,
	 	     fzfab.setVal(0.0););

}

template<class T>
void
Elastic<T>::Strain  (int amrlev,
		    amrex::MultiFab& a_eps,
		    const amrex::MultiFab& a_u,
		    bool voigt) const
{
	BL_PROFILE("Operator::Elastic::Strain()");

	const amrex::Real* DX = m_geom[amrlev][0].CellSize();
	amrex::Box domain(m_geom[amrlev][0].Domain());
	domain.convert(amrex::IntVect::TheNodeVector());


	for (MFIter mfi(a_u, amrex::TilingIfNotGPU()); mfi.isValid(); ++mfi)
	{
		const Box& bx = mfi.tilebox();
		amrex::Array4<amrex::Real> const& epsilon = a_eps.array(mfi);
		amrex::Array4<const amrex::Real> const& u = a_u.array(mfi);
		const Dim3 lo= amrex::lbound(domain), hi = amrex::ubound(domain);

		amrex::ParallelFor (bx,[=] AMREX_GPU_DEVICE(int i, int j, int k)
				    {
					    Set::Matrix gradu;

					    bool    AMREX_D_DECL(xmin = (i == lo.x), ymin = (j==lo.y), zmin = (k==lo.z)),
						    AMREX_D_DECL(xmax = (i == hi.x), ymax = (j==hi.y), zmax = (k==hi.z));

					    Numeric::StencilType sten[AMREX_SPACEDIM];
					    AMREX_D_TERM(sten[0] = (xmin ? Numeric::StencilType::Hi :
								    xmax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central);,
							 sten[1] = (ymin ? Numeric::StencilType::Hi :
								    ymax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central);,
							 sten[2] = (zmin ? Numeric::StencilType::Hi :
								    zmax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central););

					    // Fill gradu
					    for (int p = 0; p < AMREX_SPACEDIM; p++)
					    {
						    AMREX_D_TERM(gradu(p,0) = (Numeric::Stencil<Set::Scalar,1,0,0>::D(u, i,j,k,p, DX, sten));,
						    		 gradu(p,1) = (Numeric::Stencil<Set::Scalar,0,1,0>::D(u, i,j,k,p, DX, sten));,
						     		 gradu(p,2) = (Numeric::Stencil<Set::Scalar,0,0,1>::D(u, i,j,k,p, DX, sten)););
					    }

					    Set::Matrix eps = 0.5 * (gradu + gradu.transpose());

					    if (voigt)
					    {
						    AMREX_D_PICK(epsilon(i,j,k,0) = eps(0,0);
								 ,
								 epsilon(i,j,k,0) = eps(0,0); epsilon(i,j,k,1) = eps(1,1); epsilon(i,j,k,2) = eps(0,1); 
								 ,
								 epsilon(i,j,k,0) = eps(0,0); epsilon(i,j,k,1) = eps(1,1); epsilon(i,j,k,2) = eps(2,2); 
								 epsilon(i,j,k,3) = eps(1,2); epsilon(i,j,k,4) = eps(2,0); epsilon(i,j,k,5) = eps(0,1););
					    }
					    else
					    {
						    AMREX_D_PICK(epsilon(i,j,k,0) = eps(0,0);
								 ,
								 epsilon(i,j,k,0) = eps(0,0); epsilon(i,j,k,1) = eps(0,1); 
								 epsilon(i,j,k,2) = eps(1,0); epsilon(i,j,k,3) = eps(1,1);
								 ,
								 epsilon(i,j,k,0) = eps(0,0); epsilon(i,j,k,1) = eps(0,1); epsilon(i,j,k,2) = eps(0,2); 
								 epsilon(i,j,k,3) = eps(1,0); epsilon(i,j,k,4) = eps(1,1); epsilon(i,j,k,5) = eps(1,2); 
								 epsilon(i,j,k,6) = eps(2,0); epsilon(i,j,k,7) = eps(2,1); epsilon(i,j,k,8) = eps(2,2););
					    }
				    });
	}
}


template<class T>
void
Elastic<T>::Stress (int amrlev,
		    amrex::MultiFab& a_sigma,
		    const amrex::MultiFab& a_u,
		    bool voigt) const
{
	BL_PROFILE("Operator::Elastic::Stress()");

	const amrex::Real* DX = m_geom[amrlev][0].CellSize();
	amrex::Box domain(m_geom[amrlev][0].Domain());
	domain.convert(amrex::IntVect::TheNodeVector());

	for (MFIter mfi(a_u, amrex::TilingIfNotGPU()); mfi.isValid(); ++mfi)
	{
		const Box& bx = mfi.tilebox();
		amrex::Array4<T> const& C                 = (*(model[amrlev][0])).array(mfi);
		amrex::Array4<amrex::Real> const& sigma   = a_sigma.array(mfi);
		amrex::Array4<const amrex::Real> const& u = a_u.array(mfi);
		const Dim3 lo= amrex::lbound(domain), hi = amrex::ubound(domain);

		amrex::ParallelFor (bx,[=] AMREX_GPU_DEVICE(int i, int j, int k)
				    {
					    Set::Matrix gradu;

					    bool    AMREX_D_DECL(xmin = (i == lo.x), ymin = (j==lo.y), zmin = (k==lo.z)),
						    AMREX_D_DECL(xmax = (i == hi.x), ymax = (j==hi.y), zmax = (k==hi.z));

					    Numeric::StencilType sten[AMREX_SPACEDIM];
					    AMREX_D_TERM(sten[0] = (xmin ? Numeric::StencilType::Hi :
								    xmax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central);,
							 sten[1] = (ymin ? Numeric::StencilType::Hi :
								    ymax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central);,
							 sten[2] = (zmin ? Numeric::StencilType::Hi :
								    zmax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central););

					    // Fill gradu
					    for (int p = 0; p < AMREX_SPACEDIM; p++)
					    {
						    AMREX_D_TERM(gradu(p,0) = (Numeric::Stencil<Set::Scalar,1,0,0>::D(u, i,j,k,p, DX, sten));,
						     		 gradu(p,1) = (Numeric::Stencil<Set::Scalar,0,1,0>::D(u, i,j,k,p, DX, sten));,
						      		 gradu(p,2) = (Numeric::Stencil<Set::Scalar,0,0,1>::D(u, i,j,k,p, DX, sten)););
					    }
					 
					    Set::Matrix sig = C(i,j,k)(gradu);

					    if (voigt)
					    {
						    AMREX_D_PICK(sigma(i,j,k,0) = sig(0,0);
								 ,
								 sigma(i,j,k,0) = sig(0,0); sigma(i,j,k,1) = sig(1,1); sigma(i,j,k,2) = sig(0,1); 
								 ,
								 sigma(i,j,k,0) = sig(0,0); sigma(i,j,k,1) = sig(1,1); sigma(i,j,k,2) = sig(2,2); 
								 sigma(i,j,k,3) = sig(1,2); sigma(i,j,k,4) = sig(2,0); sigma(i,j,k,5) = sig(0,1););
					    }
					    else
					    {
						    AMREX_D_PICK(sigma(i,j,k,0) = sig(0,0);
								 ,
								 sigma(i,j,k,0) = sig(0,0); sigma(i,j,k,1) = sig(0,1); 
								 sigma(i,j,k,2) = sig(1,0); sigma(i,j,k,3) = sig(1,1);
								 ,
								 sigma(i,j,k,0) = sig(0,0); sigma(i,j,k,1) = sig(0,1); sigma(i,j,k,2) = sig(0,2); 
								 sigma(i,j,k,3) = sig(1,0); sigma(i,j,k,4) = sig(1,1); sigma(i,j,k,5) = sig(1,2); 
								 sigma(i,j,k,6) = sig(2,0); sigma(i,j,k,7) = sig(2,1); sigma(i,j,k,8) = sig(2,2););
					    }
				    });
	}
}


template<class T>
void
Elastic<T>::Energy (int amrlev,
		    amrex::MultiFab& a_energy,
		    const amrex::MultiFab& a_u) const
{
	BL_PROFILE("Operator::Elastic::Energy()");

	amrex::Box domain(m_geom[amrlev][0].Domain());
	domain.convert(amrex::IntVect::TheNodeVector());

	const amrex::Real* DX = m_geom[amrlev][0].CellSize();

	for (MFIter mfi(a_u, amrex::TilingIfNotGPU()); mfi.isValid(); ++mfi)
	{
		const Box& bx = mfi.tilebox();
		amrex::Array4<T> const& C                  = (*(model[amrlev][0])).array(mfi);
		amrex::Array4<amrex::Real> const& energy   = a_energy.array(mfi);
		amrex::Array4<const amrex::Real> const& u  = a_u.array(mfi);
		const Dim3 lo= amrex::lbound(domain), hi   = amrex::ubound(domain);

		amrex::ParallelFor (bx,[=] AMREX_GPU_DEVICE(int i, int j, int k)
				    {
					    Set::Matrix gradu;

					    bool    AMREX_D_DECL(xmin = (i == lo.x), ymin = (j==lo.y), zmin = (k==lo.z)),
						    AMREX_D_DECL(xmax = (i == hi.x), ymax = (j==hi.y), zmax = (k==hi.z));

					    Numeric::StencilType sten[AMREX_SPACEDIM];
					    AMREX_D_TERM(sten[0] = (xmin ? Numeric::StencilType::Hi :
								    xmax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central);,
							 sten[1] = (ymin ? Numeric::StencilType::Hi :
								    ymax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central);,
							 sten[2] = (zmin ? Numeric::StencilType::Hi :
								    zmax ? Numeric::StencilType::Lo :
								    Numeric::StencilType::Central););

					    // Fill gradu
					    for (int p = 0; p < AMREX_SPACEDIM; p++)
					    {
						    AMREX_D_TERM(gradu(p,0) = (Numeric::Stencil<Set::Scalar,1,0,0>::D(u, i,j,k,p, DX, sten));,
						     		 gradu(p,1) = (Numeric::Stencil<Set::Scalar,0,1,0>::D(u, i,j,k,p, DX, sten));,
						     		 gradu(p,2) = (Numeric::Stencil<Set::Scalar,0,0,1>::D(u, i,j,k,p, DX, sten)););
					    }
					 
					    Set::Matrix sig = C(i,j,k)(gradu);

					    energy(i,j,k) = (gradu.transpose() * sig).trace();
				    });
	}
}



template<class T>
void
Elastic<T>::averageDownCoeffs ()
{
	BL_PROFILE("Elastic::averageDownCoeffs()");
	
	// for (int amrlev = 0; amrlev < m_num_amr_levels; ++amrlev)
	// {
	// 	for (int mglev = 0; mglev < m_num_mg_levels[amrlev]; ++mglev)
	// 	{
	// 		///\todo replace number of ghost cells with 0
	// 		///\todo I think we can erase this section.
	// 		model[amrlev][mglev].reset(new amrex::FabArray<amrex::BaseFab<T> >(m_grids[amrlev][mglev], m_dmap[amrlev][mglev], 1, 1));
	// 	}
	// }

	for (int amrlev = m_num_amr_levels-1; amrlev > 0; --amrlev)
	{
		averageDownCoeffsSameAmrLevel(amrlev);
	 	averageDownCoeffsToCoarseAmrLevel(amrlev);
	}

	averageDownCoeffsSameAmrLevel(0);
	for (int amrlev = 0; amrlev < m_num_amr_levels; ++amrlev)
	{
	 	for (int mglev = 0; mglev < m_num_mg_levels[amrlev]; ++mglev)
	 	{
	 		if (model[amrlev][mglev]) {
	 			FillBoundaryCoeff(*model[amrlev][mglev], m_geom[amrlev][mglev]);
	 		}
	 	}
	}
}

template<class T>
void
Elastic<T>::averageDownCoeffsToCoarseAmrLevel (int /*flev*/) 
{
	/*
	BL_PROFILE("Operator::Elastic::averageDownCoeffsToCoarseAmrLevel()");

	//const int mglev = 0;

	// const int idim = 0;  // other dimensions are just aliases

	// // amrex::average_down(*m_sigma[flev][mglev][idim], *m_sigma[flev-1][mglev][idim], 0, 1,
	// // 		    m_amr_ref_ratio[flev-1]);

	int ncomp = AMREX_SPACEDIM;
	MultiTab finemt;
	MultiTab crsemt;
	
	

	amrex::BoxArray fineBACoarsened = finemt.boxArray(); fineBACoarsened.coarsen(m_amr_ref_ratio[flev-1]);


	//MultiFab crse_S_fine(crse_S_fine_BA, S_fine.DistributionMap(), ncomp, 0, MFInfo(), FArrayBoxFactory());
        if ((fineBACoarsened == crsemt.boxArray()) &&
	    (finemt.DistributionMap() == crsemt.DistributionMap()))
	{
		Util::Message(INFO); // this never seems to happen
	}
	else
	{
		//Util::Abort(INFO, "difference in box arrays");

		MultiTab finemtcoarsened(fineBACoarsened, finemt.DistributionMap(), ncomp, 0);

// #ifdef _OPENMP
// #pragma omp parallel
// #endif
		for (MFIter mfi(finemtcoarsened,true); mfi.isValid(); ++mfi)
		{
			//  NOTE: The tilebox is defined at the coarse level.
			const Box& bx = mfi.tilebox();
                
			const TArrayBox &fine = finemtcoarsened[mfi];
			TArrayBox &crse = crsemt[mfi];

			//  NOTE: We copy from component scomp of the fine fab into component 0 of the crse fab
			//        because the crse fab is a temporary which was made starting at comp 0, it is
			//        not part of the actual crse multifab which came in.


			AMREX_D_TERM(for (int m1 = bx.loVect()[0]-1; m1<=bx.hiVect()[0]+1; m1++),
				     for (int m2 = bx.loVect()[1]-1; m2<=bx.hiVect()[1]+1; m2++),
				     for (int m3 = bx.loVect()[2]-1; m3<=bx.hiVect()[2]+1; m3++))
			{
				amrex::IntVect m_crse(AMREX_D_DECL(m1,m2,m3));
				amrex::IntVect m_fine(AMREX_D_DECL(m1*2,m2*2,m3*2));

				crse(m_crse) = fine(m_fine);
			}
		}
            
		crsemt.copy(finemtcoarsened,0,0,ncomp);

	}
	*/
}

template<class T>
void
Elastic<T>::averageDownCoeffsSameAmrLevel (int amrlev)
{
	BL_PROFILE("Elastic::averageDownCoeffsSameAmrLevel()");

 	for (int mglev = 1; mglev < m_num_mg_levels[amrlev]; ++mglev)
 	{
		amrex::Box cdomain(m_geom[amrlev][mglev].Domain());
		cdomain.convert(amrex::IntVect::TheNodeVector());
		amrex::Box fdomain(m_geom[amrlev][mglev-1].Domain());
		fdomain.convert(amrex::IntVect::TheNodeVector());

		MultiTab& crse = *model[amrlev][mglev];
		MultiTab& fine = *model[amrlev][mglev-1];
		
		amrex::BoxArray crseba = crse.boxArray();
		amrex::BoxArray fineba = fine.boxArray();
		
		BoxArray newba = crseba;
		newba.refine(2);
		MultiTab fine_on_crseba;
		fine_on_crseba.define(newba,crse.DistributionMap(),1,4);
		fine_on_crseba.ParallelCopy(fine,0,0,1,2,4,m_geom[amrlev][mglev].periodicity());

		for (MFIter mfi(crse, amrex::TilingIfNotGPU()); mfi.isValid(); ++mfi)
		{
			Box bx = mfi.tilebox();
			bx.grow(2);
			bx = bx & cdomain;

			amrex::Array4<const T> const& fdata = fine_on_crseba.array(mfi);
			amrex::Array4<T> const& cdata       = crse.array(mfi);

			const Dim3 lo= amrex::lbound(bx), hi = amrex::ubound(bx);

			// I,J,K == coarse coordinates
			// i,j,k == fine coordinates
			amrex::ParallelFor (bx,[=] AMREX_GPU_DEVICE(int I, int J, int K) {
					int i=2*I, j=2*J, k=2*K;

					if ((I == lo.x || I == hi.x) &&
					    (J == lo.y || J == hi.y) &&
					    (K == lo.z || K == hi.z)) // Corner
						cdata(I,J,K) = fdata(i,j,k);
					else if ((J == lo.y || J == hi.y) &&
						 (K == lo.z || K == hi.z)) // X edge
						cdata(I,J,K) = fdata(i-1,j,k)*0.25 + fdata(i,j,k)*0.5 + fdata(i+1,j,k)*0.25;
					else if ((K == lo.z || K == hi.z) &&
					 	 (I == lo.x || I == hi.x)) // Y edge
					 	cdata(I,J,K) = fdata(i,j-1,k)*0.25 + fdata(i,j,k)*0.5 + fdata(i,j+1,k)*0.25;
					else if ((I == lo.x || I == hi.x) &&
					 	 (J == lo.y || J == hi.y)) // Z edge
					 	cdata(I,J,K) = fdata(i,j,k-1)*0.25 + fdata(i,j,k)*0.5 + fdata(i,j,k+1)*0.25;
					else if (I == lo.x || I == hi.x) // X face
					 	cdata(I,J,K) =
					 		(  fdata(i,j-1,k-1)     + fdata(i,j,k-1)*2.0 + fdata(i,j+1,k-1)
					 		 + fdata(i,j-1,k  )*2.0 + fdata(i,j,k  )*4.0 + fdata(i,j+1,k  )*2.0 
					 		 + fdata(i,j-1,k+1)     + fdata(i,j,k+1)*2.0 + fdata(i,j+1,k+1)    )/16.0;
					else if (J == lo.y || J == hi.y) // Y face
					 	cdata(I,J,K) =
					 		(  fdata(i-1,j,k-1)     + fdata(i-1,j,k)*2.0 + fdata(i-1,j,k+1)
					 		 + fdata(i  ,j,k-1)*2.0 + fdata(i  ,j,k)*4.0 + fdata(i  ,j,k+1)*2.0 
					 		 + fdata(i+1,j,k-1)     + fdata(i+1,j,k)*2.0 + fdata(i+1,j,k+1))/16.0;
					 else if (K == lo.z || K == hi.z) // Z face
					 	cdata(I,J,K) =
					 		(  fdata(i-1,j-1,k)     + fdata(i,j-1,k)*2.0 + fdata(i+1,j-1,k)
					 		 + fdata(i-1,j  ,k)*2.0 + fdata(i,j  ,k)*4.0 + fdata(i+1,j  ,k)*2.0 
					 		 + fdata(i-1,j+1,k)     + fdata(i,j+1,k)*2.0 + fdata(i+1,j+1,k))/16.0;
					 else // Interior
						 cdata(I,J,K) =
							 (fdata(i-1,j-1,k-1) + fdata(i-1,j-1,k+1) + fdata(i-1,j+1,k-1) + fdata(i-1,j+1,k+1) +
							  fdata(i+1,j-1,k-1) + fdata(i+1,j-1,k+1) + fdata(i+1,j+1,k-1) + fdata(i+1,j+1,k+1)) / 64.0
							 +
							 (fdata(i,j-1,k-1) + fdata(i,j-1,k+1) + fdata(i,j+1,k-1) + fdata(i,j+1,k+1) +
							  fdata(i-1,j,k-1) + fdata(i+1,j,k-1) + fdata(i-1,j,k+1) + fdata(i+1,j,k+1) +
							  fdata(i-1,j-1,k) + fdata(i-1,j+1,k) + fdata(i+1,j-1,k) + fdata(i+1,j+1,k)) / 32.0
							 +
							 (fdata(i-1,j,k) + fdata(i,j-1,k) + fdata(i,j,k-1) +
							  fdata(i+1,j,k) + fdata(i,j+1,k) + fdata(i,j,k+1)) / 16.0
							 +
							 fdata(i,j,k) / 8.0;
				});
		}
		//fine_on_crseba.ParallelCopy(fine,0,0,1,2,4,m_geom[amrlev][mglev].periodicity());
		//fine.ParallelCopy(fine_on_crseba,0,0,1,2,4,m_geom[amrlev][mglev].periodicity());
		FillBoundaryCoeff(crse,m_geom[amrlev][mglev+1]);
	}
}

template<class T>
void
Elastic<T>::FillBoundaryCoeff (MultiTab& sigma, const Geometry& geom)
{
	BL_PROFILE("Elastic::FillBoundaryCoeff()");

	//sigma.FillBoundary(geom.periodicity());
	for (int i = 0; i < 2; i++)
	{
		MultiTab & mf = sigma;
		mf.FillBoundary(geom.periodicity());
		const int ncomp = mf.nComp();
		const int ng1 = 1;
		const int ng2 = 2;
		MultiTab tmpmf(mf.boxArray(), mf.DistributionMap(), ncomp, ng1);
		//MultiTab::Copy(tmpmf, mf, 0, 0, ncomp, ng1); 
	  	tmpmf.copy(mf,0,0,ncomp,ng2,ng1,geom.periodicity());

		mf.ParallelCopy   (tmpmf, 0, 0, ncomp, ng1, ng2, geom.periodicity());
	}


	//const Box& domain = geom.Domain();

// #ifdef _OPENMP
// #pragma omp parallel
// #endif
	// for (MFIter mfi(sigma, MFItInfo().SetDynamic(true)); mfi.isValid(); ++mfi)
	// {
	// 	if (!domain.contains(mfi.fabbox()))
	// 	{
			

	// 	}
	// }
	///////Util::Warning(INFO, "FillBoundaryCoeff not fully implemented");
}



template class Elastic<Model::Solid::LinearElastic::Isotropic>;
template class Elastic<Model::Solid::LinearElastic::Cubic>;
template class Elastic<Model::Solid::LinearElastic::Laplacian>;
template class Elastic<Model::Solid::LinearElastic::Degradable::Isotropic>;
template class Elastic<Model::Solid::Viscoelastic::Isotropic>;
}

